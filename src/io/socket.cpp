#include "socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <ctime>
#include <format>
#include <limits>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace vsr::io::internal
{

namespace
{

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;

[[nodiscard]] std::uint64_t ClockNs(clockid_t p_clock)
{
  timespec time = {};
  ::clock_gettime(p_clock, &time);
  return static_cast<std::uint64_t>(time.tv_sec) * kNsPerSecond +
         static_cast<std::uint64_t>(time.tv_nsec);
}

} // namespace

std::expected<sockaddr_in, std::string> ParseAddress(const std::string &p_text)
{
  const auto colon = p_text.rfind(':');
  if (colon == std::string::npos)
  {
    return std::unexpected(std::format("address '{}' has no port", p_text));
  }
  const std::string host = p_text.substr(0, colon);
  std::uint32_t port = 0;
  const char *const port_begin = p_text.data() + colon + 1;
  const char *const port_end = p_text.data() + p_text.size();
  const auto parsed = std::from_chars(port_begin, port_end, port);
  const bool port_valid = parsed.ec == std::errc{} && parsed.ptr == port_end && port != 0 &&
                          port <= std::numeric_limits<std::uint16_t>::max();
  if (!port_valid)
  {
    return std::unexpected(std::format("address '{}' has an invalid port", p_text));
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
  {
    return std::unexpected(std::format("address '{}' is not an IPv4 address", p_text));
  }
  return address;
}

UniqueFd MakeSocket()
{
  UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  if (fd.IsOpen())
  {
    // A protocol message must not wait for more data to fill a segment.
    const int enabled = 1;
    ::setsockopt(fd.Get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  }
  return fd;
}

bool StartConnect(int p_fd, const sockaddr_in &p_address)
{
  // The sockets API takes the generic address type. This cast is its
  // documented use.
  const auto *const address = reinterpret_cast<const sockaddr *>(&p_address);
  return ::connect(p_fd, address, sizeof(p_address)) == 0 || errno == EINPROGRESS;
}

bool ConnectSucceeded(int p_fd)
{
  int error = 0;
  socklen_t error_bytes = sizeof(error);
  return ::getsockopt(p_fd, SOL_SOCKET, SO_ERROR, &error, &error_bytes) == 0 && error == 0;
}

std::uint64_t MonotonicNs() { return ClockNs(CLOCK_MONOTONIC); }

std::uint64_t RealtimeNs() { return ClockNs(CLOCK_REALTIME); }

ReadResult FrameBuffer::ReadFrom(int p_fd)
{
  if (used_bytes_ == bytes_.size())
  {
    // Drain takes each complete message, and no message is larger than the
    // buffer. A full buffer thus means that the caller did not drain.
    return ReadResult::Closed;
  }
  const ssize_t count = ::recv(p_fd, bytes_.data() + used_bytes_, bytes_.size() - used_bytes_, 0);
  if (count > 0)
  {
    used_bytes_ += static_cast<std::size_t>(count);
    return ReadResult::Data;
  }
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
  {
    return ReadResult::WouldBlock;
  }
  return ReadResult::Closed;
}

} // namespace vsr::io::internal
