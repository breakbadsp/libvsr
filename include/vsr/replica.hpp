#pragma once

#include "vsr/config.hpp"
#include "vsr/interfaces.hpp"
#include "vsr/internal/client_table.hpp"
#include "vsr/internal/journal.hpp"
#include "vsr/internal/layout.hpp"
#include "vsr/internal/superblock.hpp"
#include "vsr/message.hpp"

#include <array>
#include <bitset>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace vsr
{

enum class Status : std::uint8_t
{
  // The log is a prefix of the log of the primary of `View()`.
  Normal = 0,
  // The replica waits for the primary of `View()` to open the view.
  ViewChange,
  // The view is open, but the log is not yet checked against the primary.
  Joining,
  // The replica found a fault that it cannot repair. It ignores all input.
  Halted,
};

enum class HaltReason : std::uint8_t
{
  None = 0,
  StorageFault,
  // A log entry on storage failed its checksum.
  CorruptEntry,
  // The checkpoint differs from the checkpoint of the primary at the same
  // operation. The state machine is not deterministic, or data is corrupt.
  Divergence,
  SnapshotTooLarge,
  RestoreFailed,
};

enum class SubmitResult : std::uint8_t
{
  Accepted = 0,
  // The stored reply was sent again, or the request is in progress or old.
  Duplicate,
  NotPrimary,
  // The pipeline or the log is full. Submit again later.
  Busy,
  // The client is not registered, or the cluster removed its session. An
  // `Eviction` message went to the client. See docs/host-contract.md.
  NoSession,
  Invalid,
};

enum class OpenError
{
  BadConfig = 0,
  StorageFault,
  NotFormatted,
  ConfigMismatch,
  CorruptSnapshot,
  RestoreFailed,
};

struct Stats
{
  std::uint64_t messages_invalid_ = 0;
  std::uint64_t protocol_violations_ = 0;
  std::uint64_t requests_busy_ = 0;
  std::uint64_t view_changes_ = 0;
  std::uint64_t checkpoints_ = 0;
  std::uint64_t snapshots_installed_ = 0;
};

// Writes the initial superblock. The storage must hold
// `StorageBytes(p_config)` bytes. Format erases an existing replica.
[[nodiscard]] std::expected<void, OpenError> Format(Storage &p_storage, const Config &p_config);

[[nodiscard]] std::uint64_t StorageBytes(const Config &p_config);

// One replica of a Viewstamped Replication cluster.
//
// The replica has no thread, socket, file, or clock. The host calls
// `OnMessage`, `Submit`, and `Tick`, and then `Flush`. The replica calls the
// storage, the message bus, and the state machine during these calls. With
// the same sequence of calls and the same results from the three
// interfaces, the replica does the same thing.
class Replica final
{
public:
  // Reads the durable state. The state machine must be in its initial state.
  // The three objects must live longer than the replica.
  [[nodiscard]] static std::expected<Replica, OpenError> Open(const Config &p_config,
                                                              Storage &p_storage, MessageBus &p_bus,
                                                              StateMachine &p_state_machine);

  // Advances the timers by one tick. `p_realtime_ns` is the wall clock of
  // this replica. A primary uses it for the timestamps of operations.
  void Tick(std::uint64_t p_realtime_ns);

  // Handles one message from a replica or a client. The bytes are not trusted.
  void OnMessage(std::span<const std::byte> p_message);

  // Submits a request from a host that has its own client protocol. The
  // reply goes to `MessageBus::SendToClient` when the operation commits.
  // Request number 0 with an empty body registers the client. A client
  // sends the registration reply session_ with every operation.
  SubmitResult Submit(std::uint64_t p_client, std::uint64_t p_request, std::uint64_t p_session,
                      std::span<const std::byte> p_body);

  // Makes the new log entries durable, then acknowledges them. Call it after
  // each batch of `OnMessage` and `Submit` calls. One sync covers the batch.
  void Flush();

  [[nodiscard]] Status GetStatus() const noexcept { return status_; }

  [[nodiscard]] HaltReason GetHaltReason() const noexcept { return halt_reason_; }

  [[nodiscard]] std::uint32_t View() const noexcept { return view_; }

  [[nodiscard]] std::uint32_t LogView() const noexcept { return log_view_; }

  [[nodiscard]] std::uint8_t PrimaryIndex() const noexcept { return PrimaryOf(view_); }

  [[nodiscard]] bool IsPrimary() const noexcept
  {
    return status_ == Status::Normal && PrimaryIndex() == config_.replica_;
  }

  // The last operation in the log.
  [[nodiscard]] std::uint64_t Op() const noexcept { return journal_.Head(); }

  // The last operation that the state machine executed.
  [[nodiscard]] std::uint64_t CommitMin() const noexcept { return commit_min_; }

  // The last operation that this replica knows is committed.
  [[nodiscard]] std::uint64_t CommitMax() const noexcept { return commit_max_; }

  [[nodiscard]] std::uint64_t CheckpointOp() const noexcept { return superblock_.checkpoint_op_; }

  [[nodiscard]] const Stats &GetStats() const noexcept { return stats_; }

private:
  struct DoViewChangeRecord
  {
    bool present_ = false;
    std::uint32_t log_view_ = 0;
    std::uint64_t op_ = 0;
    std::uint64_t checksum_ = 0;
    std::uint64_t commit_ = 0;
  };

  struct SnapshotSync
  {
    bool active_ = false;
    std::uint64_t checkpoint_op_ = 0;
    std::uint64_t entry_checksum_ = 0;
    std::uint64_t checksum_ = 0;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t received_bytes_ = 0;
  };

  Replica(const Config &p_config, Storage &p_storage, MessageBus &p_bus,
          StateMachine &p_state_machine, const internal::Superblock &p_superblock);

  [[nodiscard]] std::uint8_t PrimaryOf(std::uint32_t p_view) const noexcept
  {
    return static_cast<std::uint8_t>(p_view % config_.replica_count_);
  }

  [[nodiscard]] std::uint32_t Quorum() const noexcept { return config_.replica_count_ / 2U + 1U; }

  [[nodiscard]] bool Fetching() const noexcept
  {
    return status_ == Status::ViewChange && fetching_;
  }

  [[nodiscard]] std::expected<void, OpenError> Recover();
  void Halt(HaltReason p_reason);
  [[nodiscard]] bool PersistSuperblock();
  [[nodiscard]] bool SyncJournal();
  [[nodiscard]] std::uint64_t TimeoutWithJitter(std::uint32_t p_ticks);

  void Send(std::uint8_t p_replica, Header p_header, std::span<const std::byte> p_body_first = {},
            std::span<const std::byte> p_body_second = {});
  void Broadcast(const Header &p_header, std::span<const std::byte> p_body_first = {},
                 std::span<const std::byte> p_body_second = {});
  [[nodiscard]] Header MakeHeader(Command p_command) const;
  [[nodiscard]] Header MakeHeadHeader(Command p_command) const;
  void SendPrepare(std::uint8_t p_replica, std::uint64_t p_op);
  void SendPrepareOk();
  void SendReply(const internal::ClientEntry &p_entry);
  void SendEviction(std::uint64_t p_client);
  void SendDoViewChange();
  void MaybeSendDoViewChange();
  void SendSnapshotChunk(std::uint8_t p_replica, std::uint64_t p_offset);
  void RequestPrepare(std::uint64_t p_op);
  void RequestSnapshotChunk();

  SubmitResult HandleRequest(std::uint64_t p_client, std::uint64_t p_request,
                             std::uint64_t p_session, std::span<const std::byte> p_body);
  void OnRequest(const Message &p_message);
  void OnPrepare(const Message &p_message);
  void OnPrepareOk(const Header &p_header);
  void OnCommit(const Header &p_header);
  void OnStartViewChange(const Header &p_header);
  void OnDoViewChange(const Header &p_header);
  void OnRequestPrepare(const Header &p_header);
  void OnRequestSnapshot(const Header &p_header);
  void OnSnapshot(const Message &p_message);

  [[nodiscard]] bool FollowPrimary(const Header &p_header);
  void EnterView(std::uint32_t p_view);
  void Follow();
  void OnVerified();
  void CheckFollowDone();
  void BecomeNormal();
  [[nodiscard]] bool TruncateTo(std::uint64_t p_op);
  [[nodiscard]] bool AppendEntry(const Entry &p_entry);
  void HandleEntryUnverified(const Entry &p_entry);
  void HandleEntryNormal(const Entry &p_entry);
  void LearnCommit(const Header &p_header);

  void StartViewChange(std::uint32_t p_view);
  void BeginViewChangeRound();
  void ResetViewChange();
  void CheckViewChangeQuorum();
  void FinishViewChange();

  void AdvanceCommit();
  void ExecuteCommitted();
  void ExecuteOne(std::uint64_t p_op);
  void Checkpoint();
  void InstallSnapshot();
  [[nodiscard]] bool LoadSnapshotBlob(std::span<const std::byte> p_blob);

  void TickPrimary(bool p_retry_due);
  void TickBackup(bool p_retry_due);
  void TickViewChange(bool p_retry_due);

  Config config_;
  internal::Layout layout_;
  // Pointers, not references, so that a replica can be moved.
  Storage *storage_;
  MessageBus *bus_;
  StateMachine *state_machine_;

  internal::Superblock superblock_;
  internal::Journal journal_;
  internal::ClientTable clients_;

  Status status_ = Status::ViewChange;
  HaltReason halt_reason_ = HaltReason::None;
  std::uint32_t view_ = 0;
  std::uint32_t log_view_ = 0;
  std::uint64_t commit_min_ = 0;
  std::uint64_t commit_max_ = 0;
  // The last operation that is durable in the log.
  std::uint64_t op_synced_ = 0;
  std::uint64_t prepare_ok_sent_op_ = 0;
  std::uint64_t timestamp_last_ns_ = 0;
  std::uint64_t timestamp_committed_ns_ = 0;

  // Primary: the highest operation that each replica holds in this view.
  std::array<std::uint64_t, kReplicasMax> acked_ops_{};

  // View change.
  std::bitset<kReplicasMax> start_view_change_from_;
  std::array<DoViewChangeRecord, kReplicasMax> do_view_changes_{};
  bool do_view_change_sent_ = false;
  std::uint64_t view_commit_ = 0;
  // The new primary copies the best log from `fetch_source_`.
  bool fetching_ = false;
  std::uint8_t fetch_source_ = 0;

  // True if the log is a prefix of the log that this replica follows: the
  // log of the primary, or of `fetch_source_`. Not durable.
  bool log_verified_ = false;
  // The head of the log of the primary when it opened `view_`. A replica is
  // Normal in a view only if its log holds all operations up to this one.
  bool view_start_known_ = false;
  std::uint64_t view_start_op_ = 0;

  // The head of the log that this replica follows. It can be out of date.
  bool target_known_ = false;
  std::uint64_t target_op_ = 0;
  std::uint64_t target_checksum_ = 0;

  SnapshotSync sync_;

  std::uint64_t ticks_ = 0;
  std::uint64_t realtime_ns_ = 0;
  std::uint64_t heartbeat_deadline_tick_ = 0;
  std::uint64_t retry_deadline_tick_ = 0;
  std::uint64_t primary_deadline_tick_ = 0;
  std::uint64_t view_change_deadline_tick_ = 0;
  std::uint64_t request_prepare_op_ = 0;
  std::uint64_t request_prepare_tick_ = 0;
  std::uint64_t random_state_ = 0;

  std::vector<std::byte> message_buffer_;
  std::vector<std::byte> body_buffer_;
  std::vector<std::byte> reply_buffer_;
  std::vector<std::byte> snapshot_buffer_;

  Stats stats_;
};

} // namespace vsr
