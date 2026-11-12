#pragma once

#include <cstdio>
#include <cstdlib>

namespace vsr::internal
{

[[noreturn]] inline void AssertFail(const char *p_expression, const char *p_file,
                                    int p_line) noexcept
{
  std::fprintf(stderr, "vsr: assertion failed: %s (%s:%d)\n", p_expression, p_file, p_line);
  std::abort();
}

} // namespace vsr::internal

// Assertions stay enabled in every build type. A broken invariant in a
// replication protocol must stop the replica before it can corrupt the log.
#define VSR_ASSERT(p_condition)                                                                    \
  ((p_condition) ? static_cast<void>(0)                                                            \
                 : ::vsr::internal::AssertFail(#p_condition, __FILE__, __LINE__))
