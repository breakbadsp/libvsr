#include "vsr/config.hpp"

namespace vsr
{

std::expected<void, ConfigError> ValidateConfig(const Config &p_config)
{
  constexpr std::uint32_t kBodyBytesLimit = 16 * 1024 * 1024;
  if (p_config.replica_count_ == 0 || p_config.replica_count_ > kReplicasMax)
  {
    return std::unexpected(ConfigError::ReplicaCount);
  }
  if (p_config.replica_ >= p_config.replica_count_)
  {
    return std::unexpected(ConfigError::ReplicaIndex);
  }
  if (p_config.body_bytes_max_ == 0 || p_config.body_bytes_max_ > kBodyBytesLimit)
  {
    return std::unexpected(ConfigError::BodyBytes);
  }
  if (p_config.pipeline_max_ == 0)
  {
    return std::unexpected(ConfigError::Pipeline);
  }
  if (p_config.clients_max_ == 0)
  {
    return std::unexpected(ConfigError::Clients);
  }
  if (p_config.checkpoint_interval_ == 0)
  {
    return std::unexpected(ConfigError::CheckpointInterval);
  }
  // The primary can prepare `pipeline_max_` operations past the last commit,
  // and the last commit can be one interval past the last checkpoint. The
  // ring must hold all of them, or the primary stops.
  const std::uint64_t slots_min =
      std::uint64_t{p_config.checkpoint_interval_} + p_config.pipeline_max_;
  if (p_config.log_slot_count_ < slots_min)
  {
    return std::unexpected(ConfigError::LogSlots);
  }
  if (p_config.heartbeat_ticks_ == 0 || p_config.retry_ticks_ == 0 ||
      p_config.primary_timeout_ticks_ <= p_config.heartbeat_ticks_ ||
      p_config.view_change_timeout_ticks_ <= p_config.retry_ticks_)
  {
    return std::unexpected(ConfigError::Timers);
  }
  return {};
}

} // namespace vsr
