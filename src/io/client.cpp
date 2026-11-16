#include "vsr/io/client.hpp"

#include "socket.hpp"
#include "vsr/client_core.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <utility>

// The protocol of the client is in `vsr::ClientCore`. This file gives the
// core its sockets and its clock.

namespace vsr::io
{

namespace
{

constexpr std::uint64_t kNsPerMs = 1'000'000;
constexpr int kConnectWaitMs = 50;

[[nodiscard]] std::uint64_t RandomSeed()
{
  std::random_device device;
  return (std::uint64_t{device()} << 32) | device();
}

// The time at which a wait that starts at `p_now_ns` ends. A timeout that
// the clock cannot hold gives the largest time, which never comes.
[[nodiscard]] std::uint64_t DeadlineNs(std::uint64_t p_now_ns,
                                       std::chrono::milliseconds p_timeout) noexcept
{
  const auto timeout_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(p_timeout.count(), 0));
  if (timeout_ms > (kClientNever - p_now_ns) / kNsPerMs)
  {
    return kClientNever;
  }
  return p_now_ns + timeout_ms * kNsPerMs;
}

// The time until `p_wake_ns` as a `poll` timeout, rounded up.
[[nodiscard]] int WaitMs(std::uint64_t p_now_ns, std::uint64_t p_wake_ns) noexcept
{
  const std::uint64_t wait_ns = p_wake_ns > p_now_ns ? p_wake_ns - p_now_ns : 0;
  const std::uint64_t wait_ms = wait_ns / kNsPerMs + (wait_ns % kNsPerMs == 0 ? 0 : 1);
  return static_cast<int>(
      std::min<std::uint64_t>(wait_ms, std::uint64_t{std::numeric_limits<int>::max()}));
}

[[nodiscard]] ClientError ToClientError(ClientCoreError p_error) noexcept
{
  switch (p_error)
  {
  case ClientCoreError::TooLarge:
    return ClientError::TooLarge;
  case ClientCoreError::Pending:
    return ClientError::Pending;
  case ClientCoreError::NotPending:
    return ClientError::NotPending;
  }
  return ClientError::NotPending;
}

} // namespace

class Client::Impl final
{
public:
  Impl(const ClientCoreOptions &p_options, std::vector<sockaddr_in> p_addresses)
      : core_(p_options), addresses_(std::move(p_addresses)), sockets_(addresses_.size()),
        waiters_(addresses_.size()), send_wait_ns_(p_options.retry_interval_)
  {
    for (std::size_t index = 0; index < addresses_.size(); ++index)
    {
      buffers_.emplace_back(p_options.body_bytes_max_);
    }
  }

  [[nodiscard]] std::uint64_t Id() const noexcept { return core_.Id(); }

  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError>
  Request(std::span<const std::byte> p_body, std::chrono::milliseconds p_timeout)
  {
    const auto started = core_.Request(p_body, DeadlineNs(internal::MonotonicNs(), p_timeout));
    if (!started)
    {
      return std::unexpected(ToClientError(started.error()));
    }
    return Wait();
  }

  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError>
  Resume(std::chrono::milliseconds p_timeout)
  {
    const auto started = core_.Resume(DeadlineNs(internal::MonotonicNs(), p_timeout));
    if (!started)
    {
      return std::unexpected(ToClientError(started.error()));
    }
    return Wait();
  }

private:
  [[nodiscard]] bool EnsureConnected(std::size_t p_replica)
  {
    auto &socket = sockets_[p_replica];
    if (socket.IsOpen())
    {
      return true;
    }
    buffers_[p_replica].Clear();
    socket = internal::MakeSocket();
    if (!socket.IsOpen() || !internal::StartConnect(socket.Get(), addresses_[p_replica]))
    {
      socket.Reset();
      return false;
    }
    pollfd waiter = {socket.Get(), POLLOUT, 0};
    const bool writable = ::poll(&waiter, 1, kConnectWaitMs) == 1;
    if (!writable || !internal::ConnectSucceeded(socket.Get()))
    {
      socket.Reset();
      return false;
    }
    return true;
  }

  void Send(std::size_t p_replica, std::span<const std::byte> p_message)
  {
    if (!EnsureConnected(p_replica))
    {
      return;
    }
    // The socket does not block, so a large request can need several
    // sends. Wait for the socket at most one retry interval.
    const std::uint64_t deadline_ns = internal::MonotonicNs() + send_wait_ns_;
    std::size_t sent_bytes = 0;
    while (sent_bytes < p_message.size())
    {
      const ssize_t count = ::send(sockets_[p_replica].Get(), p_message.data() + sent_bytes,
                                   p_message.size() - sent_bytes, MSG_NOSIGNAL);
      if (count > 0)
      {
        sent_bytes += static_cast<std::size_t>(count);
        continue;
      }
      const bool would_block = count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
      pollfd waiter = {sockets_[p_replica].Get(), POLLOUT, 0};
      const bool can_wait = would_block && internal::MonotonicNs() < deadline_ns &&
                            ::poll(&waiter, 1, kConnectWaitMs) >= 0;
      if (!can_wait && !(count < 0 && errno == EINTR))
      {
        // A part of a message is in the stream. The stream is not usable.
        sockets_[p_replica].Reset();
        return;
      }
    }
  }

  // Reads one socket and gives its messages to the core. Puts the body in
  // `reply_` if the reply to the request in progress arrived.
  [[nodiscard]] ClientEventKind Receive(std::size_t p_replica)
  {
    ClientEventKind kind = ClientEventKind::None;
    const auto result = buffers_[p_replica].ReadFrom(sockets_[p_replica].Get());
    const bool valid = buffers_[p_replica].Drain(
        [this, &kind](std::span<const std::byte> p_bytes)
        {
          if (kind != ClientEventKind::None)
          {
            return;
          }
          const ClientEvent event = core_.OnMessage(p_bytes);
          kind = event.kind_;
          if (kind == ClientEventKind::Reply)
          {
            // The reply refers to the buffer, which the next read changes.
            reply_.assign(event.reply_.begin(), event.reply_.end());
          }
        });
    if (result == internal::ReadResult::Closed || !valid)
    {
      sockets_[p_replica].Reset();
    }
    return kind;
  }

  // Waits for the open sockets until `p_wake_ns`, then reads those that
  // have data.
  [[nodiscard]] ClientEventKind ReceiveUntil(std::uint64_t p_wake_ns)
  {
    for (std::size_t replica = 0; replica < sockets_.size(); ++replica)
    {
      // `poll` ignores a negative descriptor.
      waiters_[replica] =
          pollfd{sockets_[replica].IsOpen() ? sockets_[replica].Get() : -1, POLLIN, 0};
    }
    const int ready_count =
        ::poll(waiters_.data(), waiters_.size(), WaitMs(internal::MonotonicNs(), p_wake_ns));
    for (std::size_t replica = 0; ready_count > 0 && replica < waiters_.size(); ++replica)
    {
      if (waiters_[replica].revents == 0)
      {
        continue;
      }
      const ClientEventKind kind = Receive(replica);
      if (kind != ClientEventKind::None)
      {
        return kind;
      }
    }
    return ClientEventKind::None;
  }

  // Runs the core until the request in progress has its result, or the
  // deadline of the wait passes.
  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError> Wait()
  {
    while (true)
    {
      const ClientSend send = core_.Poll(internal::MonotonicNs());
      for (std::size_t replica = send.replica_begin_; replica < send.replica_end_; ++replica)
      {
        // The primary replies on the connection that carried the request.
        Send(replica, send.message_);
      }
      const ClientEventKind kind = ReceiveUntil(core_.WakeTime());
      if (kind == ClientEventKind::Reply)
      {
        return std::move(reply_);
      }
      if (kind == ClientEventKind::Evicted)
      {
        return std::unexpected(ClientError::Evicted);
      }
      if (core_.Expire(internal::MonotonicNs()))
      {
        return std::unexpected(ClientError::Timeout);
      }
    }
  }

  ClientCore core_;
  std::vector<sockaddr_in> addresses_;
  std::vector<UniqueFd> sockets_;
  std::vector<internal::FrameBuffer> buffers_;
  // One entry for each replica, for `poll`.
  std::vector<pollfd> waiters_;
  std::uint64_t send_wait_ns_;
  std::vector<std::byte> reply_;
};

Client::Client(std::unique_ptr<Impl> p_impl) noexcept : impl_(std::move(p_impl)) {}

Client::Client(Client &&) noexcept = default;
Client &Client::operator=(Client &&) noexcept = default;
Client::~Client() = default;

std::expected<Client, std::string> Client::Create(ClientOptions p_options)
{
  if (p_options.addresses_.empty() || p_options.retry_ms_ == 0)
  {
    return std::unexpected(std::string{"at least one replica address is necessary"});
  }
  if (p_options.addresses_.size() > std::numeric_limits<std::uint8_t>::max())
  {
    return std::unexpected(std::string{"too many replica addresses"});
  }
  std::vector<sockaddr_in> addresses;
  for (const auto &text : p_options.addresses_)
  {
    const auto address = internal::ParseAddress(text);
    if (!address)
    {
      return std::unexpected(address.error());
    }
    addresses.push_back(*address);
  }
  ClientCoreOptions core_options;
  core_options.cluster_ = p_options.cluster_;
  core_options.client_ = p_options.client_;
  core_options.seed_ = RandomSeed();
  core_options.replica_count_ = static_cast<std::uint8_t>(addresses.size());
  core_options.body_bytes_max_ = p_options.body_bytes_max_;
  core_options.retry_interval_ = p_options.retry_ms_ * kNsPerMs;
  return Client{std::make_unique<Impl>(core_options, std::move(addresses))};
}

std::expected<std::vector<std::byte>, ClientError>
Client::Request(std::span<const std::byte> p_body, std::chrono::milliseconds p_timeout)
{
  return impl_->Request(p_body, p_timeout);
}

std::expected<std::vector<std::byte>, ClientError>
Client::Resume(std::chrono::milliseconds p_timeout)
{
  return impl_->Resume(p_timeout);
}

std::uint64_t Client::Id() const noexcept { return impl_->Id(); }

} // namespace vsr::io
