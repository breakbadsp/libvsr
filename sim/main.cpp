#include "simulation.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{

[[nodiscard]] std::uint64_t ParseNumber(std::string_view p_text, std::uint64_t p_fallback)
{
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(p_text.data(), p_text.data() + p_text.size(), value);
  return parsed.ec == std::errc{} ? value : p_fallback;
}

} // namespace

// Usage: vsr_sim [first_seed] [seed_count] [replica_count] [chaos_ticks] [client_count]
int main(int p_argc, char **p_argv)
{
  const std::uint64_t first_seed = p_argc > 1 ? ParseNumber(p_argv[1], 1) : 1;
  const std::uint64_t seed_count = p_argc > 2 ? ParseNumber(p_argv[2], 1) : 1;
  vsr::sim::Options options;
  if (p_argc > 3)
  {
    options.replica_count_ = static_cast<std::uint8_t>(ParseNumber(p_argv[3], 3));
  }
  if (p_argc > 4)
  {
    options.chaos_ticks_ = ParseNumber(p_argv[4], options.chaos_ticks_);
  }
  if (p_argc > 5)
  {
    options.client_count_ = static_cast<std::uint32_t>(ParseNumber(p_argv[5], 4));
  }
  std::uint64_t committed_ops = 0;
  std::uint64_t evictions = 0;
  std::uint64_t crashes = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t view_max = 0;
  for (std::uint64_t seed = first_seed; seed < first_seed + seed_count; ++seed)
  {
    options.seed_ = seed;
    const auto result = vsr::sim::Run(options);
    if (!result.ok_)
    {
      std::printf("FAIL seed=%llu replicas=%u: %s\n", static_cast<unsigned long long>(seed),
                  static_cast<unsigned>(options.replica_count_), result.failure_.c_str());
      return EXIT_FAILURE;
    }
    committed_ops += result.committed_ops_;
    crashes += result.crashes_;
    evictions += result.evictions_;
    snapshots += result.snapshots_installed_;
    view_max = result.view_max_ > view_max ? result.view_max_ : view_max;
  }
  std::printf(
      "PASS seeds=%llu..%llu replicas=%u committed_ops=%llu crashes=%llu "
      "snapshots_installed=%llu view_max=%llu evictions=%llu\n",
      static_cast<unsigned long long>(first_seed),
      static_cast<unsigned long long>(first_seed + seed_count - 1),
      static_cast<unsigned>(options.replica_count_), static_cast<unsigned long long>(committed_ops),
      static_cast<unsigned long long>(crashes), static_cast<unsigned long long>(snapshots),
      static_cast<unsigned long long>(view_max), static_cast<unsigned long long>(evictions));
  return EXIT_SUCCESS;
}
