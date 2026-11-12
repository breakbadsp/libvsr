#include "vsr/checksum.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace vsr
{

namespace
{

[[nodiscard]] std::vector<std::byte> Counting(std::size_t p_count)
{
  std::vector<std::byte> bytes(p_count);
  for (std::size_t index = 0; index < p_count; ++index)
  {
    bytes[index] = static_cast<std::byte>(index);
  }
  return bytes;
}

} // namespace

// The checksum is XXH64 with seed 0. The expected values come from the
// `xxh64sum` tool, for inputs that use each part of the algorithm: empty,
// less than 4 bytes, 4-byte and 8-byte steps, and the 32-byte stripes.
TEST(Checksum, MatchesReferenceValues)
{
  EXPECT_EQ(Checksum({}), 0xEF46DB3751D8E999ULL);
  const std::array abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  EXPECT_EQ(Checksum(abc), 0x44BC2CF5AD770999ULL);
  EXPECT_EQ(Checksum(Counting(39)), 0x00A396EF1679A859ULL);
  EXPECT_EQ(Checksum(Counting(100)), 0x6AC1E58032166597ULL);
}

} // namespace vsr
