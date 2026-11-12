#include "simulation.hpp"

#include <gtest/gtest.h>

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

} // namespace

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
