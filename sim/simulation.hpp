#pragma once

#include "vsr/config.hpp"

#include <cstdint>
#include <string>

// A deterministic simulation of a cluster. It runs the production replica
// code with a simulated network, storage, and clock, injects faults, and
// checks safety during the run and liveness after the faults stop. A seed
// gives the same run each time.

namespace vsr::sim
{

struct Options
{
  std::uint64_t seed_ = 1;
  std::uint8_t replica_count_ = 3;
  std::uint32_t client_count_ = 4;
  // Ticks with faults, then ticks without faults.
  std::uint64_t chaos_ticks_ = 4000;
  std::uint64_t heal_ticks_ = 3000;

  // Probabilities, in parts per 10 000 for each opportunity.
  std::uint32_t drop_rate_ = 500;
  std::uint32_t duplicate_rate_ = 200;
  std::uint32_t crash_rate_ = 20;
  std::uint32_t storage_fault_rate_ = 10;
  std::uint32_t partition_rate_ = 20;
  std::uint32_t delay_ticks_max_ = 8;
  // Wall clocks of replicas differ by up to this value.
  std::uint64_t clock_skew_ns_max_ = 50'000'000;
};

struct Result
{
  bool ok_ = true;
  // The first check that failed.
  std::string failure_;
  // Operations that the state machine executed.
  std::uint64_t committed_ops_ = 0;
  // Clients that lost their session, because more clients were active
  // than the client table holds.
  std::uint64_t evictions_ = 0;
  std::uint64_t requests_completed_ = 0;
  std::uint64_t crashes_ = 0;
  std::uint64_t view_max_ = 0;
  std::uint64_t checkpoints_ = 0;
  std::uint64_t snapshots_installed_ = 0;
};

// The replica limits that the simulation uses. They are small, so that a
// short run reaches the ring limit, checkpoints, and snapshot transfer.
[[nodiscard]] Config SimulationConfig(std::uint8_t p_replica_count, std::uint8_t p_replica);

[[nodiscard]] Result Run(const Options &p_options);

} // namespace vsr::sim
