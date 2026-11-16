#pragma once

#include "vsr/config.hpp"
#include "vsr/interfaces.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// A deterministic simulation of a cluster. It runs the production replica
// code with a simulated network, storage, and clock, injects faults, and
// checks safety during the run and liveness after the faults stop. A seed
// gives the same run each time.
//
// A host application can run its own state machine in the simulator:
//
//   class MyApplication final : public vsr::sim::Application { ... };
//
//   vsr::sim::Options options;
//   options.application_factory_ = [] { return std::make_unique<MyApplication>(); };
//   options.body_generator_ = [](std::uint64_t p_random) { return MakeCommand(p_random); };
//   options.limits_.body_bytes_max_ = 4096;
//   options.limits_.snapshot_bytes_max_ = 1 << 20;
//   const vsr::sim::Result result = vsr::sim::Run(options);
//
// CMake: `add_subdirectory(libvsr EXCLUDE_FROM_ALL)`, then link the target
// `vsr_sim_lib` and include "vsr/sim/simulation.hpp".
namespace vsr::sim
{

// The application under test. It is a `vsr::StateMachine`, with one more
// function for the checks of the simulator. The simulator makes one object
// for each start of each replica, and one more for each snapshot check.
class Application : public StateMachine
{
public:
  virtual ~Application() = default;

  // A checksum of the full state. Two objects must give the same value if,
  // and only if, they hold the same state. The simulator compares it across
  // replicas after each operation, and after each snapshot restore.
  [[nodiscard]] virtual std::uint64_t Digest() const = 0;
};

// Makes an application in its initial state. Each call must give an equal
// state, and the objects must not share data.
using ApplicationFactory = std::function<std::unique_ptr<Application>()>;

// Makes the body of one client request. `p_random` is a random number from
// the seed of the run: use it as the only source of variation. The body
// must not be larger than `Limits::body_bytes_max_`.
using BodyGenerator = std::function<std::vector<std::byte>(std::uint64_t p_random)>;

// The replica limits that the simulation uses. The defaults are small, so
// that a short run reaches the ring limit, checkpoints, and snapshot
// transfer. Each field has the meaning of the `vsr::Config` field with the
// same name.
struct Limits
{
  // Largest request body and reply body of the application.
  std::uint32_t body_bytes_max_ = 128;
  std::uint32_t pipeline_max_ = 4;
  std::uint32_t clients_max_ = 8;
  std::uint32_t log_slot_count_ = 32;
  std::uint32_t checkpoint_interval_ = 8;
  // Largest snapshot of the application. The simulator adds its own
  // header to this value.
  std::uint64_t snapshot_bytes_max_ = 1024;
};

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
  // The share of the client sends that reach the replica through
  // `Replica::Submit`, the path of a host with its own client protocol. The
  // other sends are `Request` messages for `Replica::OnMessage`.
  std::uint32_t submit_rate_ = 3000;
  // Wall clocks of replicas differ by up to this value.
  std::uint64_t clock_skew_ns_max_ = 50'000'000;

  Limits limits_;
  // Empty: the simulator uses its own state machine, whose state is a hash
  // of every operation.
  ApplicationFactory application_factory_;
  // Empty: a body has a random size and random bytes.
  BodyGenerator body_generator_;
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
  // Requests that `Replica::Submit` put in the log.
  std::uint64_t submits_accepted_ = 0;
  std::uint64_t crashes_ = 0;
  std::uint64_t view_max_ = 0;
  std::uint64_t checkpoints_ = 0;
  std::uint64_t snapshots_installed_ = 0;
};

// The configuration of one replica in a run with these options.
[[nodiscard]] Config SimulationConfig(const Options &p_options, std::uint8_t p_replica);

[[nodiscard]] Result Run(const Options &p_options);

} // namespace vsr::sim
