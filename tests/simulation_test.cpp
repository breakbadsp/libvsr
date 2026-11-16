#include "vsr/checksum.hpp"
#include "vsr/sim/simulation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Each test runs the production replica code in the simulator for a range of
// seeds. A run fails if two replicas disagree, if a client sees a wrong or
// repeated result, or if the cluster does not recover after the faults stop.
// To replay one failure: `vsr_sim <seed> 1 <replica_count>`.

namespace vsr::sim
{

namespace
{

struct Totals
{
  std::uint64_t committed_ops_ = 0;
  std::uint64_t crashes_ = 0;
  std::uint64_t snapshots_installed_ = 0;
  std::uint64_t checkpoints_ = 0;
  std::uint64_t view_max_ = 0;
  std::uint64_t evictions_ = 0;
};

[[nodiscard]] Totals RunSeeds(Options p_options, std::uint64_t p_seed_count)
{
  Totals totals;
  for (std::uint64_t seed = 1; seed <= p_seed_count; ++seed)
  {
    p_options.seed_ = seed;
    const Result result = Run(p_options);
    EXPECT_TRUE(result.ok_) << "seed " << seed << ": " << result.failure_;
    if (!result.ok_)
    {
      break;
    }
    totals.committed_ops_ += result.committed_ops_;
    totals.crashes_ += result.crashes_;
    totals.snapshots_installed_ += result.snapshots_installed_;
    totals.checkpoints_ += result.checkpoints_;
    totals.evictions_ += result.evictions_;
    totals.view_max_ = std::max(totals.view_max_, result.view_max_);
  }
  return totals;
}

// What a `SumApplication` does wrong. Each defect breaks one rule of the
// host contract.
struct Defects
{
  // Added to the state at each operation. Not zero: replicas disagree.
  std::uint64_t state_salt_ = 0;
  // Added to each reply. Not zero: replicas give different replies.
  std::uint64_t reply_salt_ = 0;
  // The snapshot does not hold the operation count.
  bool snapshot_loses_count_ = false;
};

// A second state machine, to show that the simulator runs and checks the
// application of a host. The state is the sum of all operands and the
// number of operations. A valid body is one 64-bit operand.
class SumApplication final : public Application
{
public:
  SumApplication(Defects p_defects, std::size_t p_snapshot_bytes)
      : defects_(p_defects), snapshot_bytes_(p_snapshot_bytes)
  {
  }

  [[nodiscard]] std::size_t Execute(const Operation &, std::span<const std::byte> p_body,
                                    std::span<std::byte> p_reply) override
  {
    std::uint64_t operand = 0;
    if (p_body.size() != sizeof(operand))
    {
      return 0;
    }
    std::memcpy(&operand, p_body.data(), sizeof(operand));
    state_[0] += operand + defects_.state_salt_;
    state_[1] += 1;
    const std::uint64_t reply = state_[0] + defects_.reply_salt_;
    std::memcpy(p_reply.data(), &reply, sizeof(reply));
    return sizeof(reply);
  }

  [[nodiscard]] std::expected<std::size_t, std::monostate>
  Snapshot(std::span<std::byte> p_out) override
  {
    if (p_out.size() < snapshot_bytes_)
    {
      return std::unexpected(std::monostate{});
    }
    std::ranges::fill(p_out.first(snapshot_bytes_), std::byte{0});
    const std::array<std::uint64_t, 2> stored{state_[0],
                                              defects_.snapshot_loses_count_ ? 0 : state_[1]};
    std::memcpy(p_out.data(), stored.data(), sizeof(stored));
    return snapshot_bytes_;
  }

  [[nodiscard]] bool Restore(std::span<const std::byte> p_snapshot) override
  {
    if (p_snapshot.size() != snapshot_bytes_)
    {
      return false;
    }
    std::memcpy(state_.data(), p_snapshot.data(), sizeof(state_));
    return true;
  }

  [[nodiscard]] std::uint64_t Digest() const override
  {
    return Checksum(std::as_bytes(std::span{state_}));
  }

private:
  Defects defects_;
  std::size_t snapshot_bytes_;
  // The sum, then the operation count.
  std::array<std::uint64_t, 2> state_{};
};

constexpr std::size_t kSumSnapshotBytes = 2 * sizeof(std::uint64_t);

// Options that run `SumApplication` with valid bodies.
[[nodiscard]] Options SumOptions(std::size_t p_snapshot_bytes = kSumSnapshotBytes)
{
  Options options;
  options.application_factory_ = [p_snapshot_bytes]
  { return std::make_unique<SumApplication>(Defects{}, p_snapshot_bytes); };
  options.body_generator_ = [](std::uint64_t p_random)
  {
    std::vector<std::byte> body(sizeof(p_random));
    std::memcpy(body.data(), &p_random, sizeof(p_random));
    return body;
  };
  return options;
}

// Options that give each `SumApplication` object a different salt. A salt
// is a multiple of `p_step`, so `Defects{}` as the step gives no defect.
[[nodiscard]] Options DefectOptions(Defects p_step)
{
  Options options = SumOptions();
  options.application_factory_ = [p_step, made = std::make_shared<std::uint64_t>(0)]
  {
    *made += 1;
    Defects defects = p_step;
    defects.state_salt_ *= *made;
    defects.reply_salt_ *= *made;
    return std::make_unique<SumApplication>(defects, kSumSnapshotBytes);
  };
  return options;
}

} // namespace

// The simulator runs the application of a host through the same faults and
// checks as its own state machine.
TEST(Simulation, HostApplicationWithFaults)
{
  const Totals totals = RunSeeds(SumOptions(), 40);
  EXPECT_GT(totals.committed_ops_, 0U);
  EXPECT_GT(totals.crashes_, 0U);
  EXPECT_GT(totals.checkpoints_, 0U);
  EXPECT_GT(totals.snapshots_installed_, 0U);
}

// The same options pass without a defect, so each failure below comes from
// the defect and not from the options.
TEST(Simulation, HostApplicationWithoutDefectPasses)
{
  const Result result = sim::Run(DefectOptions(Defects{}));
  EXPECT_TRUE(result.ok_) << result.failure_;
}

// Replicas that reach different states from the same operations fail the
// run, also when their replies are equal.
TEST(Simulation, HostApplicationWithDifferentStateFails)
{
  const Result result = sim::Run(DefectOptions(Defects{.state_salt_ = 1, .reply_salt_ = ~0ULL}));
  EXPECT_FALSE(result.ok_);
  EXPECT_NE(result.failure_.find("different state"), std::string::npos) << result.failure_;
}

// Replicas with equal states that give different replies fail the run.
TEST(Simulation, HostApplicationWithDifferentReplyFails)
{
  const Result result = sim::Run(DefectOptions(Defects{.reply_salt_ = 1}));
  EXPECT_FALSE(result.ok_);
  EXPECT_NE(result.failure_.find("different reply"), std::string::npos) << result.failure_;
}

// A snapshot that does not hold the full state fails the run.
TEST(Simulation, HostApplicationWithIncompleteSnapshotFails)
{
  const Result result = sim::Run(DefectOptions(Defects{.snapshot_loses_count_ = true}));
  EXPECT_FALSE(result.ok_);
  EXPECT_NE(result.failure_.find("snapshot"), std::string::npos) << result.failure_;
}

// The limits come from the options: a snapshot and a body above the default
// limits need larger limits.
TEST(Simulation, HostApplicationLimits)
{
  constexpr std::size_t kLargeSnapshotBytes = 5000;
  constexpr std::size_t kLargeBodyBytes = 400;
  Options options = SumOptions(kLargeSnapshotBytes);
  EXPECT_FALSE(sim::Run(options).ok_);
  options.limits_.snapshot_bytes_max_ = kLargeSnapshotBytes;
  const Result result = sim::Run(options);
  EXPECT_TRUE(result.ok_) << result.failure_;
  EXPECT_GT(result.checkpoints_, 0U);

  options.body_generator_ = [](std::uint64_t) { return std::vector<std::byte>(kLargeBodyBytes); };
  EXPECT_FALSE(sim::Run(options).ok_);
  options.limits_.body_bytes_max_ = kLargeBodyBytes;
  EXPECT_TRUE(sim::Run(options).ok_);
}

// A host with its own client protocol calls `Replica::Submit` for each
// request. The cluster serves the clients through this path only, with the
// same checks, and also with no `Submit` call.
TEST(Simulation, SubmitPath)
{
  Options options;
  options.submit_rate_ = 10'000;
  for (std::uint64_t seed = 1; seed <= 40; ++seed)
  {
    options.seed_ = seed;
    const Result result = sim::Run(options);
    ASSERT_TRUE(result.ok_) << "seed " << seed << ": " << result.failure_;
    // A request can be in the log more than once after a view change.
    EXPECT_GE(result.submits_accepted_, result.requests_completed_);
    EXPECT_GT(result.requests_completed_, 0U);
  }
  options.submit_rate_ = 0;
  options.seed_ = 1;
  const Result messages_only = sim::Run(options);
  EXPECT_TRUE(messages_only.ok_) << messages_only.failure_;
  EXPECT_EQ(messages_only.submits_accepted_, 0U);
  EXPECT_GT(messages_only.requests_completed_, 0U);
}

// The trace is a function of the seed: two runs give the same lines. It
// shows the events of the run, and it does not change the run.
TEST(Simulation, TraceIsDeterministic)
{
  Options options;
  options.seed_ = 7;
  const Result without_trace = sim::Run(options);

  std::array<std::vector<std::string>, 2> traces;
  std::array<Result, 2> results;
  for (std::size_t run = 0; run < traces.size(); ++run)
  {
    options.trace_ = [&lines = traces[run]](std::string_view p_line)
    { lines.emplace_back(p_line); };
    results[run] = sim::Run(options);
  }
  EXPECT_EQ(traces[0], traces[1]);
  EXPECT_EQ(results[0].committed_ops_, without_trace.committed_ops_);
  EXPECT_EQ(results[0].crashes_, without_trace.crashes_);

  const auto count = [&traces](std::string_view p_event)
  {
    return static_cast<std::uint64_t>(
        std::ranges::count_if(traces[0], [p_event](const std::string &p_line)
                              { return p_line.find(p_event) != std::string::npos; }));
  };
  ASSERT_GT(results[0].crashes_, 0U);
  EXPECT_EQ(count(" crash "), results[0].crashes_);
  // Each replica starts one time, and one more time after each crash.
  EXPECT_EQ(count(" start "), results[0].crashes_ + options.replica_count_);
  EXPECT_EQ(count(" is complete "), results[0].requests_completed_);
  EXPECT_GT(count(" deliver "), 0U);
  EXPECT_GT(count(" drop "), 0U);
  EXPECT_GT(count(" state "), 0U);
}

// Without faults, each request commits, and no view change is necessary
// after the first primary is elected.
TEST(Simulation, NoFaultsThreeReplicas)
{
  Options options;
  options.drop_rate_ = 0;
  options.duplicate_rate_ = 0;
  options.crash_rate_ = 0;
  options.storage_fault_rate_ = 0;
  options.partition_rate_ = 0;
  options.delay_ticks_max_ = 1;
  options.chaos_ticks_ = 1000;
  options.heal_ticks_ = 600;
  const Totals totals = RunSeeds(options, 5);
  EXPECT_GT(totals.committed_ops_, 5U * 100U);
  EXPECT_GT(totals.checkpoints_, 0U);
  EXPECT_LE(totals.view_max_, 2U);
}

// The usual cluster, with message loss, crashes, power loss during storage
// writes, and partitions. The run must also reach the paths that the faults
// are there for: view changes, checkpoints, and snapshot transfer.
TEST(Simulation, FaultsThreeReplicas)
{
  const Totals totals = RunSeeds(Options{}, 150);
  // With fewer clients than sessions, no healthy client loses its session.
  EXPECT_EQ(totals.evictions_, 0U);
  EXPECT_GT(totals.committed_ops_, 0U);
  EXPECT_GT(totals.crashes_, 0U);
  EXPECT_GT(totals.view_max_, 2U);
  EXPECT_GT(totals.checkpoints_, 0U);
  EXPECT_GT(totals.snapshots_installed_, 0U);
}

TEST(Simulation, FaultsFiveReplicas)
{
  Options options;
  options.replica_count_ = 5;
  const Totals totals = RunSeeds(options, 60);
  EXPECT_GT(totals.crashes_, 0U);
  EXPECT_GT(totals.snapshots_installed_, 0U);
}

// More clients than the client table holds. The cluster removes sessions,
// and a client whose session is gone sends its request again. No request
// can execute twice.
TEST(Simulation, MoreClientsThanSessions)
{
  Options options;
  options.client_count_ = 12;
  const Totals totals = RunSeeds(options, 60);
  EXPECT_GT(totals.evictions_, 0U);
  EXPECT_GT(totals.committed_ops_, 0U);
}

// Two replicas tolerate no failure, but they must stay safe and recover.
TEST(Simulation, FaultsTwoReplicas)
{
  Options options;
  options.replica_count_ = 2;
  const Totals totals = RunSeeds(options, 40);
  EXPECT_GT(totals.crashes_, 0U);
}

// One replica has no peer: each power loss tests recovery from storage only.
TEST(Simulation, FaultsOneReplica)
{
  Options options;
  options.replica_count_ = 1;
  options.crash_rate_ = 60;
  const Totals totals = RunSeeds(options, 40);
  EXPECT_GT(totals.crashes_, 0U);
  EXPECT_GT(totals.committed_ops_, 0U);
}

} // namespace vsr::sim
