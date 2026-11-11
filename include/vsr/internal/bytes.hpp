#pragma once

#include "vsr/assert.hpp"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

namespace vsr::internal
{

// Appends integers and bytes to a buffer of fixed size.
class ByteWriter final
{
public:
  explicit ByteWriter(std::span<std::byte> p_out) noexcept : out_(p_out) {}

  template <std::unsigned_integral TNumber> void Put(TNumber p_value) noexcept
  {
    VSR_ASSERT(sizeof(TNumber) <= out_.size() - used_bytes_);
    std::memcpy(out_.data() + used_bytes_, &p_value, sizeof(TNumber));
    used_bytes_ += sizeof(TNumber);
  }

  void PutBytes(std::span<const std::byte> p_bytes) noexcept
  {
    VSR_ASSERT(p_bytes.size() <= out_.size() - used_bytes_);
    if (!p_bytes.empty())
    {
      std::memcpy(out_.data() + used_bytes_, p_bytes.data(), p_bytes.size());
    }
    used_bytes_ += p_bytes.size();
  }

  [[nodiscard]] std::size_t UsedBytes() const noexcept { return used_bytes_; }

private:
  std::span<std::byte> out_;
  std::size_t used_bytes_ = 0;
};

// Reads integers and bytes from input that is not trusted.
class ByteReader final
{
public:
  explicit ByteReader(std::span<const std::byte> p_in) noexcept : in_(p_in) {}

  template <std::unsigned_integral TNumber> [[nodiscard]] std::optional<TNumber> Get() noexcept
  {
    if (in_.size() - used_bytes_ < sizeof(TNumber))
    {
      return std::nullopt;
    }
    TNumber value = 0;
    std::memcpy(&value, in_.data() + used_bytes_, sizeof(TNumber));
    used_bytes_ += sizeof(TNumber);
    return value;
  }

  [[nodiscard]] std::optional<std::span<const std::byte>> GetBytes(std::size_t p_count) noexcept
  {
    if (in_.size() - used_bytes_ < p_count)
    {
      return std::nullopt;
    }
    const auto bytes = in_.subspan(used_bytes_, p_count);
    used_bytes_ += p_count;
    return bytes;
  }

  [[nodiscard]] std::span<const std::byte> Rest() const noexcept
  {
    return in_.subspan(used_bytes_);
  }

private:
  std::span<const std::byte> in_;
  std::size_t used_bytes_ = 0;
};

} // namespace vsr::internal
