#include "vsr/replica.hpp"

#include "vsr/assert.hpp"
#include "vsr/checksum.hpp"
#include "vsr/internal/bytes.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace vsr
{

namespace
{

// Entries that one RequestPrepare returns. It bounds the work for one message.
constexpr std::uint64_t kRepairBatchOps = 8;
// Operations that one call executes, so that a long catch-up does not hold
// the host for an unbounded time.
constexpr std::uint32_t kExecuteBatchOps = 64;

[[nodiscard]] std::span<const std::byte> AsBytes(const EntryHeader &p_header) noexcept
{
  return std::as_bytes(std::span<const EntryHeader, 1>{&p_header, 1});
}

} // namespace

std::uint64_t StorageBytes(const Config &p_config)
{
  return internal::MakeLayout(p_config).total_bytes_;
}

std::expected<void, OpenError> Format(Storage &p_storage, const Config &p_config)
{
  if (!ValidateConfig(p_config))
  {
    return std::unexpected(OpenError::BadConfig);
  }
  // Entries of an earlier replica could link to the new root. Erase the
  // slot of operation 1, so that recovery finds an empty log.
  const auto layout = internal::MakeLayout(p_config);
  const std::array<std::byte, kEntryHeaderBytes> zeros{};
  if (!p_storage.Write(layout.SlotOffset(1), zeros))
  {
    return std::unexpected(OpenError::StorageFault);
  }
  // Two writes replace both copies, also a copy with a higher sequence.
  auto superblock = internal::MakeSuperblock(p_config);
  for (std::uint64_t copy_index = 0; copy_index < internal::kSuperblockCopyCount; ++copy_index)
  {
    if (!internal::WriteSuperblock(p_storage, superblock))
    {
      return std::unexpected(OpenError::StorageFault);
    }
  }
  return {};
}

Replica::Replica(const Config &p_config, Storage &p_storage, MessageBus &p_bus,
                 StateMachine &p_state_machine, const internal::Superblock &p_superblock)
    : config_(p_config), layout_(internal::MakeLayout(p_config)), storage_(&p_storage),
      bus_(&p_bus), state_machine_(&p_state_machine), superblock_(p_superblock),
      journal_(&p_storage, layout_), clients_(p_config.clients_max_, p_config.body_bytes_max_),
      message_buffer_(MessageBytesMax(p_config.body_bytes_max_)),
      body_buffer_(p_config.body_bytes_max_), reply_buffer_(p_config.body_bytes_max_),
      snapshot_buffer_(internal::SnapshotBlobBytesMax(p_config))
{
}

std::expected<Replica, OpenError> Replica::Open(const Config &p_config, Storage &p_storage,
                                                MessageBus &p_bus, StateMachine &p_state_machine)
{
  if (!ValidateConfig(p_config))
  {
    return std::unexpected(OpenError::BadConfig);
  }
  const auto superblock = internal::ReadSuperblock(p_storage);
  if (!superblock)
  {
    return std::unexpected(superblock.error() == internal::SuperblockError::Io
                               ? OpenError::StorageFault
                               : OpenError::NotFormatted);
  }
  if (!internal::MatchesConfig(*superblock, p_config))
  {
    return std::unexpected(OpenError::ConfigMismatch);
  }
  Replica replica{p_config, p_storage, p_bus, p_state_machine, *superblock};
  if (const auto recovered = replica.Recover(); !recovered)
  {
    return std::unexpected(recovered.error());
  }
  return replica;
}

std::expected<void, OpenError> Replica::Recover()
{
  view_ = superblock_.view_;
  log_view_ = superblock_.log_view_;
  if (superblock_.checkpoint_op_ > 0)
  {
    if (superblock_.checkpoint_bytes_ > snapshot_buffer_.size())
    {
      return std::unexpected(OpenError::CorruptSnapshot);
    }
    const auto blob = std::span{snapshot_buffer_}.first(superblock_.checkpoint_bytes_);
    if (!storage_->Read(layout_.SnapshotOffset(superblock_.checkpoint_slot_), blob))
    {
      return std::unexpected(OpenError::StorageFault);
    }
    if (Checksum(blob) != superblock_.checkpoint_checksum_)
    {
      return std::unexpected(OpenError::CorruptSnapshot);
    }
    if (!LoadSnapshotBlob(blob))
    {
      return std::unexpected(OpenError::RestoreFailed);
    }
  }
  if (!journal_.Recover(superblock_.checkpoint_op_, superblock_.checkpoint_entry_checksum_))
  {
    return std::unexpected(OpenError::StorageFault);
  }
  commit_min_ = superblock_.checkpoint_op_;
  commit_max_ = commit_min_;
  op_synced_ = journal_.Head();
  if (journal_.Head() > journal_.Floor())
  {
    timestamp_last_ns_ =
        std::max(timestamp_last_ns_, journal_.HeaderAt(journal_.Head()).timestamp_ns_);
  }
  // Replicas must not time out at the same tick, or view changes collide.
  random_state_ = config_.cluster_ ^ (std::uint64_t{config_.replica_} + 1) * 0x9E3779B97F4A7C15ULL;
  retry_deadline_tick_ = config_.retry_ticks_;

  const bool log_is_current = log_view_ == view_;
  if (log_is_current && PrimaryOf(view_) != config_.replica_)
  {
    status_ = Status::Normal;
    primary_deadline_tick_ = TimeoutWithJitter(config_.primary_timeout_ticks_);
    return {};
  }
  if (log_is_current)
  {
    // This replica was the primary. A backup can hold an entry that this
    // replica sent but did not make durable, and a quorum of backups can
    // commit it. The primary cannot continue with a shorter log, so it asks
    // for a new view.
    StartViewChange(view_ + 1);
  }
  else
  {
    BeginViewChangeRound();
  }
  if (status_ == Status::Halted)
  {
    return std::unexpected(OpenError::StorageFault);
  }
  return {};
}

void Replica::Halt(HaltReason p_reason)
{
  if (status_ != Status::Halted)
  {
    status_ = Status::Halted;
    halt_reason_ = p_reason;
  }
}

bool Replica::PersistSuperblock()
{
  superblock_.view_ = view_;
  superblock_.log_view_ = log_view_;
  if (!internal::WriteSuperblock(*storage_, superblock_))
  {
    Halt(HaltReason::StorageFault);
    return false;
  }
  return true;
}

bool Replica::SyncJournal()
{
  if (!journal_.Sync())
  {
    Halt(HaltReason::StorageFault);
    return false;
  }
  op_synced_ = journal_.Head();
  return true;
}

std::uint64_t Replica::TimeoutWithJitter(std::uint32_t p_ticks)
{
  // SplitMix64.
  random_state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t mixed = random_state_;
  mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
  mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
  mixed ^= mixed >> 31;
  return ticks_ + p_ticks + mixed % p_ticks;
}

// --- Sending ---------------------------------------------------------------

Header Replica::MakeHeader(Command p_command) const
{
  Header header;
  header.command_ = p_command;
  header.cluster_ = config_.cluster_;
  header.view_ = view_;
  header.replica_ = config_.replica_;
  return header;
}

Header Replica::MakeHeadHeader(Command p_command) const
{
  Header header = MakeHeader(p_command);
  header.op_ = journal_.Head();
  header.log_checksum_ = journal_.HeadChecksum();
  header.commit_ = commit_max_;
  header.checkpoint_op_ = superblock_.checkpoint_op_;
  header.checkpoint_checksum_ = superblock_.checkpoint_checksum_;
  header.view_start_op_ = view_start_op_;
  return header;
}

void Replica::Send(std::uint8_t p_replica, Header p_header, std::span<const std::byte> p_body_first,
                   std::span<const std::byte> p_body_second)
{
  VSR_ASSERT(p_replica < config_.replica_count_);
  VSR_ASSERT(p_replica != config_.replica_);
  const std::size_t message_bytes =
      EncodeMessage(p_header, p_body_first, p_body_second, message_buffer_);
  bus_->SendToReplica(p_replica, std::span{message_buffer_}.first(message_bytes));
}

void Replica::Broadcast(const Header &p_header, std::span<const std::byte> p_body_first,
                        std::span<const std::byte> p_body_second)
{
  if (config_.replica_count_ == 1)
  {
    return;
  }
  const std::size_t message_bytes =
      EncodeMessage(p_header, p_body_first, p_body_second, message_buffer_);
  const auto message = std::span<const std::byte>{message_buffer_}.first(message_bytes);
  for (std::uint8_t replica = 0; replica < config_.replica_count_; ++replica)
  {
    if (replica != config_.replica_)
    {
      bus_->SendToReplica(replica, message);
    }
  }
}

void Replica::SendPrepare(std::uint8_t p_replica, std::uint64_t p_op)
{
  const EntryHeader entry_header = journal_.HeaderAt(p_op);
  const auto body = journal_.ReadBody(p_op, body_buffer_);
  if (!body)
  {
    Halt(body.error() == internal::JournalReadError::Io ? HaltReason::StorageFault
                                                        : HaltReason::CorruptEntry);
    return;
  }
  Header header = MakeHeader(Command::Prepare);
  header.commit_ = commit_max_;
  header.checkpoint_op_ = superblock_.checkpoint_op_;
  header.checkpoint_checksum_ = superblock_.checkpoint_checksum_;
  header.view_start_op_ = view_start_op_;
  Send(p_replica, header, AsBytes(entry_header), *body);
}

void Replica::SendPrepareOk()
{
  VSR_ASSERT(status_ == Status::Normal);
  VSR_ASSERT(PrimaryOf(view_) != config_.replica_);
  if (op_synced_ < journal_.Floor())
  {
    return;
  }
  Header header = MakeHeader(Command::PrepareOk);
  header.op_ = op_synced_;
  header.log_checksum_ = journal_.ChecksumAt(op_synced_);
  Send(PrimaryOf(view_), header);
  prepare_ok_sent_op_ = op_synced_;
}

void Replica::SendReply(const internal::ClientEntry &p_entry)
{
  Header header = MakeHeader(Command::Reply);
  header.client_ = p_entry.client_;
  header.request_ = p_entry.request_;
  header.op_ = p_entry.op_;
  const std::size_t message_bytes =
      EncodeMessage(header, clients_.Reply(p_entry), {}, message_buffer_);
  bus_->SendToClient(p_entry.client_, std::span{message_buffer_}.first(message_bytes));
}

void Replica::SendEviction(std::uint64_t p_client)
{
  Header header = MakeHeader(Command::Eviction);
  header.client_ = p_client;
  const std::size_t message_bytes = EncodeMessage(header, {}, {}, message_buffer_);
  bus_->SendToClient(p_client, std::span{message_buffer_}.first(message_bytes));
}

void Replica::RequestPrepare(std::uint64_t p_op)
{
  // One request returns a batch. Do not ask again while the batch can still
  // arrive, or each entry of the batch causes a new batch.
  const bool outstanding = request_prepare_op_ != 0 && p_op >= request_prepare_op_ &&
                           p_op < request_prepare_op_ + kRepairBatchOps &&
                           ticks_ < request_prepare_tick_ + config_.retry_ticks_;
  if (outstanding)
  {
    return;
  }
  request_prepare_op_ = p_op;
  request_prepare_tick_ = ticks_;
  Header header = MakeHeader(Command::RequestPrepare);
  header.op_ = p_op;
  Send(Fetching() ? fetch_source_ : PrimaryOf(view_), header);
}

void Replica::RequestSnapshotChunk()
{
  VSR_ASSERT(sync_.active_);
  Header header = MakeHeader(Command::RequestSnapshot);
  header.checkpoint_op_ = sync_.checkpoint_op_;
  header.offset_ = sync_.received_bytes_;
  Send(PrimaryOf(view_), header);
}

void Replica::SendSnapshotChunk(std::uint8_t p_replica, std::uint64_t p_offset)
{
  if (superblock_.checkpoint_op_ == 0 || p_offset >= superblock_.checkpoint_bytes_)
  {
    return;
  }
  const std::uint64_t chunk_bytes =
      std::min<std::uint64_t>(config_.body_bytes_max_, superblock_.checkpoint_bytes_ - p_offset);
  const auto chunk = std::span{body_buffer_}.first(chunk_bytes);
  if (!storage_->Read(layout_.SnapshotOffset(superblock_.checkpoint_slot_) + p_offset, chunk))
  {
    Halt(HaltReason::StorageFault);
    return;
  }
  Header header = MakeHeadHeader(Command::Snapshot);
  header.log_checksum_ = superblock_.checkpoint_entry_checksum_;
  header.total_bytes_ = superblock_.checkpoint_bytes_;
  header.offset_ = p_offset;
  Send(p_replica, header, chunk);
}

// --- Input -----------------------------------------------------------------

void Replica::OnMessage(std::span<const std::byte> p_message)
{
  if (status_ == Status::Halted)
  {
    return;
  }
  const auto message = DecodeMessage(p_message);
  if (!message || message->header_.cluster_ != config_.cluster_)
  {
    stats_.messages_invalid_ += 1;
    return;
  }
  const Header &header = message->header_;
  if (header.command_ == Command::Request)
  {
    OnRequest(*message);
    return;
  }
  if (header.replica_ >= config_.replica_count_ || header.replica_ == config_.replica_)
  {
    stats_.messages_invalid_ += 1;
    return;
  }
  switch (header.command_)
  {
  case Command::Prepare:
    OnPrepare(*message);
    return;
  case Command::PrepareOk:
    OnPrepareOk(header);
    return;
  case Command::Commit:
  case Command::StartView:
    OnCommit(header);
    return;
  case Command::StartViewChange:
    OnStartViewChange(header);
    return;
  case Command::DoViewChange:
    OnDoViewChange(header);
    return;
  case Command::RequestPrepare:
    OnRequestPrepare(header);
    return;
  case Command::RequestSnapshot:
    OnRequestSnapshot(header);
    return;
  case Command::Snapshot:
    OnSnapshot(*message);
    return;
  case Command::Reserved:
  case Command::Request:
  case Command::Reply:
  case Command::Eviction:
    stats_.messages_invalid_ += 1;
    return;
  }
}

SubmitResult Replica::Submit(std::uint64_t p_client, std::uint64_t p_request,
                             std::span<const std::byte> p_body)
{
  if (status_ == Status::Halted)
  {
    return SubmitResult::Invalid;
  }
  return HandleRequest(p_client, p_request, p_body);
}

void Replica::OnRequest(const Message &p_message)
{
  const Header &header = p_message.header_;
  if (IsPrimary())
  {
    static_cast<void>(HandleRequest(header.client_, header.request_, p_message.body_));
    return;
  }
  // Send the request on once. Two replicas with different views could
  // otherwise send it to each other without end.
  const bool view_is_open = status_ == Status::Normal || status_ == Status::Joining;
  if (!view_is_open || header.forwarded_ != 0 || p_message.body_.size() > config_.body_bytes_max_)
  {
    return;
  }
  Header forward = header;
  forward.forwarded_ = 1;
  Send(PrimaryOf(view_), forward, p_message.body_);
}

SubmitResult Replica::HandleRequest(std::uint64_t p_client, std::uint64_t p_request,
                                    std::span<const std::byte> p_body)
{
  if (p_client == 0 || p_body.size() > config_.body_bytes_max_)
  {
    return SubmitResult::Invalid;
  }
  if (!IsPrimary())
  {
    return SubmitResult::NotPrimary;
  }
  const bool is_registration = p_request == 0;
  if (is_registration && !p_body.empty())
  {
    return SubmitResult::Invalid;
  }
  const auto *const session = clients_.Find(p_client);
  if (session != nullptr)
  {
    if (p_request == session->request_)
    {
      SendReply(*session);
    }
    if (p_request <= session->request_)
    {
      return SubmitResult::Duplicate;
    }
  }
  // The client table shows only executed requests. The same request, or
  // the registration of the client, can be in the log and not yet executed.
  bool pending = false;
  for (std::uint64_t pending_op = commit_min_ + 1; pending_op <= journal_.Head(); ++pending_op)
  {
    const auto &entry = journal_.HeaderAt(pending_op);
    if (entry.client_ == p_client)
    {
      pending = true;
      if (entry.request_ >= p_request)
      {
        return SubmitResult::Duplicate;
      }
    }
  }
  if (session == nullptr && !is_registration)
  {
    // Without a session there is no record of what this client already
    // executed, so the request could execute a second time. The client
    // must register again. While its registration is in the log, it waits.
    if (pending)
    {
      return SubmitResult::Busy;
    }
    SendEviction(p_client);
    return SubmitResult::NoSession;
  }
  const std::uint64_t op = journal_.Head() + 1;
  VSR_ASSERT(commit_max_ <= journal_.Head());
  const bool pipeline_full = journal_.Head() - commit_max_ >= config_.pipeline_max_;
  const bool log_full = op > superblock_.checkpoint_op_ + config_.log_slot_count_;
  if (pipeline_full || log_full)
  {
    stats_.requests_busy_ += 1;
    return SubmitResult::Busy;
  }

  EntryHeader entry_header;
  entry_header.parent_ = journal_.HeadChecksum();
  entry_header.cluster_ = config_.cluster_;
  entry_header.client_ = p_client;
  entry_header.request_ = p_request;
  entry_header.op_ = op;
  // Strictly increasing, also when the wall clock goes back.
  entry_header.timestamp_ns_ = std::max(realtime_ns_, timestamp_last_ns_ + 1);
  entry_header.view_ = view_;
  SealEntry(entry_header, p_body);
  if (!AppendEntry(Entry{entry_header, p_body}))
  {
    return SubmitResult::Invalid;
  }
  Header header = MakeHeader(Command::Prepare);
  header.commit_ = commit_max_;
  header.checkpoint_op_ = superblock_.checkpoint_op_;
  header.checkpoint_checksum_ = superblock_.checkpoint_checksum_;
  header.view_start_op_ = view_start_op_;
  Broadcast(header, AsBytes(entry_header), p_body);
  return SubmitResult::Accepted;
}

// Checks a message that only the primary of a view sends, and moves this
// replica into that view if the view is new. Returns true if this replica
// now follows the sender.
bool Replica::FollowPrimary(const Header &p_header)
{
  if (p_header.view_ < view_ || p_header.replica_ != PrimaryOf(p_header.view_))
  {
    return false;
  }
  if (p_header.view_ > view_)
  {
    EnterView(p_header.view_);
  }
  else if (status_ == Status::ViewChange)
  {
    status_ = Status::Joining;
    ResetViewChange();
  }
  if (status_ != Status::Normal && status_ != Status::Joining)
  {
    return false;
  }
  view_start_known_ = true;
  view_start_op_ = p_header.view_start_op_;
  primary_deadline_tick_ = TimeoutWithJitter(config_.primary_timeout_ticks_);
  return true;
}

void Replica::EnterView(std::uint32_t p_view)
{
  VSR_ASSERT(p_view > view_);
  if (!SyncJournal())
  {
    return;
  }
  view_ = p_view;
  if (!PersistSuperblock())
  {
    return;
  }
  status_ = Status::Joining;
  ResetViewChange();
}

void Replica::OnPrepare(const Message &p_message)
{
  const Header &header = p_message.header_;
  // DecodeMessage checked the entry.
  Entry entry;
  std::memcpy(&entry.header_, p_message.body_.data(), kEntryHeaderBytes);
  entry.body_ = p_message.body_.subspan(kEntryHeaderBytes);
  if (entry.body_.size() > config_.body_bytes_max_)
  {
    stats_.messages_invalid_ += 1;
    return;
  }
  if (Fetching() && header.view_ == view_ && header.replica_ == fetch_source_)
  {
    HandleEntryUnverified(entry);
    return;
  }
  if (!FollowPrimary(header))
  {
    return;
  }
  if (status_ == Status::Joining)
  {
    HandleEntryUnverified(entry);
  }
  else
  {
    HandleEntryNormal(entry);
  }
  LearnCommit(header);
}

// The primary says that all operations up to `commit_` are committed. That
// is true for this log only if this log is a prefix of the log of the primary.
void Replica::LearnCommit(const Header &p_header)
{
  const bool log_is_prefix =
      status_ == Status::Normal || (status_ == Status::Joining && log_verified_);
  if (!log_is_prefix)
  {
    return;
  }
  const bool same_checkpoint =
      p_header.checkpoint_op_ != 0 && p_header.checkpoint_op_ == superblock_.checkpoint_op_;
  if (same_checkpoint && p_header.checkpoint_checksum_ != superblock_.checkpoint_checksum_)
  {
    Halt(HaltReason::Divergence);
    return;
  }
  commit_max_ = std::max(commit_max_, p_header.commit_);
  ExecuteCommitted();
}

void Replica::HandleEntryNormal(const Entry &p_entry)
{
  const EntryHeader &entry = p_entry.header_;
  if (entry.op_ <= journal_.Head())
  {
    if (entry.op_ >= journal_.Floor() && journal_.ChecksumAt(entry.op_) != entry.checksum_)
    {
      stats_.protocol_violations_ += 1;
      return;
    }
    // The primary sent it again, so the acknowledgement was possibly lost.
    if (entry.op_ <= op_synced_)
    {
      SendPrepareOk();
    }
    return;
  }
  if (entry.op_ > journal_.Head() + 1)
  {
    RequestPrepare(journal_.Head() + 1);
    return;
  }
  if (entry.parent_ != journal_.HeadChecksum())
  {
    stats_.protocol_violations_ += 1;
    return;
  }
  if (!AppendEntry(p_entry))
  {
    return;
  }
  if (target_known_ && journal_.Head() < target_op_)
  {
    RequestPrepare(journal_.Head() + 1);
  }
}

// An entry from the log that this replica follows, while its own log is
// possibly not a prefix of that log. The entry shows where the two logs
// start to differ. Entries of this log after that point are not in the
// followed log, so they are not committed, and this replica removes them.
void Replica::HandleEntryUnverified(const Entry &p_entry)
{
  const EntryHeader &entry = p_entry.header_;
  if (entry.op_ <= journal_.Head())
  {
    if (entry.op_ < journal_.Floor())
    {
      return;
    }
    if (journal_.ChecksumAt(entry.op_) == entry.checksum_)
    {
      if (entry.op_ == journal_.Head())
      {
        OnVerified();
      }
      return;
    }
    if (entry.op_ == journal_.Floor())
    {
      stats_.protocol_violations_ += 1;
      return;
    }
    if (!TruncateTo(entry.op_ - 1))
    {
      return;
    }
  }
  if (entry.op_ != journal_.Head() + 1)
  {
    RequestPrepare(journal_.Head() + 1);
    return;
  }
  if (entry.parent_ != journal_.HeadChecksum())
  {
    // The head of this log is not in the followed log. Remove it and ask
    // for the entry in its place.
    if (journal_.Head() == journal_.Floor())
    {
      stats_.protocol_violations_ += 1;
      return;
    }
    if (!TruncateTo(journal_.Head() - 1))
    {
      return;
    }
    RequestPrepare(journal_.Head() + 1);
    return;
  }
  if (Fetching() && entry.op_ > target_op_)
  {
    return;
  }
  if (!log_verified_)
  {
    OnVerified();
  }
  const bool still_following = status_ == Status::Joining || Fetching();
  if (!still_following)
  {
    // OnVerified made this replica Normal. The entry is a usual Prepare now.
    if (status_ == Status::Normal && !IsPrimary())
    {
      HandleEntryNormal(p_entry);
    }
    return;
  }
  if (!AppendEntry(p_entry))
  {
    return;
  }
  CheckFollowDone();
  if (status_ == Status::Joining || Fetching())
  {
    RequestPrepare(journal_.Head() + 1);
  }
}

bool Replica::AppendEntry(const Entry &p_entry)
{
  // An entry past this point would replace an entry that no checkpoint
  // covers. The replica must execute and checkpoint first.
  if (p_entry.header_.op_ > superblock_.checkpoint_op_ + config_.log_slot_count_)
  {
    return false;
  }
  if (!journal_.Append(p_entry.header_, p_entry.body_))
  {
    Halt(HaltReason::StorageFault);
    return false;
  }
  timestamp_last_ns_ = std::max(timestamp_last_ns_, p_entry.header_.timestamp_ns_);
  return true;
}

bool Replica::TruncateTo(std::uint64_t p_op)
{
  VSR_ASSERT(p_op >= journal_.Floor());
  VSR_ASSERT(p_op <= journal_.Head());
  if (p_op == journal_.Head())
  {
    return true;
  }
  // A committed operation is in every log that a replica can follow. A
  // request to remove one comes from a peer that does not follow the protocol.
  if (p_op < std::min(commit_max_, journal_.Head()))
  {
    stats_.protocol_violations_ += 1;
    return false;
  }
  if (!journal_.TruncateTo(p_op))
  {
    Halt(HaltReason::StorageFault);
    return false;
  }
  op_synced_ = std::min(op_synced_, p_op);
  prepare_ok_sent_op_ = std::min(prepare_ok_sent_op_, p_op);
  return true;
}

// Compares this log with the head of the followed log, then asks for the
// next entry that this log needs.
void Replica::Follow()
{
  VSR_ASSERT(status_ == Status::Joining || Fetching());
  if (sync_.active_)
  {
    return;
  }
  if (!log_verified_)
  {
    if (!target_known_ || target_op_ < journal_.Floor())
    {
      return;
    }
    if (journal_.Head() >= target_op_)
    {
      if (journal_.ChecksumAt(target_op_) == target_checksum_)
      {
        if (!TruncateTo(target_op_))
        {
          return;
        }
        OnVerified();
      }
      else
      {
        if (target_op_ == journal_.Floor())
        {
          stats_.protocol_violations_ += 1;
          return;
        }
        if (!TruncateTo(target_op_ - 1))
        {
          return;
        }
      }
    }
  }
  if (status_ == Status::Joining || Fetching())
  {
    RequestPrepare(journal_.Head() + 1);
  }
}

void Replica::OnVerified()
{
  log_verified_ = true;
  if (Fetching())
  {
    // The source log holds every committed operation, so the commit numbers
    // of the quorum are valid for this log now.
    commit_max_ = std::max(commit_max_, std::min(view_commit_, target_op_));
  }
  CheckFollowDone();
}

void Replica::CheckFollowDone()
{
  if (!log_verified_)
  {
    return;
  }
  if (Fetching())
  {
    if (journal_.Head() != target_op_)
    {
      return;
    }
    if (journal_.HeadChecksum() != target_checksum_)
    {
      stats_.protocol_violations_ += 1;
      return;
    }
    FinishViewChange();
    return;
  }
  if (status_ == Status::Joining && view_start_known_ && journal_.Head() >= view_start_op_)
  {
    BecomeNormal();
  }
}

// A log with `log_view_ == v` must hold every operation that the primary
// held when it opened view v. A later view change selects a log by
// `log_view_` first, so a shorter log with a new `log_view_` could replace a
// longer log that holds committed operations.
void Replica::BecomeNormal()
{
  VSR_ASSERT(status_ == Status::Joining);
  VSR_ASSERT(log_verified_);
  if (!SyncJournal())
  {
    return;
  }
  log_view_ = view_;
  if (!PersistSuperblock())
  {
    return;
  }
  status_ = Status::Normal;
  SendPrepareOk();
}

void Replica::OnCommit(const Header &p_header)
{
  if (!FollowPrimary(p_header))
  {
    return;
  }
  target_known_ = true;
  target_op_ = p_header.op_;
  target_checksum_ = p_header.log_checksum_;
  if (status_ == Status::Joining)
  {
    Follow();
  }
  LearnCommit(p_header);
  const bool behind = status_ == Status::Normal && journal_.Head() < target_op_;
  if (behind && !sync_.active_)
  {
    RequestPrepare(journal_.Head() + 1);
  }
}

void Replica::OnPrepareOk(const Header &p_header)
{
  if (!IsPrimary() || p_header.view_ != view_)
  {
    return;
  }
  if (p_header.op_ > journal_.Head())
  {
    stats_.protocol_violations_ += 1;
    return;
  }
  if (p_header.op_ < journal_.Floor())
  {
    return;
  }
  if (journal_.ChecksumAt(p_header.op_) != p_header.log_checksum_)
  {
    stats_.protocol_violations_ += 1;
    return;
  }
  auto &acked_op = acked_ops_[p_header.replica_];
  acked_op = std::max(acked_op, p_header.op_);
  AdvanceCommit();
}

void Replica::OnRequestPrepare(const Header &p_header)
{
  if (p_header.view_ != view_)
  {
    return;
  }
  // A replica in a view change does not change its log, so the new primary
  // can copy it. A backup in an open view does not serve: its log can be
  // only a part of the log of the primary.
  const bool frozen_for_new_primary = status_ == Status::ViewChange && do_view_change_sent_ &&
                                      p_header.replica_ == PrimaryOf(view_);
  if (!IsPrimary() && !frozen_for_new_primary)
  {
    return;
  }
  if (p_header.op_ == 0 || p_header.op_ > journal_.Head())
  {
    return;
  }
  if (p_header.op_ <= journal_.Floor())
  {
    // The ring no longer holds the entry. A checkpoint replaces it.
    if (IsPrimary())
    {
      SendSnapshotChunk(p_header.replica_, 0);
    }
    return;
  }
  const std::uint64_t last_op = std::min(journal_.Head(), p_header.op_ + kRepairBatchOps - 1);
  for (std::uint64_t op = p_header.op_; op <= last_op && status_ != Status::Halted; ++op)
  {
    SendPrepare(p_header.replica_, op);
  }
}

// --- View change -----------------------------------------------------------

void Replica::ResetViewChange()
{
  start_view_change_from_.reset();
  do_view_changes_.fill(DoViewChangeRecord{});
  do_view_change_sent_ = false;
  view_commit_ = 0;
  fetching_ = false;
  log_verified_ = false;
  view_start_known_ = false;
  view_start_op_ = 0;
  target_known_ = false;
  sync_ = SnapshotSync{};
  request_prepare_op_ = 0;
  prepare_ok_sent_op_ = 0;
}

void Replica::StartViewChange(std::uint32_t p_view)
{
  VSR_ASSERT(p_view > view_);
  // The DoViewChange of this replica names the head of its log. Make the
  // log durable first, so that a restart cannot give a shorter log.
  if (!SyncJournal())
  {
    return;
  }
  view_ = p_view;
  if (!PersistSuperblock())
  {
    return;
  }
  stats_.view_changes_ += 1;
  BeginViewChangeRound();
}

void Replica::BeginViewChangeRound()
{
  status_ = Status::ViewChange;
  ResetViewChange();
  view_change_deadline_tick_ = TimeoutWithJitter(config_.view_change_timeout_ticks_);
  Broadcast(MakeHeader(Command::StartViewChange));
  MaybeSendDoViewChange();
}

void Replica::MaybeSendDoViewChange()
{
  VSR_ASSERT(status_ == Status::ViewChange);
  const std::size_t in_view_count = start_view_change_from_.count() + 1;
  if (do_view_change_sent_ || in_view_count < Quorum())
  {
    return;
  }
  do_view_change_sent_ = true;
  SendDoViewChange();
}

void Replica::SendDoViewChange()
{
  VSR_ASSERT(do_view_change_sent_);
  if (PrimaryOf(view_) == config_.replica_)
  {
    auto &own = do_view_changes_[config_.replica_];
    own.present_ = true;
    own.log_view_ = log_view_;
    own.op_ = journal_.Head();
    own.checksum_ = journal_.HeadChecksum();
    own.commit_ = commit_max_;
    CheckViewChangeQuorum();
    return;
  }
  Header header = MakeHeader(Command::DoViewChange);
  header.log_view_ = log_view_;
  header.op_ = journal_.Head();
  header.log_checksum_ = journal_.HeadChecksum();
  header.commit_ = commit_max_;
  Send(PrimaryOf(view_), header);
}

void Replica::OnStartViewChange(const Header &p_header)
{
  if (p_header.view_ < view_)
  {
    return;
  }
  if (p_header.view_ > view_)
  {
    StartViewChange(p_header.view_);
  }
  if (status_ == Status::ViewChange)
  {
    start_view_change_from_.set(p_header.replica_);
    MaybeSendDoViewChange();
    return;
  }
  // The sender does not know that the view is open.
  if (IsPrimary())
  {
    Send(p_header.replica_, MakeHeadHeader(Command::StartView));
  }
}

void Replica::OnDoViewChange(const Header &p_header)
{
  if (p_header.view_ < view_ || PrimaryOf(p_header.view_) != config_.replica_)
  {
    return;
  }
  if (p_header.view_ > view_)
  {
    StartViewChange(p_header.view_);
  }
  if (status_ == Status::Normal)
  {
    Send(p_header.replica_, MakeHeadHeader(Command::StartView));
    return;
  }
  if (status_ != Status::ViewChange)
  {
    return;
  }
  auto &record = do_view_changes_[p_header.replica_];
  record.present_ = true;
  record.log_view_ = p_header.log_view_;
  record.op_ = p_header.op_;
  record.checksum_ = p_header.log_checksum_;
  record.commit_ = p_header.commit_;
  // A DoViewChange also shows that the sender is in this view.
  start_view_change_from_.set(p_header.replica_);
  MaybeSendDoViewChange();
  CheckViewChangeQuorum();
}

void Replica::CheckViewChangeQuorum()
{
  const bool is_new_primary = status_ == Status::ViewChange && PrimaryOf(view_) == config_.replica_;
  if (!is_new_primary || fetching_ || !do_view_changes_[config_.replica_].present_)
  {
    return;
  }
  std::uint32_t present_count = 0;
  std::uint8_t best = config_.replica_;
  std::uint64_t commit = 0;
  for (std::uint8_t replica = 0; replica < config_.replica_count_; ++replica)
  {
    const auto &record = do_view_changes_[replica];
    if (!record.present_)
    {
      continue;
    }
    present_count += 1;
    commit = std::max(commit, record.commit_);
    const auto &leader = do_view_changes_[best];
    // The log of the latest view wins, then the longer log. Each log with
    // the highest `log_view_` in a quorum that is at least as long as the
    // others holds every committed operation, so a tie can go either way.
    const bool better = record.log_view_ > leader.log_view_ ||
                        (record.log_view_ == leader.log_view_ && record.op_ > leader.op_);
    if (better)
    {
      best = replica;
    }
  }
  if (present_count < Quorum())
  {
    return;
  }
  view_commit_ = commit;
  target_known_ = true;
  target_op_ = do_view_changes_[best].op_;
  target_checksum_ = do_view_changes_[best].checksum_;
  if (best == config_.replica_)
  {
    FinishViewChange();
    return;
  }
  fetching_ = true;
  fetch_source_ = best;
  Follow();
}

void Replica::FinishViewChange()
{
  VSR_ASSERT(status_ == Status::ViewChange);
  VSR_ASSERT(PrimaryOf(view_) == config_.replica_);
  if (!SyncJournal())
  {
    return;
  }
  log_view_ = view_;
  if (!PersistSuperblock())
  {
    return;
  }
  status_ = Status::Normal;
  fetching_ = false;
  log_verified_ = true;
  view_start_known_ = true;
  view_start_op_ = journal_.Head();
  commit_max_ = std::max(commit_max_, std::min(view_commit_, journal_.Head()));
  acked_ops_.fill(0);
  heartbeat_deadline_tick_ = ticks_ + config_.heartbeat_ticks_;
  Broadcast(MakeHeadHeader(Command::StartView));
  AdvanceCommit();
  ExecuteCommitted();
}

// --- Commit and execution --------------------------------------------------

void Replica::AdvanceCommit()
{
  if (!IsPrimary())
  {
    return;
  }
  acked_ops_[config_.replica_] = op_synced_;
  // The highest operation that a quorum of replicas holds.
  std::uint64_t quorum_op = 0;
  for (std::size_t candidate = 0; candidate < config_.replica_count_; ++candidate)
  {
    std::uint32_t holder_count = 0;
    for (std::size_t replica = 0; replica < config_.replica_count_; ++replica)
    {
      holder_count += acked_ops_[replica] >= acked_ops_[candidate] ? 1U : 0U;
    }
    if (holder_count >= Quorum())
    {
      quorum_op = std::max(quorum_op, acked_ops_[candidate]);
    }
  }
  if (quorum_op > commit_max_)
  {
    commit_max_ = quorum_op;
    ExecuteCommitted();
  }
}

void Replica::ExecuteCommitted()
{
  // Every entry up to min(commit_max_, Head()) is committed in each status:
  // `commit_max_` only rises while the log is a prefix of a log that holds
  // all committed operations, and TruncateTo never removes such an entry.
  const std::uint64_t limit = std::min(commit_max_, journal_.Head());
  std::uint32_t executed_count = 0;
  while (status_ != Status::Halted && commit_min_ < limit && executed_count < kExecuteBatchOps)
  {
    ExecuteOne(commit_min_ + 1);
    executed_count += 1;
  }
}

void Replica::ExecuteOne(std::uint64_t p_op)
{
  const EntryHeader entry = journal_.HeaderAt(p_op);
  const auto body = journal_.ReadBody(p_op, body_buffer_);
  if (!body)
  {
    Halt(body.error() == internal::JournalReadError::Io ? HaltReason::StorageFault
                                                        : HaltReason::CorruptEntry);
    return;
  }
  // The client table decides if the entry executes, and the table is equal
  // on each replica at this operation, so each replica decides the same.
  // An entry executes only if its client has a session and the request is
  // newer than the last one of that session. A client that lost its
  // session between prepare and commit gets no execution and no reply.
  const auto *const session = clients_.Find(entry.client_);
  const bool is_registration = entry.request_ == 0;
  internal::ClientEntry commit;
  commit.client_ = entry.client_;
  commit.request_ = entry.request_;
  commit.op_ = entry.op_;
  bool committed = false;
  if (is_registration && session == nullptr)
  {
    clients_.Commit(commit, {});
    committed = true;
  }
  else if (!is_registration && session != nullptr && session->request_ < entry.request_)
  {
    Operation operation;
    operation.op_ = entry.op_;
    operation.timestamp_ns_ = entry.timestamp_ns_;
    operation.client_ = entry.client_;
    operation.request_ = entry.request_;
    const std::size_t reply_bytes = state_machine_->Execute(operation, *body, reply_buffer_);
    VSR_ASSERT(reply_bytes <= reply_buffer_.size());
    clients_.Commit(commit, std::span{reply_buffer_}.first(reply_bytes));
    committed = true;
  }
  if (committed && IsPrimary())
  {
    SendReply(*clients_.Find(entry.client_));
  }
  commit_min_ = p_op;
  timestamp_committed_ns_ = entry.timestamp_ns_;
  if (p_op % config_.checkpoint_interval_ == 0)
  {
    Checkpoint();
  }
}

// Each replica takes a checkpoint at the same operations. The checksums of
// the checkpoints are then equal, unless the state machines differ.
void Replica::Checkpoint()
{
  // The buffer is shared with a snapshot that is in transfer. The transfer
  // starts again.
  sync_ = SnapshotSync{};

  constexpr std::size_t kTimestampBytes = sizeof(std::uint64_t);
  std::memcpy(snapshot_buffer_.data(), &timestamp_committed_ns_, kTimestampBytes);
  const std::size_t clients_bytes =
      clients_.Serialize(std::span{snapshot_buffer_}.subspan(kTimestampBytes));
  const std::size_t state_offset = kTimestampBytes + clients_bytes;
  const auto state_bytes = state_machine_->Snapshot(
      std::span{snapshot_buffer_}.subspan(state_offset, config_.snapshot_bytes_max_));
  if (!state_bytes)
  {
    Halt(HaltReason::SnapshotTooLarge);
    return;
  }
  VSR_ASSERT(*state_bytes <= config_.snapshot_bytes_max_);
  const auto blob = std::span<const std::byte>{snapshot_buffer_}.first(state_offset + *state_bytes);

  // The other slot. The current checkpoint stays valid until the superblock
  // names the new one.
  const auto slot_index =
      static_cast<std::uint8_t>((superblock_.checkpoint_slot_ + 1U) % internal::kSnapshotSlotCount);
  if (!storage_->Write(layout_.SnapshotOffset(slot_index), blob) || !storage_->Sync())
  {
    Halt(HaltReason::StorageFault);
    return;
  }
  superblock_.checkpoint_op_ = commit_min_;
  superblock_.checkpoint_entry_checksum_ = journal_.ChecksumAt(commit_min_);
  superblock_.checkpoint_checksum_ = Checksum(blob);
  superblock_.checkpoint_bytes_ = blob.size();
  superblock_.checkpoint_slot_ = slot_index;
  if (!PersistSuperblock())
  {
    return;
  }
  stats_.checkpoints_ += 1;
}

bool Replica::LoadSnapshotBlob(std::span<const std::byte> p_blob)
{
  internal::ByteReader reader{p_blob};
  const auto timestamp_ns = reader.Get<std::uint64_t>();
  if (!timestamp_ns)
  {
    return false;
  }
  const auto clients_bytes = clients_.Deserialize(reader.Rest());
  if (!clients_bytes)
  {
    return false;
  }
  const auto state = reader.Rest().subspan(*clients_bytes);
  if (state.size() > config_.snapshot_bytes_max_ || !state_machine_->Restore(state))
  {
    return false;
  }
  timestamp_committed_ns_ = *timestamp_ns;
  timestamp_last_ns_ = std::max(timestamp_last_ns_, *timestamp_ns);
  return true;
}

// --- Snapshot transfer -----------------------------------------------------

void Replica::OnRequestSnapshot(const Header &p_header)
{
  if (!IsPrimary() || p_header.view_ != view_)
  {
    return;
  }
  // The checkpoint can change during a transfer. The receiver starts again
  // when it sees the first chunk of a new checkpoint.
  const bool same_checkpoint = p_header.checkpoint_op_ == superblock_.checkpoint_op_;
  SendSnapshotChunk(p_header.replica_, same_checkpoint ? p_header.offset_ : 0);
}

void Replica::OnSnapshot(const Message &p_message)
{
  const Header &header = p_message.header_;
  if (!FollowPrimary(header))
  {
    return;
  }
  // A snapshot is needed only for operations that the primary cannot send.
  if (header.checkpoint_op_ <= journal_.Head())
  {
    return;
  }
  if (header.total_bytes_ > snapshot_buffer_.size())
  {
    stats_.messages_invalid_ += 1;
    return;
  }
  const bool is_current = sync_.active_ && sync_.checkpoint_op_ == header.checkpoint_op_ &&
                          sync_.checksum_ == header.checkpoint_checksum_;
  if (!is_current)
  {
    const bool is_newer = !sync_.active_ || header.checkpoint_op_ > sync_.checkpoint_op_;
    if (!is_newer || header.offset_ != 0)
    {
      return;
    }
    sync_ = SnapshotSync{};
    sync_.active_ = true;
    sync_.checkpoint_op_ = header.checkpoint_op_;
    sync_.entry_checksum_ = header.log_checksum_;
    sync_.checksum_ = header.checkpoint_checksum_;
    sync_.total_bytes_ = header.total_bytes_;
  }
  if (header.offset_ != sync_.received_bytes_)
  {
    return;
  }
  if (p_message.body_.empty() ||
      p_message.body_.size() > sync_.total_bytes_ - sync_.received_bytes_)
  {
    stats_.messages_invalid_ += 1;
    return;
  }
  std::memcpy(snapshot_buffer_.data() + sync_.received_bytes_, p_message.body_.data(),
              p_message.body_.size());
  sync_.received_bytes_ += p_message.body_.size();
  if (sync_.received_bytes_ < sync_.total_bytes_)
  {
    RequestSnapshotChunk();
    return;
  }
  const auto blob = std::span<const std::byte>{snapshot_buffer_}.first(sync_.total_bytes_);
  if (Checksum(blob) != sync_.checksum_)
  {
    stats_.protocol_violations_ += 1;
    sync_ = SnapshotSync{};
    return;
  }
  InstallSnapshot();
}

void Replica::InstallSnapshot()
{
  VSR_ASSERT(sync_.active_);
  VSR_ASSERT(sync_.checkpoint_op_ > journal_.Head());
  const auto blob = std::span<const std::byte>{snapshot_buffer_}.first(sync_.total_bytes_);
  const auto slot_index =
      static_cast<std::uint8_t>((superblock_.checkpoint_slot_ + 1U) % internal::kSnapshotSlotCount);
  if (!storage_->Write(layout_.SnapshotOffset(slot_index), blob) || !storage_->Sync())
  {
    Halt(HaltReason::StorageFault);
    return;
  }
  // The superblock comes before the log reset. After a crash between the
  // two, the checkpoint covers every entry that this replica acknowledged.
  superblock_.checkpoint_op_ = sync_.checkpoint_op_;
  superblock_.checkpoint_entry_checksum_ = sync_.entry_checksum_;
  superblock_.checkpoint_checksum_ = sync_.checksum_;
  superblock_.checkpoint_bytes_ = sync_.total_bytes_;
  superblock_.checkpoint_slot_ = slot_index;
  if (!PersistSuperblock())
  {
    return;
  }
  if (!journal_.Reset(sync_.checkpoint_op_, sync_.entry_checksum_))
  {
    Halt(HaltReason::StorageFault);
    return;
  }
  if (!LoadSnapshotBlob(blob))
  {
    Halt(HaltReason::RestoreFailed);
    return;
  }
  commit_min_ = sync_.checkpoint_op_;
  commit_max_ = std::max(commit_max_, commit_min_);
  op_synced_ = commit_min_;
  prepare_ok_sent_op_ = 0;
  request_prepare_op_ = 0;
  sync_ = SnapshotSync{};
  stats_.snapshots_installed_ += 1;
  // A checkpoint holds committed operations only, so the log is a prefix of
  // the log of the primary.
  if (status_ == Status::Joining)
  {
    OnVerified();
  }
}

// --- Timers ----------------------------------------------------------------

void Replica::Tick(std::uint64_t p_realtime_ns)
{
  if (status_ == Status::Halted)
  {
    return;
  }
  ticks_ += 1;
  realtime_ns_ = p_realtime_ns;
  const bool retry_due = ticks_ >= retry_deadline_tick_;
  if (retry_due)
  {
    retry_deadline_tick_ = ticks_ + config_.retry_ticks_;
  }
  switch (status_)
  {
  case Status::Normal:
    if (IsPrimary())
    {
      TickPrimary(retry_due);
    }
    else
    {
      TickBackup(retry_due);
    }
    break;
  case Status::Joining:
    TickBackup(retry_due);
    break;
  case Status::ViewChange:
    TickViewChange(retry_due);
    break;
  case Status::Halted:
    break;
  }
  ExecuteCommitted();
}

void Replica::TickPrimary(bool p_retry_due)
{
  if (ticks_ >= heartbeat_deadline_tick_)
  {
    heartbeat_deadline_tick_ = ticks_ + config_.heartbeat_ticks_;
    Broadcast(MakeHeadHeader(Command::Commit));
  }
  if (!p_retry_due)
  {
    return;
  }
  // Send the entries of the pipeline again to each backup that did not
  // acknowledge them. A backup that is further behind asks for entries.
  for (std::uint8_t replica = 0; replica < config_.replica_count_; ++replica)
  {
    if (replica == config_.replica_ || acked_ops_[replica] < commit_max_)
    {
      continue;
    }
    const std::uint64_t last_op = std::min(journal_.Head(), acked_ops_[replica] + kRepairBatchOps);
    for (std::uint64_t op = acked_ops_[replica] + 1; op <= last_op; ++op)
    {
      if (status_ == Status::Halted)
      {
        return;
      }
      SendPrepare(replica, op);
    }
  }
}

void Replica::TickBackup(bool p_retry_due)
{
  if (ticks_ >= primary_deadline_tick_)
  {
    StartViewChange(view_ + 1);
    return;
  }
  if (!p_retry_due)
  {
    return;
  }
  request_prepare_op_ = 0;
  if (sync_.active_)
  {
    RequestSnapshotChunk();
  }
  else if (status_ == Status::Joining)
  {
    Follow();
  }
  else if (target_known_ && journal_.Head() < target_op_)
  {
    RequestPrepare(journal_.Head() + 1);
  }
  else if (status_ == Status::Normal)
  {
    // Also when nothing is new: the last acknowledgement was possibly lost,
    // and the primary sends an entry again only while it is in the pipeline.
    SendPrepareOk();
  }
}

void Replica::TickViewChange(bool p_retry_due)
{
  if (ticks_ >= view_change_deadline_tick_)
  {
    StartViewChange(view_ + 1);
    return;
  }
  if (!p_retry_due)
  {
    return;
  }
  Broadcast(MakeHeader(Command::StartViewChange));
  if (do_view_change_sent_ && PrimaryOf(view_) != config_.replica_)
  {
    SendDoViewChange();
  }
  if (Fetching())
  {
    request_prepare_op_ = 0;
    Follow();
  }
}

void Replica::Flush()
{
  if (status_ == Status::Halted)
  {
    return;
  }
  if (!SyncJournal())
  {
    return;
  }
  if (status_ == Status::Normal)
  {
    if (IsPrimary())
    {
      AdvanceCommit();
    }
    else if (op_synced_ > prepare_ok_sent_op_)
    {
      SendPrepareOk();
    }
  }
  ExecuteCommitted();
}

} // namespace vsr
