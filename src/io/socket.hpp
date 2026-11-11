#pragma once

#include "vsr/io/unique_fd.hpp"
#include "vsr/message.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <netinet/in.h>
#include <span>
#include <string>
#include <vector>

namespace vsr::io::internal
{

// Parses "a.b.c.d:port". Host names are not supported.
[[nodiscard]] std::expected<sockaddr_in, std::string> ParseAddress(const std::string &p_text);

// A TCP socket that does not block and does not delay small messages.
[[nodiscard]] UniqueFd MakeSocket();

// Starts a connect. Returns false if it failed at once. The connect can
// still be in progress when this returns true.
[[nodiscard]] bool StartConnect(int p_fd, const sockaddr_in &p_address);

// The result of a connect that was in progress, when the socket is writable.
[[nodiscard]] bool ConnectSucceeded(int p_fd);

[[nodiscard]] std::uint64_t MonotonicNs();
[[nodiscard]] std::uint64_t RealtimeNs();

enum class ReadResult
{
  Data = 0,
  WouldBlock,
  Closed,
};

// Collects the bytes of a stream and gives the complete messages.
class FrameBuffer final
{
public:
  explicit FrameBuffer(std::uint32_t p_body_bytes_max)
      : body_bytes_max_(p_body_bytes_max), bytes_(MessageBytesMax(p_body_bytes_max))
  {
  }

  [[nodiscard]] ReadResult ReadFrom(int p_fd);

  // Calls `p_on_message` for each complete message, in order. Returns false
  // if the stream does not hold valid frames. The caller must then close it.
  template <typename TCallback> [[nodiscard]] bool Drain(TCallback &&p_on_message)
  {
    std::size_t offset = 0;
    bool valid = true;
    while (used_bytes_ - offset >= kHeaderBytes)
    {
      const auto rest = std::span<const std::byte>{bytes_}.subspan(offset, used_bytes_ - offset);
      const auto message_bytes = PeekMessageBytes(rest, body_bytes_max_);
      if (!message_bytes)
      {
        valid = false;
        break;
      }
      if (rest.size() < *message_bytes)
      {
        break;
      }
      p_on_message(rest.first(*message_bytes));
      offset += *message_bytes;
    }
    if (offset > 0)
    {
      std::memmove(bytes_.data(), bytes_.data() + offset, used_bytes_ - offset);
      used_bytes_ -= offset;
    }
    return valid;
  }

  void Clear() noexcept { used_bytes_ = 0; }

private:
  std::uint32_t body_bytes_max_;
  std::vector<std::byte> bytes_;
  std::size_t used_bytes_ = 0;
};

} // namespace vsr::io::internal
