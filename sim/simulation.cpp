#include "vsr/sim/simulation.hpp"

#include "vsr/assert.hpp"
#include "vsr/checksum.hpp"
#include "vsr/interfaces.hpp"
#include "vsr/internal/layout.hpp"
#include "vsr/internal/superblock.hpp"
#include "vsr/message.hpp"
#include "vsr/replica.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vsr::sim
{

namespace
{

constexpr std::uint32_t kRateDenominator = 10'000;
constexpr std::uint64_t kTickNs = 1'000'000;
constexpr std::uint64_t kClientRetryTicks = 40;
constexpr std::uint64_t kClientThinkTicksMax = 20;
constexpr std::uint64_t kRestartDelayTicksMax = 300;
constexpr std::uint32_t kStorageFaultOpsMax = 6;
// A crash before a flush is the case that loses writes, so make it frequent.
constexpr std::uint32_t kUnflushedCrashFactor = 5;
// Larger than one default message body, so that a snapshot needs several
// chunks.
constexpr std::size_t kHashSnapshotPaddingBytes = 600;
constexpr std::size_t kHashSnapshotBytes = sizeof(std::uint64_t) + kHashSnapshotPaddingBytes;
constexpr std::size_t kHashReplyBytes = 2 * sizeof(std::uint64_t);
// The simulator writes the last executed operation before the snapshot of
// the application.
constexpr std::size_t kSnapshotHeaderBytes = sizeof(std::uint64_t);

class Random final
{
public:
  explicit Random(std::uint64_t p_seed) noexcept : state_(p_seed) {}

  [[nodiscard]] std::uint64_t Next() noexcept
  {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t mixed = state_;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31);
  }

  // A value in [0, p_limit). Requires p_limit > 0.
  [[nodiscard]] std::uint64_t Below(std::uint64_t p_limit) noexcept { return Next() % p_limit; }

  [[nodiscard]] bool Chance(std::uint32_t p_rate) noexcept
  {
    return Below(kRateDenominator) < p_rate;
  }

private:
  std::uint64_t state_;
};

// Storage that loses, keeps, or tears each write that was not synced when
// the power fails.
class SimStorage final : public Storage
{
public:
  explicit SimStorage(std::uint64_t p_bytes) : current_(p_bytes), durable_(p_bytes) {}

  [[nodiscard]] std::expected<void, StorageError> Read(std::uint64_t p_offset,
                                                       std::span<std::byte> p_out) override
  {
    if (failing_)
    {
      return std::unexpected(StorageError::Io);
    }
    VSR_ASSERT(p_offset + p_out.size() <= current_.size());
    std::memcpy(p_out.data(), current_.data() + p_offset, p_out.size());
    return {};
  }

  [[nodiscard]] std::expected<void, StorageError> Write(std::uint64_t p_offset,
                                                        std::span<const std::byte> p_bytes) override
  {
    VSR_ASSERT(p_offset + p_bytes.size() <= current_.size());
    // The power can fail during this write, so it stays pending also when
    // the fault starts here.
    pending_.push_back(PendingWrite{p_offset, {p_bytes.begin(), p_bytes.end()}});
    if (!CountOperation())
    {
      return std::unexpected(StorageError::Io);
    }
    std::memcpy(current_.data() + p_offset, p_bytes.data(), p_bytes.size());
    return {};
  }

  [[nodiscard]] std::expected<void, StorageError> Sync() override
  {
    if (!CountOperation())
    {
      return std::unexpected(StorageError::Io);
    }
    for (const auto &write : pending_)
    {
      std::memcpy(durable_.data() + write.offset_, write.bytes_.data(), write.bytes_.size());
    }
    pending_.clear();
    return {};
  }

  // The storage fails at one of the next operations, as in a power loss.
  void InjectFault(std::uint32_t p_operations) { operations_until_fault_ = p_operations; }

  [[nodiscard]] bool Failing() const noexcept { return failing_; }

  // True if a write that is not yet synced holds a log entry at or before
  // `p_op`. Such an entry can be lost in a power loss.
  [[nodiscard]] bool HasUnsyncedEntryUpTo(std::uint64_t p_op) const
  {
    return std::ranges::any_of(pending_,
                               [p_op](const PendingWrite &p_write)
                               {
                                 const auto entry = DecodeEntry(p_write.bytes_);
                                 return entry.has_value() && entry->header_.op_ <= p_op;
                               });
  }

  // Reads what a power loss at this moment would leave, if it lost every
  // write that is not synced.
  void ReadDurable(std::uint64_t p_offset, std::span<std::byte> p_out) const
  {
    VSR_ASSERT(p_offset + p_out.size() <= durable_.size());
    std::memcpy(p_out.data(), durable_.data() + p_offset, p_out.size());
  }

  void Crash(Random &p_random)
  {
    for (const auto &write : pending_)
    {
      const std::uint64_t outcome = p_random.Below(3);
      std::size_t kept_bytes = 0;
      if (outcome == 1)
      {
        kept_bytes = write.bytes_.size();
      }
      else if (outcome == 2)
      {
        kept_bytes = p_random.Below(write.bytes_.size() + 1);
      }
      std::memcpy(durable_.data() + write.offset_, write.bytes_.data(), kept_bytes);
    }
    pending_.clear();
    current_ = durable_;
    failing_ = false;
    operations_until_fault_ = 0;
  }

private:
  struct PendingWrite
  {
    std::uint64_t offset_ = 0;
    std::vector<std::byte> bytes_;
  };

  [[nodiscard]] bool CountOperation() noexcept
  {
    if (failing_)
    {
      return false;
    }
    if (operations_until_fault_ > 0)
    {
      operations_until_fault_ -= 1;
      failing_ = operations_until_fault_ == 0;
    }
    return !failing_;
  }

  std::vector<std::byte> current_;
  std::vector<std::byte> durable_;
  std::vector<PendingWrite> pending_;
  std::uint32_t operations_until_fault_ = 0;
  bool failing_ = false;
};

// The durable content of a storage, for the checks.
class DurableView final : public Storage
{
public:
  explicit DurableView(const SimStorage *p_storage) : storage_(p_storage) {}

  [[nodiscard]] std::expected<void, StorageError> Read(std::uint64_t p_offset,
                                                       std::span<std::byte> p_out) override
  {
    storage_->ReadDurable(p_offset, p_out);
    return {};
  }

  [[nodiscard]] std::expected<void, StorageError> Write(std::uint64_t,
                                                        std::span<const std::byte>) override
  {
    return std::unexpected(StorageError::Io);
  }

  [[nodiscard]] std::expected<void, StorageError> Sync() override
  {
    return std::unexpected(StorageError::Io);
  }

private:
  const SimStorage *storage_;
};

struct Committed
{
  std::uint64_t client_ = 0;
  std::uint64_t request_ = 0;
  std::uint64_t timestamp_ns_ = 0;
  // The digest of the application after the operation.
  std::uint64_t digest_ = 0;
  std::uint64_t reply_checksum_ = 0;
};

// The safety checks. It records the first execution of each operation, and
// compares every other execution with it.
class Checker final
{
public:
  explicit Checker(std::size_t p_replica_count) : executed_ops_(p_replica_count) {}

  void Fail(std::string p_message)
  {
    if (failure_.empty())
    {
      failure_ = std::move(p_message);
    }
  }

  [[nodiscard]] const std::string &Failure() const noexcept { return failure_; }

  // The highest operation that a state machine executed.
  [[nodiscard]] std::uint64_t HighestOp() const noexcept
  {
    return log_.empty() ? 0 : log_.size() - 1;
  }

  [[nodiscard]] std::uint64_t ExecutedCount() const noexcept { return executed_count_; }

  // False for an operation that no state machine executed: a client
  // registration, or an entry of a client without a session.
  [[nodiscard]] bool Has(std::uint64_t p_op) const noexcept
  {
    return p_op < log_.size() && log_[p_op].has_value();
  }

  [[nodiscard]] const Committed &At(std::uint64_t p_op) const noexcept
  {
    VSR_ASSERT(Has(p_op));
    return *log_[p_op];
  }

  // A replica starts with a new application. Each new application must
  // hold the same state.
  void OnStart(std::size_t p_replica, std::uint64_t p_digest)
  {
    executed_ops_[p_replica] = 0;
    if (!initial_digest_.has_value())
    {
      initial_digest_ = p_digest;
    }
    else if (*initial_digest_ != p_digest)
    {
      Fail(std::format("replica {} started with a different initial state", p_replica));
    }
  }

  void OnRestore(std::size_t p_replica, std::uint64_t p_op, std::uint64_t p_digest)
  {
    // A snapshot from before the first execution holds the initial state.
    const bool known = p_op == 0 || Has(p_op);
    if (!known)
    {
      Fail(std::format("replica {} restored a snapshot at op {} that no replica executed",
                       p_replica, p_op));
      return;
    }
    const std::uint64_t expected = p_op == 0 ? initial_digest_.value_or(0) : At(p_op).digest_;
    if (expected != p_digest)
    {
      Fail(std::format("replica {} restored a wrong state at op {}", p_replica, p_op));
    }
    executed_ops_[p_replica] = p_op;
  }

  void OnExecute(std::size_t p_replica, const Operation &p_operation, const Committed &p_result)
  {
    const std::uint64_t op = p_operation.op_;
    if (op <= executed_ops_[p_replica])
    {
      Fail(std::format("replica {} executed op {} after op {}", p_replica, op,
                       executed_ops_[p_replica]));
      return;
    }
    executed_ops_[p_replica] = op;
    if (op <= HighestOp())
    {
      // Another replica went past this operation. It must have executed it:
      // the digest does not always cover the order, and this gives the
      // clearer message.
      if (!Has(op))
      {
        Fail(std::format("replica {} executed op {} that another replica did not execute",
                         p_replica, op));
        return;
      }
      const auto &first = At(op);
      if (first.client_ != p_result.client_ || first.request_ != p_result.request_ ||
          first.timestamp_ns_ != p_result.timestamp_ns_)
      {
        Fail(std::format("replica {} executed a different operation at op {}", p_replica, op));
      }
      else if (first.digest_ != p_result.digest_)
      {
        Fail(std::format("replica {} has a different state after op {}", p_replica, op));
      }
      else if (first.reply_checksum_ != p_result.reply_checksum_)
      {
        Fail(std::format("replica {} made a different reply for op {}", p_replica, op));
      }
      return;
    }
    if (p_result.timestamp_ns_ <= timestamp_last_ns_)
    {
      Fail(std::format("the timestamp of op {} does not increase", op));
    }
    timestamp_last_ns_ = p_result.timestamp_ns_;
    auto &request_last = requests_last_[p_result.client_];
    if (p_result.request_ <= request_last)
    {
      Fail(std::format("client {} request {} executed more than once, at op {}", p_result.client_,
                       p_result.request_, op));
    }
    request_last = p_result.request_;
    log_.resize(op + 1);
    log_[op] = p_result;
    executed_count_ += 1;
  }

private:
  // Indexed by operation number.
  std::vector<std::optional<Committed>> log_;
  std::vector<std::uint64_t> executed_ops_;
  std::unordered_map<std::uint64_t, std::uint64_t> requests_last_;
  std::optional<std::uint64_t> initial_digest_;
  std::uint64_t timestamp_last_ns_ = 0;
  std::uint64_t executed_count_ = 0;
  std::string failure_;
};

// The default application. Its state is a hash of every operation that it
// executed. Two replicas have equal hashes only if they executed the same
// operations in the same order.
class HashApplication final : public Application
{
public:
  [[nodiscard]] std::size_t Execute(const Operation &p_operation, std::span<const std::byte> p_body,
                                    std::span<std::byte> p_reply) override
  {
    const std::array<std::uint64_t, 6> input{
        state_hash_,         p_operation.op_,      p_operation.timestamp_ns_,
        p_operation.client_, p_operation.request_, Checksum(p_body)};
    state_hash_ = Checksum(std::as_bytes(std::span{input}));
    VSR_ASSERT(p_reply.size() >= kHashReplyBytes);
    std::memcpy(p_reply.data(), &state_hash_, sizeof(state_hash_));
    std::memcpy(p_reply.data() + sizeof(state_hash_), &p_operation.op_, sizeof(p_operation.op_));
    return kHashReplyBytes;
  }

  [[nodiscard]] std::expected<std::size_t, std::monostate>
  Snapshot(std::span<std::byte> p_out) override
  {
    if (p_out.size() < kHashSnapshotBytes)
    {
      return std::unexpected(std::monostate{});
    }
    std::memcpy(p_out.data(), &state_hash_, sizeof(state_hash_));
    for (std::size_t index = 0; index < kHashSnapshotPaddingBytes; ++index)
    {
      p_out[sizeof(state_hash_) + index] = PaddingByte(index);
    }
    return kHashSnapshotBytes;
  }

  [[nodiscard]] bool Restore(std::span<const std::byte> p_snapshot) override
  {
    if (p_snapshot.size() != kHashSnapshotBytes)
    {
      return false;
    }
    std::memcpy(&state_hash_, p_snapshot.data(), sizeof(state_hash_));
    for (std::size_t index = 0; index < kHashSnapshotPaddingBytes; ++index)
    {
      if (p_snapshot[sizeof(state_hash_) + index] != PaddingByte(index))
      {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::uint64_t Digest() const override { return state_hash_; }

private:
  [[nodiscard]] std::byte PaddingByte(std::size_t p_index) const noexcept
  {
    return static_cast<std::byte>((state_hash_ >> (p_index % 8 * 8)) & 0xFFU);
  }

  std::uint64_t state_hash_ = 0;
};

// The state machine that a replica gets. It passes each call to the
// application, and gives each result to the checker.
class CheckedStateMachine final : public StateMachine
{
public:
  CheckedStateMachine(Checker *p_checker, std::size_t p_replica,
                      const ApplicationFactory *p_factory)
      : checker_(p_checker), replica_(p_replica), factory_(p_factory), application_((*p_factory)())
  {
    VSR_ASSERT(application_ != nullptr);
    checker_->OnStart(replica_, application_->Digest());
  }

  [[nodiscard]] std::size_t Execute(const Operation &p_operation, std::span<const std::byte> p_body,
                                    std::span<std::byte> p_reply) override
  {
    std::size_t reply_bytes = application_->Execute(p_operation, p_body, p_reply);
    if (reply_bytes > p_reply.size())
    {
      checker_->Fail(std::format("the reply for op {} is larger than the limit", p_operation.op_));
      reply_bytes = 0;
    }
    last_op_ = p_operation.op_;

    Committed result;
    result.client_ = p_operation.client_;
    result.request_ = p_operation.request_;
    result.timestamp_ns_ = p_operation.timestamp_ns_;
    result.digest_ = application_->Digest();
    result.reply_checksum_ = Checksum(p_reply.first(reply_bytes));
    checker_->OnExecute(replica_, p_operation, result);
    return reply_bytes;
  }

  [[nodiscard]] std::expected<std::size_t, std::monostate>
  Snapshot(std::span<std::byte> p_out) override
  {
    if (p_out.size() < kSnapshotHeaderBytes)
    {
      return std::unexpected(std::monostate{});
    }
    const auto state_bytes = application_->Snapshot(p_out.subspan(kSnapshotHeaderBytes));
    if (!state_bytes)
    {
      return std::unexpected(std::monostate{});
    }
    std::memcpy(p_out.data(), &last_op_, sizeof(last_op_));
    CheckSnapshot(p_out.subspan(kSnapshotHeaderBytes, *state_bytes));
    return kSnapshotHeaderBytes + *state_bytes;
  }

  [[nodiscard]] bool Restore(std::span<const std::byte> p_snapshot) override
  {
    if (p_snapshot.size() < kSnapshotHeaderBytes)
    {
      return false;
    }
    if (!application_->Restore(p_snapshot.subspan(kSnapshotHeaderBytes)))
    {
      return false;
    }
    std::memcpy(&last_op_, p_snapshot.data(), sizeof(last_op_));
    checker_->OnRestore(replica_, last_op_, application_->Digest());
    return true;
  }

private:
  // Each snapshot must give the same state in a new application. This also
  // checks the snapshots that no replica installs.
  void CheckSnapshot(std::span<const std::byte> p_state)
  {
    const auto copy = (*factory_)();
    if (!copy->Restore(p_state) || copy->Digest() != application_->Digest())
    {
      checker_->Fail(std::format("replica {}: the snapshot after op {} does not restore the "
                                 "same state",
                                 replica_, last_op_));
    }
  }

  Checker *checker_;
  std::size_t replica_;
  const ApplicationFactory *factory_;
  std::unique_ptr<Application> application_;
  std::uint64_t last_op_ = 0;
};

struct Packet
{
  std::uint64_t deliver_tick_ = 0;
  std::uint64_t sequence_ = 0;
  bool to_client_ = false;
  // A request that the host gives to `Replica::Submit`.
  bool submit_ = false;
  // The host already sent this request on to the primary.
  bool forwarded_ = false;
  std::uint64_t source_replica_ = 0;
  std::uint64_t target_ = 0;
  std::vector<std::byte> bytes_;
};

struct PacketLater
{
  [[nodiscard]] bool operator()(const Packet &p_left, const Packet &p_right) const noexcept
  {
    return std::pair{p_left.deliver_tick_, p_left.sequence_} >
           std::pair{p_right.deliver_tick_, p_right.sequence_};
  }
};

struct Client
{
  std::uint64_t id_ = 0;
  std::uint64_t request_ = 0;
  std::vector<std::byte> body_;
  std::uint64_t session_ = 0;
  bool registered_ = false;
  bool waiting_ = false;
  std::uint64_t next_send_tick_ = 0;
  std::uint64_t retry_tick_ = 0;
  // Operations committed before the request was first sent. The request
  // must commit after them.
  std::uint64_t committed_before_ = 0;
  std::uint32_t view_hint_ = 0;
};

class Cluster;

class SimBus final : public MessageBus
{
public:
  SimBus(Cluster *p_cluster, std::uint8_t p_replica) : cluster_(p_cluster), replica_(p_replica) {}

  void SendToReplica(std::uint8_t p_replica, std::span<const std::byte> p_message) override;
  void SendToClient(std::uint64_t p_client, std::span<const std::byte> p_message) override;

private:
  Cluster *cluster_;
  std::uint8_t replica_;
};

struct Node
{
  Node(Cluster *p_cluster, std::uint8_t p_replica, std::uint64_t p_storage_bytes)
      : storage_(p_storage_bytes), bus_(p_cluster, p_replica)
  {
  }

  SimStorage storage_;
  SimBus bus_;
  std::unique_ptr<CheckedStateMachine> state_machine_;
  std::optional<Replica> replica_;
  std::uint64_t restart_tick_ = 0;
  std::uint64_t clock_skew_ns_ = 0;
  bool isolated_ = false;
};

class Cluster final
{
public:
  explicit Cluster(const Options &p_options)
      : options_(p_options), config_(SimulationConfig(p_options, 0)),
        factory_(p_options.application_factory_), random_(p_options.seed_),
        checker_(p_options.replica_count_)
  {
    if (!factory_)
    {
      factory_ = [] { return std::make_unique<HashApplication>(); };
    }
    for (std::uint8_t replica = 0; replica < options_.replica_count_; ++replica)
    {
      const Config config = SimulationConfig(options_, replica);
      nodes_.push_back(std::make_unique<Node>(this, replica, StorageBytes(config)));
      nodes_.back()->clock_skew_ns_ = random_.Below(options_.clock_skew_ns_max_ + 1);
      if (!Format(nodes_.back()->storage_, config))
      {
        checker_.Fail("format failed");
      }
    }
    for (std::uint32_t index = 0; index < options_.client_count_; ++index)
    {
      Client client;
      client.id_ = client_id_next_++;
      client_indexes_[client.id_] = clients_.size();
      clients_.push_back(std::move(client));
    }
  }

  [[nodiscard]] Result Run()
  {
    for (std::size_t replica = 0; replica < nodes_.size(); ++replica)
    {
      Start(replica);
    }
    const std::uint64_t total_ticks = options_.chaos_ticks_ + options_.heal_ticks_;
    // Clients stop before the end, so that the replicas can reach one state.
    const std::uint64_t quiet_tick = options_.chaos_ticks_ + options_.heal_ticks_ * 2 / 3;
    for (tick_ = 1; tick_ <= total_ticks && checker_.Failure().empty(); ++tick_)
    {
      chaos_ = tick_ <= options_.chaos_ticks_;
      if (chaos_)
      {
        InjectFaults();
      }
      else if (tick_ == options_.chaos_ticks_ + 1)
      {
        Heal();
      }
      RestartDueNodes();
      DeliverPackets();
      StepClients(tick_ <= quiet_tick);
      TickNodes();
    }
    if (checker_.Failure().empty())
    {
      CheckLiveness();
    }
    return MakeResult();
  }

  void SendToReplica(std::uint8_t p_source, std::uint8_t p_target,
                     std::span<const std::byte> p_message)
  {
    CheckDurableBeforePromise(p_source, p_message);
    Packet packet;
    packet.source_replica_ = p_source;
    packet.target_ = p_target;
    packet.bytes_.assign(p_message.begin(), p_message.end());
    Enqueue(std::move(packet));
  }

  void SendToClient(std::uint64_t p_client, std::span<const std::byte> p_message)
  {
    Packet packet;
    packet.to_client_ = true;
    packet.target_ = p_client;
    packet.bytes_.assign(p_message.begin(), p_message.end());
    Enqueue(std::move(packet));
  }

private:
  // A PrepareOk and a DoViewChange promise that the sender holds the log up
  // to an operation. The promise is false after a power loss if one of
  // these entries is not yet synced.
  void CheckDurableBeforePromise(std::uint8_t p_source, std::span<const std::byte> p_message)
  {
    VSR_ASSERT(p_message.size() >= kHeaderBytes);
    Header header;
    std::memcpy(&header, p_message.data(), kHeaderBytes);
    const bool is_promise =
        header.command_ == Command::PrepareOk || header.command_ == Command::DoViewChange;
    if (!is_promise)
    {
      return;
    }
    if (nodes_[p_source]->storage_.HasUnsyncedEntryUpTo(header.op_))
    {
      checker_.Fail(std::format("replica {} sent command {} for op {} before the log was synced",
                                p_source, std::to_underlying(header.command_), header.op_));
    }
    // A DoViewChange also promises to ignore older views, and a PrepareOk
    // says that the log belongs to the view. Both must survive a restart.
    DurableView durable{&nodes_[p_source]->storage_};
    const auto superblock = internal::ReadSuperblock(durable);
    const bool view_is_durable =
        superblock.has_value() &&
        (header.command_ == Command::DoViewChange ? superblock->view_ >= header.view_
                                                  : superblock->log_view_ >= header.view_);
    if (!view_is_durable)
    {
      checker_.Fail(
          std::format("replica {} sent command {} for view {} before the view was durable",
                      p_source, std::to_underlying(header.command_), header.view_));
    }
  }

  // An operation is committed only if a quorum holds it on durable storage,
  // in the log or in a checkpoint. The check runs when the first replica
  // executes the operation.
  void CheckCommittedOpsAreDurable()
  {
    const auto layout = internal::MakeLayout(config_);
    std::vector<std::byte> slot(layout.slot_bytes_);
    while (durable_checked_ops_ < checker_.HighestOp())
    {
      durable_checked_ops_ += 1;
      const std::uint64_t op = durable_checked_ops_;
      if (!checker_.Has(op))
      {
        continue;
      }
      const auto &committed = checker_.At(op);
      std::uint32_t holder_count = 0;
      for (const auto &node : nodes_)
      {
        DurableView durable{&node->storage_};
        const auto superblock = internal::ReadSuperblock(durable);
        const bool in_checkpoint = superblock.has_value() && superblock->checkpoint_op_ >= op;
        node->storage_.ReadDurable(layout.SlotOffset(op), slot);
        const auto entry = DecodeEntry(slot);
        const bool in_log = entry.has_value() && entry->header_.op_ == op &&
                            entry->header_.client_ == committed.client_ &&
                            entry->header_.request_ == committed.request_ &&
                            entry->header_.timestamp_ns_ == committed.timestamp_ns_;
        holder_count += in_checkpoint || in_log ? 1U : 0U;
      }
      if (holder_count < options_.replica_count_ / 2U + 1U)
      {
        checker_.Fail(
            std::format("op {} executed, but only {} replicas hold it durably", op, holder_count));
      }
    }
  }

  void Enqueue(Packet p_packet)
  {
    if (chaos_ && random_.Chance(options_.drop_rate_))
    {
      return;
    }
    const std::uint32_t delay_max = chaos_ ? options_.delay_ticks_max_ : 1;
    const bool duplicate = chaos_ && random_.Chance(options_.duplicate_rate_);
    if (duplicate)
    {
      Packet copy = p_packet;
      copy.deliver_tick_ = tick_ + 1 + random_.Below(delay_max);
      copy.sequence_ = sequence_next_++;
      packets_.push(std::move(copy));
    }
    p_packet.deliver_tick_ = tick_ + 1 + random_.Below(delay_max);
    p_packet.sequence_ = sequence_next_++;
    packets_.push(std::move(p_packet));
  }

  void Start(std::size_t p_replica)
  {
    auto &node = *nodes_[p_replica];
    node.state_machine_ = std::make_unique<CheckedStateMachine>(&checker_, p_replica, &factory_);
    const Config config = SimulationConfig(options_, static_cast<std::uint8_t>(p_replica));
    auto replica = Replica::Open(config, node.storage_, node.bus_, *node.state_machine_);
    if (!replica)
    {
      checker_.Fail(std::format("replica {} did not open: error {}", p_replica,
                                std::to_underlying(replica.error())));
      return;
    }
    node.replica_.emplace(std::move(*replica));
    node.restart_tick_ = 0;
  }

  void Crash(std::size_t p_replica)
  {
    auto &node = *nodes_[p_replica];
    VSR_ASSERT(node.replica_.has_value());
    Collect(*node.replica_);
    node.replica_.reset();
    node.state_machine_.reset();
    node.storage_.Crash(random_);
    node.restart_tick_ = tick_ + 1 + random_.Below(kRestartDelayTicksMax);
    crashes_ += 1;
  }

  void Collect(const Replica &p_replica)
  {
    view_max_ = std::max<std::uint64_t>(view_max_, p_replica.View());
    checkpoints_ += p_replica.GetStats().checkpoints_;
    snapshots_installed_ += p_replica.GetStats().snapshots_installed_;
    protocol_violations_ += p_replica.GetStats().protocol_violations_;
    messages_invalid_ += p_replica.GetStats().messages_invalid_;
  }

  // A replica stops itself when the storage fails. With an injected fault
  // that is a power loss. Each other reason is a defect.
  void CheckNode(std::size_t p_replica)
  {
    CheckCommittedOpsAreDurable();
    auto &node = *nodes_[p_replica];
    if (!node.replica_ || node.replica_->GetStatus() != Status::Halted)
    {
      return;
    }
    const bool power_loss =
        node.storage_.Failing() && node.replica_->GetHaltReason() == HaltReason::StorageFault;
    if (!power_loss)
    {
      checker_.Fail(std::format("replica {} halted: reason {}", p_replica,
                                std::to_underlying(node.replica_->GetHaltReason())));
      return;
    }
    Crash(p_replica);
  }

  void InjectFaults()
  {
    for (std::size_t replica = 0; replica < nodes_.size(); ++replica)
    {
      auto &node = *nodes_[replica];
      if (!node.replica_)
      {
        continue;
      }
      if (random_.Chance(options_.crash_rate_))
      {
        Crash(replica);
      }
      else if (random_.Chance(options_.storage_fault_rate_))
      {
        node.storage_.InjectFault(1 +
                                  static_cast<std::uint32_t>(random_.Below(kStorageFaultOpsMax)));
      }
    }
    if (random_.Chance(options_.partition_rate_))
    {
      const bool any_isolated =
          std::ranges::any_of(nodes_, [](const auto &p_node) { return p_node->isolated_; });
      for (auto &node : nodes_)
      {
        node->isolated_ = false;
      }
      if (!any_isolated)
      {
        nodes_[random_.Below(nodes_.size())]->isolated_ = true;
      }
    }
  }

  void Heal()
  {
    for (auto &node : nodes_)
    {
      node->isolated_ = false;
      if (!node->replica_)
      {
        node->restart_tick_ = tick_;
      }
    }
  }

  void RestartDueNodes()
  {
    for (std::size_t replica = 0; replica < nodes_.size(); ++replica)
    {
      const auto &node = *nodes_[replica];
      if (!node.replica_ && node.restart_tick_ != 0 && tick_ >= node.restart_tick_)
      {
        Start(replica);
      }
    }
  }

  void DeliverPackets()
  {
    while (!packets_.empty() && packets_.top().deliver_tick_ <= tick_)
    {
      const Packet packet = packets_.top();
      packets_.pop();
      if (packet.to_client_)
      {
        OnClientPacket(packet);
        continue;
      }
      auto &node = *nodes_[packet.target_];
      const bool link_down = node.isolated_ || nodes_[packet.source_replica_]->isolated_;
      if (!node.replica_ || (link_down && packet.source_replica_ != packet.target_))
      {
        continue;
      }
      if (packet.submit_)
      {
        SubmitRequest(packet);
      }
      else
      {
        node.replica_->OnMessage(packet.bytes_);
      }
      // A host can flush after one message or after several. The power can
      // fail before the flush.
      if (!chaos_ || random_.Below(4) != 0)
      {
        node.replica_->Flush();
      }
      else if (random_.Chance(options_.crash_rate_ * kUnflushedCrashFactor))
      {
        Crash(packet.target_);
        continue;
      }
      CheckNode(packet.target_);
    }
  }

  // The path of a host with its own client protocol: the host decodes the
  // request and calls `Submit`. A host that is not the primary sends the
  // request on to the primary one time, as a replica does for a `Request`.
  void SubmitRequest(const Packet &p_packet)
  {
    Replica &replica = *nodes_[p_packet.target_]->replica_;
    const auto message = DecodeMessage(p_packet.bytes_);
    VSR_ASSERT(message.has_value());
    const Header &header = message->header_;
    const bool was_primary = replica.IsPrimary();
    const std::uint64_t head_before = replica.Op();
    const SubmitResult result =
        replica.Submit(header.client_, header.request_, header.session_, message->body_);
    const bool appended = replica.Op() == head_before + 1;
    // A valid request is `Invalid` only when the storage fails.
    const bool consistent = result == SubmitResult::Invalid
                                ? replica.GetStatus() == Status::Halted
                                : (result == SubmitResult::NotPrimary) != was_primary &&
                                      (result == SubmitResult::Accepted) == appended;
    if (!consistent)
    {
      checker_.Fail(std::format("replica {}: Submit of client {} request {} gave result {}, "
                                "primary {}, log head {} then {}",
                                p_packet.target_, header.client_, header.request_,
                                std::to_underlying(result), was_primary, head_before,
                                replica.Op()));
    }
    submits_accepted_ += result == SubmitResult::Accepted ? 1U : 0U;
    const bool view_is_open =
        replica.GetStatus() == Status::Normal || replica.GetStatus() == Status::Joining;
    if (result == SubmitResult::NotPrimary && view_is_open && !p_packet.forwarded_)
    {
      Packet forward = p_packet;
      forward.forwarded_ = true;
      forward.source_replica_ = replica.PrimaryIndex();
      forward.target_ = replica.PrimaryIndex();
      Enqueue(std::move(forward));
    }
  }

  void SendRequest(Client &p_client, std::uint64_t p_replica)
  {
    Header header;
    header.command_ = Command::Request;
    header.cluster_ = config_.cluster_;
    header.client_ = p_client.id_;
    header.request_ = p_client.request_;
    header.session_ = p_client.session_;
    std::vector<std::byte> bytes(kHeaderBytes + p_client.body_.size());
    const std::size_t message_bytes = EncodeMessage(header, p_client.body_, {}, bytes);
    Packet packet;
    // A request has no source replica. Use the target, so that only the
    // state of the target decides the delivery.
    packet.source_replica_ = p_replica;
    packet.target_ = p_replica;
    packet.submit_ = random_.Chance(options_.submit_rate_);
    packet.bytes_.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(message_bytes));
    Enqueue(std::move(packet));
    p_client.retry_tick_ = tick_ + kClientRetryTicks;
  }

  [[nodiscard]] std::vector<std::byte> MakeBody()
  {
    if (options_.body_generator_)
    {
      auto body = options_.body_generator_(random_.Next());
      if (body.size() > config_.body_bytes_max_)
      {
        checker_.Fail("the body generator made a body that is larger than the limit");
        body.clear();
      }
      return body;
    }
    std::vector<std::byte> body(random_.Below(config_.body_bytes_max_ + 1));
    for (auto &byte : body)
    {
      byte = static_cast<std::byte>(random_.Below(256));
    }
    return body;
  }

  void StepClients(bool p_start_requests)
  {
    for (auto &client : clients_)
    {
      if (client.waiting_)
      {
        if (tick_ >= client.retry_tick_)
        {
          SendRequest(client, random_.Below(nodes_.size()));
        }
        continue;
      }
      if (!p_start_requests || tick_ < client.next_send_tick_)
      {
        continue;
      }
      // The first request of a client identifier is the registration:
      // request number 0 with no body.
      client.body_.clear();
      if (client.registered_)
      {
        client.request_ += 1;
        client.body_ = MakeBody();
      }
      client.waiting_ = true;
      client.committed_before_ = checker_.HighestOp();
      SendRequest(client, client.view_hint_ % nodes_.size());
    }
  }

  // The cluster removed the session of the client. The client cannot learn
  // the outcome of its request. It continues with a new identifier.
  void OnEviction(Client &p_client)
  {
    client_indexes_.erase(p_client.id_);
    const std::size_t index = static_cast<std::size_t>(&p_client - clients_.data());
    p_client.id_ = client_id_next_++;
    client_indexes_[p_client.id_] = index;
    p_client.request_ = 0;
    p_client.registered_ = false;
    p_client.session_ = 0;
    p_client.waiting_ = false;
    p_client.next_send_tick_ = tick_ + random_.Below(kClientThinkTicksMax);
    evictions_ += 1;
  }

  void OnClientPacket(const Packet &p_packet)
  {
    const auto message = DecodeMessage(p_packet.bytes_);
    const bool known_command =
        message.has_value() && (message->header_.command_ == Command::Reply ||
                                message->header_.command_ == Command::Eviction);
    if (!known_command)
    {
      checker_.Fail("a client received an invalid message");
      return;
    }
    const Header &header = message->header_;
    const auto found = client_indexes_.find(header.client_);
    if (found == client_indexes_.end())
    {
      // For an identifier that the client used before an eviction.
      return;
    }
    auto &client = clients_[found->second];
    if (header.command_ == Command::Eviction)
    {
      // A replica that is behind does not know a new session. Its notice
      // has an older view than a reply that this client has seen.
      if (header.view_ >= client.view_hint_)
      {
        OnEviction(client);
      }
      return;
    }
    if (!client.waiting_ || header.request_ != client.request_ ||
        (client.registered_ && header.session_ != client.session_))
    {
      return;
    }
    if (!client.registered_)
    {
      client.registered_ = true;
      client.session_ = header.session_;
    }
    else if (!CheckReply(client, *message))
    {
      return;
    }
    client.waiting_ = false;
    client.view_hint_ = std::max(client.view_hint_, header.view_);
    client.next_send_tick_ = tick_ + random_.Below(kClientThinkTicksMax);
    requests_completed_ += 1;
  }

  [[nodiscard]] bool CheckReply(const Client &p_client, const Message &p_message)
  {
    const Header &header = p_message.header_;
    if (header.op_ <= p_client.committed_before_ || !checker_.Has(header.op_))
    {
      checker_.Fail(std::format("client {} request {}: reply names op {}, which is not an "
                                "operation executed after op {}",
                                p_client.id_, p_client.request_, header.op_,
                                p_client.committed_before_));
      return false;
    }
    const auto &committed = checker_.At(header.op_);
    const bool same_request =
        committed.client_ == p_client.id_ && committed.request_ == p_client.request_;
    const bool same_reply = Checksum(p_message.body_) == committed.reply_checksum_;
    if (!same_request || !same_reply)
    {
      checker_.Fail(std::format("client {} request {}: wrong reply for op {}", p_client.id_,
                                p_client.request_, header.op_));
      return false;
    }
    return true;
  }

  void TickNodes()
  {
    for (std::size_t replica = 0; replica < nodes_.size(); ++replica)
    {
      auto &node = *nodes_[replica];
      if (!node.replica_)
      {
        continue;
      }
      node.replica_->Tick(tick_ * kTickNs + node.clock_skew_ns_);
      // A slow host flushes late. Entries then stay unsynced for some ticks,
      // while messages about them arrive.
      if (!chaos_ || random_.Below(4) != 0)
      {
        node.replica_->Flush();
      }
      CheckNode(replica);
    }
  }

  // After the faults stop, the cluster must serve every request and the
  // replicas must reach the same state.
  void CheckLiveness()
  {
    for (const auto &client : clients_)
    {
      if (client.waiting_)
      {
        checker_.Fail(std::format("liveness: client {} request {} has no reply", client.id_,
                                  client.request_));
      }
    }
    std::uint32_t primary_count = 0;
    for (std::size_t replica = 0; replica < nodes_.size(); ++replica)
    {
      const auto &node = *nodes_[replica];
      if (!node.replica_)
      {
        checker_.Fail(std::format("liveness: replica {} is down", replica));
        continue;
      }
      const Replica &current = *node.replica_;
      primary_count += current.IsPrimary() ? 1U : 0U;
      if (current.GetStatus() != Status::Normal || current.View() != nodes_[0]->replica_->View())
      {
        checker_.Fail(std::format("liveness: replica {} has status {} in view {}", replica,
                                  std::to_underlying(current.GetStatus()), current.View()));
      }
      const bool all_executed = current.CommitMin() == nodes_[0]->replica_->CommitMin() &&
                                current.CommitMin() == current.Op() &&
                                current.CommitMin() >= checker_.HighestOp();
      if (!all_executed)
      {
        checker_.Fail(std::format("liveness: replica {} executed up to op {}, log head {}, "
                                  "highest executed op {}",
                                  replica, current.CommitMin(), current.Op(),
                                  checker_.HighestOp()));
      }
    }
    if (primary_count != 1)
    {
      checker_.Fail(std::format("liveness: {} primaries", primary_count));
    }
    if (protocol_violations_ != 0)
    {
      checker_.Fail(
          std::format("{} protocol violations between correct replicas", protocol_violations_));
    }
  }

  [[nodiscard]] Result MakeResult()
  {
    for (const auto &node : nodes_)
    {
      if (node->replica_)
      {
        Collect(*node->replica_);
      }
    }
    if (checker_.Failure().empty() && protocol_violations_ != 0)
    {
      checker_.Fail(
          std::format("{} protocol violations between correct replicas", protocol_violations_));
    }
    if (checker_.Failure().empty() && messages_invalid_ != 0)
    {
      checker_.Fail(std::format("{} invalid messages between correct replicas", messages_invalid_));
    }
    Result result;
    result.ok_ = checker_.Failure().empty();
    result.failure_ = checker_.Failure();
    result.committed_ops_ = checker_.ExecutedCount();
    result.evictions_ = evictions_;
    result.requests_completed_ = requests_completed_;
    result.submits_accepted_ = submits_accepted_;
    result.crashes_ = crashes_;
    result.view_max_ = view_max_;
    result.checkpoints_ = checkpoints_;
    result.snapshots_installed_ = snapshots_installed_;
    return result;
  }

  Options options_;
  // The configuration of replica 0. Only `replica_` differs between replicas.
  Config config_;
  ApplicationFactory factory_;
  Random random_;
  Checker checker_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Client> clients_;
  std::priority_queue<Packet, std::vector<Packet>, PacketLater> packets_;
  std::uint64_t tick_ = 0;
  std::uint64_t sequence_next_ = 0;
  std::uint64_t durable_checked_ops_ = 0;
  std::uint64_t client_id_next_ = 1;
  std::uint64_t evictions_ = 0;
  std::unordered_map<std::uint64_t, std::size_t> client_indexes_;
  bool chaos_ = false;
  std::uint64_t requests_completed_ = 0;
  std::uint64_t submits_accepted_ = 0;
  std::uint64_t crashes_ = 0;
  std::uint64_t view_max_ = 0;
  std::uint64_t checkpoints_ = 0;
  std::uint64_t snapshots_installed_ = 0;
  std::uint64_t protocol_violations_ = 0;
  std::uint64_t messages_invalid_ = 0;
};

void SimBus::SendToReplica(std::uint8_t p_replica, std::span<const std::byte> p_message)
{
  cluster_->SendToReplica(replica_, p_replica, p_message);
}

void SimBus::SendToClient(std::uint64_t p_client, std::span<const std::byte> p_message)
{
  cluster_->SendToClient(p_client, p_message);
}

} // namespace

Config SimulationConfig(const Options &p_options, std::uint8_t p_replica)
{
  const Limits &limits = p_options.limits_;
  Config config;
  config.cluster_ = 0x5652'5F53'494D;
  config.replica_count_ = p_options.replica_count_;
  config.replica_ = p_replica;
  config.body_bytes_max_ = limits.body_bytes_max_;
  config.pipeline_max_ = limits.pipeline_max_;
  config.clients_max_ = limits.clients_max_;
  config.log_slot_count_ = limits.log_slot_count_;
  config.checkpoint_interval_ = limits.checkpoint_interval_;
  config.snapshot_bytes_max_ = limits.snapshot_bytes_max_ + kSnapshotHeaderBytes;
  return config;
}

Result Run(const Options &p_options)
{
  if (const auto valid = ValidateConfig(SimulationConfig(p_options, 0)); !valid)
  {
    Result result;
    result.ok_ = false;
    result.failure_ = std::format("the options are not valid: configuration error {}",
                                  std::to_underlying(valid.error()));
    return result;
  }
  Cluster cluster{p_options};
  return cluster.Run();
}

} // namespace vsr::sim
