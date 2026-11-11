#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vsr
{

// XXH64. It detects torn writes and identifies log entries in the hash chain.
// It is not a defence against a malicious peer.
[[nodiscard]] std::uint64_t Checksum(std::span<const std::byte> p_bytes) noexcept;

} // namespace vsr
