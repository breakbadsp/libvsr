#include "vsr/checksum.hpp"

#include <bit>
#include <cstring>

namespace vsr
{

namespace
{

constexpr std::uint64_t kPrime1 = 11400714785074694791ULL;
constexpr std::uint64_t kPrime2 = 14029467366897019727ULL;
constexpr std::uint64_t kPrime3 = 1609587929392839161ULL;
constexpr std::uint64_t kPrime4 = 9650029242287828579ULL;
constexpr std::uint64_t kPrime5 = 2870177450012600261ULL;
constexpr std::size_t kStripeBytes = 32;

static_assert(std::endian::native == std::endian::little, "the formats are little-endian");

[[nodiscard]] std::uint64_t Read64(const std::byte *p_bytes) noexcept
{
  std::uint64_t value = 0;
  std::memcpy(&value, p_bytes, sizeof(value));
  return value;
}

[[nodiscard]] std::uint32_t Read32(const std::byte *p_bytes) noexcept
{
  std::uint32_t value = 0;
  std::memcpy(&value, p_bytes, sizeof(value));
  return value;
}

[[nodiscard]] constexpr std::uint64_t Round(std::uint64_t p_accumulator,
                                            std::uint64_t p_input) noexcept
{
  return std::rotl(p_accumulator + p_input * kPrime2, 31) * kPrime1;
}

[[nodiscard]] constexpr std::uint64_t MergeRound(std::uint64_t p_hash,
                                                 std::uint64_t p_accumulator) noexcept
{
  return (p_hash ^ Round(0, p_accumulator)) * kPrime1 + kPrime4;
}

} // namespace

std::uint64_t Checksum(std::span<const std::byte> p_bytes) noexcept
{
  const std::byte *cursor = p_bytes.data();
  std::size_t remaining_bytes = p_bytes.size();
  std::uint64_t hash = kPrime5;
  if (remaining_bytes >= kStripeBytes)
  {
    std::uint64_t v1 = kPrime1 + kPrime2;
    std::uint64_t v2 = kPrime2;
    std::uint64_t v3 = 0;
    std::uint64_t v4 = 0 - kPrime1;
    while (remaining_bytes >= kStripeBytes)
    {
      v1 = Round(v1, Read64(cursor));
      v2 = Round(v2, Read64(cursor + 8));
      v3 = Round(v3, Read64(cursor + 16));
      v4 = Round(v4, Read64(cursor + 24));
      cursor += kStripeBytes;
      remaining_bytes -= kStripeBytes;
    }
    hash = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    hash = MergeRound(hash, v1);
    hash = MergeRound(hash, v2);
    hash = MergeRound(hash, v3);
    hash = MergeRound(hash, v4);
  }
  hash += p_bytes.size();
  while (remaining_bytes >= 8)
  {
    hash ^= Round(0, Read64(cursor));
    hash = std::rotl(hash, 27) * kPrime1 + kPrime4;
    cursor += 8;
    remaining_bytes -= 8;
  }
  if (remaining_bytes >= 4)
  {
    hash ^= Read32(cursor) * kPrime1;
    hash = std::rotl(hash, 23) * kPrime2 + kPrime3;
    cursor += 4;
    remaining_bytes -= 4;
  }
  while (remaining_bytes > 0)
  {
    hash ^= std::to_integer<std::uint64_t>(*cursor) * kPrime5;
    hash = std::rotl(hash, 11) * kPrime1;
    cursor += 1;
    remaining_bytes -= 1;
  }
  hash ^= hash >> 33;
  hash *= kPrime2;
  hash ^= hash >> 29;
  hash *= kPrime3;
  hash ^= hash >> 32;
  return hash;
}

} // namespace vsr
