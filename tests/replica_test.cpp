#include "test_support.hpp"
#include "vsr/replica.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>

// A cluster of one replica shows the contract with the host: requests,
// replies, duplicates, limits, and what survives a restart. The simulation
// tests cover clusters of several replicas.

namespace vsr
{

namespace
{

using test::Bytes;

[[nodiscard]] Config SmallConfig()
{
  Config config;
  config.cluster_ = 42;
  config.replica_count_ = 1;
  config.replica_ = 0;
  config.body_bytes_max_ = 64;
  config.pipeline_max_ = 2;
  config.clients_max_ = 4;
  config.log_slot_count_ = 8;
  config.checkpoint_interval_ = 4;
  config.snapshot_bytes_max_ = 1024;
  return config;
}

class SingleReplica : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ASSERT_TRUE(Format(storage_, config_).has_value());
    Start();
  }

  // Opens the replica on the same storage with a new state machine, as a
  // process does after a restart.
  void Start()
  {
    replica_.reset();
    state_machine_ = std::make_unique<test::KeyValueStateMachine>();
    bus_.replies_.clear();
    auto opened = Replica::Open(config_, storage_, bus_, *state_machine_);
    ASSERT_TRUE(opened.has_value());
    replica_.emplace(std::move(*opened));
  }

  // Gives the client a session, as each client must do before its first
  // operation. The reply to a registration is not kept.
  void Register(std::uint64_t p_client)
  {
    ASSERT_EQ(replica_->Submit(p_client, 0, 0, {}), SubmitResult::Accepted);
    replica_->Flush();
    ASSERT_FALSE(bus_.replies_.empty());
    ASSERT_EQ(bus_.replies_.back().client_, p_client);
    ASSERT_EQ(bus_.replies_.back().request_, 0U);
    sessions_[p_client] = bus_.replies_.back().op_;
    bus_.replies_.pop_back();
  }

  // One request and one flush. Returns the reply body.
  [[nodiscard]] std::optional<std::string> Request(std::uint64_t p_client, std::uint64_t p_request,
                                                   std::string_view p_body)
  {
    const std::size_t reply_count = bus_.replies_.size();
    if (replica_->Submit(p_client, p_request, sessions_[p_client], Bytes(p_body)) !=
        SubmitResult::Accepted)
    {
      return std::nullopt;
    }
    replica_->Flush();
    if (bus_.replies_.size() != reply_count + 1)
    {
      return std::nullopt;
    }
    return bus_.replies_.back().body_;
  }

  std::map<std::uint64_t, std::uint64_t> sessions_;
  Config config_ = SmallConfig();
  test::MemoryStorage storage_{StorageBytes(config_)};
  test::RecordingBus bus_;
  std::unique_ptr<test::KeyValueStateMachine> state_machine_;
  std::optional<Replica> replica_;
};

} // namespace

// Storage that was not formatted, or that was formatted with other limits,
// does not open.
TEST(ReplicaOpen, RefusesWrongStorage)
{
  const Config config = SmallConfig();
  test::MemoryStorage storage{StorageBytes(config)};
  test::RecordingBus bus;
  test::KeyValueStateMachine state_machine;
  const auto unformatted = Replica::Open(config, storage, bus, state_machine);
  ASSERT_FALSE(unformatted.has_value());
  EXPECT_EQ(unformatted.error(), OpenError::NotFormatted);

  ASSERT_TRUE(Format(storage, config).has_value());
  Config other = config;
  other.body_bytes_max_ = 32;
  const auto mismatch = Replica::Open(other, storage, bus, state_machine);
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(mismatch.error(), OpenError::ConfigMismatch);

  Config invalid = config;
  invalid.log_slot_count_ = 2;
  const auto bad = Replica::Open(invalid, storage, bus, state_machine);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), OpenError::BadConfig);
}

// A replica that is alone is the primary, and each request gets the reply of
// the state machine, in order.
TEST_F(SingleReplica, ExecutesRequestsInOrder)
{
  EXPECT_TRUE(replica_->IsPrimary());
  Register(1);
  Register(2);
  EXPECT_EQ(Request(1, 1, "a=1"), "");
  EXPECT_EQ(Request(1, 2, "a=2"), "1");
  EXPECT_EQ(Request(2, 1, "a=3"), "2");
  EXPECT_EQ(bus_.replies_.back().client_, 2U);
  EXPECT_EQ(bus_.replies_.back().request_, 1U);
  // Two registrations and three operations.
  EXPECT_EQ(bus_.replies_.back().op_, 5U);
  EXPECT_EQ(replica_->CommitMin(), 5U);
}

// A request that is sent again does not execute again. The client gets the
// stored reply. An older request gets nothing.
TEST_F(SingleReplica, DuplicateRequestGetsStoredReply)
{
  Register(1);
  EXPECT_EQ(Request(1, 1, "a=1"), "");
  EXPECT_EQ(Request(1, 2, "a=2"), "1");
  const std::size_t reply_count = bus_.replies_.size();

  EXPECT_EQ(replica_->Submit(1, 2, sessions_[1], Bytes("a=2")), SubmitResult::Duplicate);
  ASSERT_EQ(bus_.replies_.size(), reply_count + 1);
  EXPECT_EQ(bus_.replies_.back().body_, "1");
  EXPECT_EQ(bus_.replies_.back().op_, 3U);

  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes("a=1")), SubmitResult::Duplicate);
  EXPECT_EQ(bus_.replies_.size(), reply_count + 1);
  EXPECT_EQ(replica_->Op(), 3U);
  EXPECT_EQ(state_machine_->values_.at("a"), "2");
}

// A request that is in the log but not yet durable is a duplicate too.
TEST_F(SingleReplica, DuplicateOfPendingRequestIsNotPreparedAgain)
{
  Register(1);
  const std::uint64_t op_before = replica_->Op();
  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes("a=1")), SubmitResult::Accepted);
  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes("a=1")), SubmitResult::Duplicate);
  replica_->Flush();
  EXPECT_EQ(replica_->Op(), op_before + 1);
  EXPECT_EQ(bus_.replies_.size(), 1U);
}

// No reply leaves the replica before the operation is durable, and one sync
// makes a batch of operations durable.
TEST_F(SingleReplica, RepliesOnlyAfterFlushAndSyncsOncePerBatch)
{
  Register(1);
  Register(2);
  Register(3);
  // Operation 4 takes a checkpoint, which has its own syncs. The batch
  // below is operations 5 and 6.
  ASSERT_TRUE(Request(3, 1, "c=1").has_value());
  ASSERT_EQ(replica_->CheckpointOp(), 4U);
  bus_.replies_.clear();
  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes("a=1")), SubmitResult::Accepted);
  EXPECT_EQ(replica_->Submit(2, 1, sessions_[2], Bytes("b=1")), SubmitResult::Accepted);
  EXPECT_TRUE(bus_.replies_.empty());
  const std::uint64_t sync_count = storage_.sync_count_;
  replica_->Flush();
  EXPECT_EQ(storage_.sync_count_, sync_count + 1);
  EXPECT_EQ(bus_.replies_.size(), 2U);
}

// The pipeline limit and the request limits are refused, and nothing executes.
TEST_F(SingleReplica, RefusesRequestsOverLimits)
{
  Register(1);
  Register(2);
  Register(3);
  EXPECT_EQ(replica_->Submit(0, 1, sessions_[0], Bytes("a=1")), SubmitResult::Invalid);
  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes(std::string(65, 'x'))),
            SubmitResult::Invalid);
  // A registration has no body.
  EXPECT_EQ(replica_->Submit(4, 0, 0, Bytes("a=1")), SubmitResult::Invalid);

  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes("a=1")), SubmitResult::Accepted);
  EXPECT_EQ(replica_->Submit(2, 1, sessions_[2], Bytes("b=1")), SubmitResult::Accepted);
  EXPECT_EQ(replica_->Submit(3, 1, sessions_[3], Bytes("c=1")), SubmitResult::Busy);
  replica_->Flush();
  EXPECT_EQ(replica_->Submit(3, 1, sessions_[3], Bytes("c=1")), SubmitResult::Accepted);
  replica_->Flush();
  EXPECT_EQ(state_machine_->values_.size(), 3U);
}

// The timestamp of each operation is larger than the one before it, also
// when the wall clock of the host goes back.
TEST_F(SingleReplica, TimestampsIncreaseWhenClockGoesBack)
{
  Register(1);
  replica_->Tick(5000);
  EXPECT_EQ(Request(1, 1, "a=1"), "");
  const std::uint64_t first_ns = state_machine_->last_timestamp_ns_;
  EXPECT_GE(first_ns, 5000U);
  replica_->Tick(1000);
  EXPECT_EQ(Request(1, 2, "a=2"), "1");
  EXPECT_GT(state_machine_->last_timestamp_ns_, first_ns);
}

// After a restart before the first checkpoint, the replica executes the log
// again and has the same state. A duplicate still gets the stored reply.
TEST_F(SingleReplica, RestartReplaysTheLog)
{
  Register(1);
  EXPECT_EQ(Request(1, 1, "a=1"), "");
  EXPECT_EQ(Request(1, 2, "b=2"), "");
  ASSERT_EQ(replica_->CheckpointOp(), 0U);
  const std::uint64_t commit_before = replica_->CommitMin();

  Start();
  replica_->Flush();
  EXPECT_TRUE(replica_->IsPrimary());
  EXPECT_EQ(replica_->CommitMin(), commit_before);
  EXPECT_EQ(state_machine_->values_.at("a"), "1");
  EXPECT_EQ(state_machine_->values_.at("b"), "2");

  bus_.replies_.clear();
  EXPECT_EQ(replica_->Submit(1, 2, sessions_[1], Bytes("b=2")), SubmitResult::Duplicate);
  ASSERT_EQ(bus_.replies_.size(), 1U);
  EXPECT_EQ(bus_.replies_.back().body_, "");
  EXPECT_EQ(Request(1, 3, "b=3"), "2");
}

// After a restart, the state comes from the checkpoint and the log after it.
// The log ring is smaller than the number of operations, so the old entries
// are gone and only the snapshot can give their effect.
TEST_F(SingleReplica, RestartRestoresTheCheckpoint)
{
  constexpr std::uint64_t kRequestCount = 22;
  Register(1);
  for (std::uint64_t request = 1; request <= kRequestCount; ++request)
  {
    const auto reply =
        Request(1, request, "k" + std::to_string(request % 5) + "=" + std::to_string(request));
    ASSERT_TRUE(reply.has_value()) << request;
  }
  ASSERT_GT(kRequestCount, config_.log_slot_count_);
  EXPECT_EQ(replica_->CheckpointOp(), 20U);
  const auto values_before = state_machine_->values_;
  const std::uint64_t commit_before = replica_->CommitMin();

  Start();
  replica_->Flush();
  EXPECT_EQ(replica_->CommitMin(), commit_before);
  EXPECT_EQ(state_machine_->values_, values_before);
  EXPECT_EQ(Request(1, kRequestCount + 1, "k0=end"), "20");
}

// Format erases a replica: no operation of the old replica comes back.
TEST_F(SingleReplica, FormatErasesOldLog)
{
  Register(1);
  EXPECT_EQ(Request(1, 1, "a=1"), "");
  replica_.reset();
  ASSERT_TRUE(Format(storage_, config_).has_value());
  Start();
  replica_->Flush();
  EXPECT_EQ(replica_->Op(), 0U);
  EXPECT_TRUE(state_machine_->values_.empty());
  // The session is gone too.
  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes("a=9")), SubmitResult::NoSession);
  Register(1);
  EXPECT_EQ(Request(1, 1, "a=9"), "");
}

// A client that did not register gets an eviction notice, and its
// operation does not execute.
TEST_F(SingleReplica, UnregisteredClientIsRefused)
{
  EXPECT_EQ(replica_->Submit(9, 1, sessions_[9], Bytes("a=1")), SubmitResult::NoSession);
  replica_->Flush();
  ASSERT_EQ(bus_.evicted_clients_.size(), 1U);
  EXPECT_EQ(bus_.evicted_clients_.back(), 9U);
  EXPECT_EQ(replica_->Op(), 0U);
  EXPECT_TRUE(state_machine_->values_.empty());
}

// The client table holds four sessions. A fifth client removes the session
// that committed least recently. That client then sends its last request
// again, because the reply was lost. The request must not execute a second
// time: the replica no longer knows that it executed, so it refuses it.
TEST_F(SingleReplica, ClientWithoutSessionCannotExecuteAgain)
{
  Register(1);
  EXPECT_EQ(Request(1, 1, "counter=first"), "");
  for (std::uint64_t client = 2; client <= 5; ++client)
  {
    Register(client);
  }
  const std::uint64_t op_before = replica_->Op();

  EXPECT_EQ(replica_->Submit(1, 1, sessions_[1], Bytes("counter=first")), SubmitResult::NoSession);
  EXPECT_EQ(replica_->Submit(1, 2, sessions_[1], Bytes("counter=second")), SubmitResult::NoSession);
  replica_->Flush();
  EXPECT_EQ(bus_.evicted_clients_, (std::vector<std::uint64_t>{1, 1}));
  EXPECT_EQ(replica_->Op(), op_before);
  EXPECT_EQ(state_machine_->values_.at("counter"), "first");

  // The other sessions still work, and the client can register again.
  EXPECT_EQ(Request(5, 1, "counter=fifth"), "first");
  Register(1);
  EXPECT_EQ(Request(1, 1, "counter=again"), "fifth");
}

// An operation prepared before eviction must not execute after the eviction
// commits earlier in the same batch.
TEST_F(SingleReplica, EvictionBeforeCommitInvalidatesPreparedOperation)
{
  Register(1);
  ASSERT_EQ(Request(1, 1, "counter=first"), "");
  Register(2);
  Register(3);
  Register(4);
  ASSERT_EQ(replica_->Submit(5, 0, 0, {}), SubmitResult::Accepted);
  ASSERT_EQ(replica_->Submit(1, 2, sessions_[1], Bytes("counter=duplicate")),
            SubmitResult::Accepted);
  replica_->Flush();
  EXPECT_EQ(state_machine_->values_.at("counter"), "first");
  EXPECT_EQ(replica_->Submit(1, 2, sessions_[1], Bytes("counter=duplicate")),
            SubmitResult::NoSession);
}

// Delayed registration and operation copies cannot revive an evicted session,
// including after the replacement session is checkpointed and restarted.
TEST_F(SingleReplica, DelayedRegistrationCannotReplayEvictedOperation)
{
  Register(1);
  const std::uint64_t old_session = sessions_[1];
  ASSERT_EQ(Request(1, 1, "counter=first"), "");
  for (std::uint64_t client = 2; client <= 5; ++client)
  {
    Register(client);
  }
  Register(1);
  ASSERT_NE(sessions_[1], old_session);
  EXPECT_EQ(replica_->Submit(1, 1, old_session, Bytes("counter=duplicate")),
            SubmitResult::NoSession);
  EXPECT_EQ(state_machine_->values_.at("counter"), "first");
  ASSERT_EQ(Request(1, 1, "counter=second"), "first");
  ASSERT_EQ(Request(1, 2, "counter=third"), "second");
  ASSERT_EQ(Request(1, 3, "counter=fourth"), "third");
  Start();
  replica_->Flush();
  EXPECT_EQ(replica_->Submit(1, 1, old_session, Bytes("counter=duplicate")),
            SubmitResult::NoSession);
  EXPECT_EQ(state_machine_->values_.at("counter"), "fourth");
  EXPECT_EQ(Request(1, 4, "counter=fifth"), "fourth");
}

// The sessions are part of the checkpoint: after a restart, a duplicate
// still gets the stored reply and a removed client is still refused.
TEST_F(SingleReplica, SessionsSurviveRestartFromCheckpoint)
{
  Register(1);
  Register(2);
  for (std::uint64_t request = 1; request <= 12; ++request)
  {
    ASSERT_TRUE(Request(2, request, "k=" + std::to_string(request)).has_value());
  }
  ASSERT_GT(replica_->CheckpointOp(), 0U);

  Start();
  replica_->Flush();
  bus_.replies_.clear();
  EXPECT_EQ(replica_->Submit(2, 12, sessions_[2], Bytes("k=12")), SubmitResult::Duplicate);
  ASSERT_EQ(bus_.replies_.size(), 1U);
  EXPECT_EQ(bus_.replies_.back().body_, "11");
  EXPECT_EQ(replica_->Submit(7, 1, sessions_[7], Bytes("k=x")), SubmitResult::NoSession);
  EXPECT_EQ(state_machine_->values_.at("k"), "12");
}

} // namespace vsr
