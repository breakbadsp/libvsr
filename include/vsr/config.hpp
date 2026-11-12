#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

namespace vsr
{

inline constexpr std::size_t kReplicasMax = 7;

// Every limit of a replica. All replicas of one cluster use the same values,
// except `replica_`. The storage file records the values that fix its layout.
struct Config
{
  std::uint64_t cluster_ = 0;
  std::uint8_t replica_count_ = 0;
  std::uint8_t replica_ = 0;

  // Largest request body, reply body, and snapshot chunk.
  std::uint32_t body_bytes_max_ = 64 * 1024;
  // Largest number of prepared operations that are not yet committed.
  std::uint32_t pipeline_max_ = 32;
  // Largest number of clients with a stored reply. See docs/host-contract.md.
  std::uint32_t clients_max_ = 64;
  // Number of operations in the log ring.
  std::uint32_t log_slot_count_ = 1024;
  // A checkpoint is taken at each operation number that is a multiple of this.
  std::uint32_t checkpoint_interval_ = 256;
  // Largest state machine snapshot.
  std::uint64_t snapshot_bytes_max_ = 16 * 1024 * 1024;

  // Timers, in ticks. The host sets the length of a tick.
  std::uint32_t heartbeat_ticks_ = 5;
  std::uint32_t retry_ticks_ = 10;
  std::uint32_t primary_timeout_ticks_ = 50;
  std::uint32_t view_change_timeout_ticks_ = 100;
};

enum class ConfigError
{
  ReplicaCount = 0,
  ReplicaIndex,
  BodyBytes,
  Pipeline,
  Clients,
  LogSlots,
  CheckpointInterval,
  Timers,
};

[[nodiscard]] std::expected<void, ConfigError> ValidateConfig(const Config &p_config);

} // namespace vsr
