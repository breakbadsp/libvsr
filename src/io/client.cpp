#include "vsr/io/client.hpp"

#include "socket.hpp"
#include "vsr/message.hpp"

#include <algorithm>
#include <cerrno>
#include <optional>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <utility>

namespace vsr::io
{

namespace
{

constexpr std::uint64_t kNsPerMs = 1'000'000;
constexpr int kConnectWaitMs = 50;

[[nodiscard]] std::uint64_t RandomClientId()
{
  std::random_device device;
  std::uint64_t id = 0;
  while (id == 0)
  {
    id = (std::uint64_t{device()} << 32) | device();
  }
  return id;
}

} // namespace

class Client::Impl final
{
public:
  Impl(ClientOptions p_options, std::vector<sockaddr_in> p_addresses)
      : options_(std::move(p_options)), addresses_(std::move(p_addresses)),
        sockets_(addresses_.size())
  {
    for (std::size_t index = 0; index < addresses_.size(); ++index)
    {
      buffers_.emplace_back(options_.body_bytes_max_);
    }
  }

  [[nodiscard]] std::uint64_t Id() const noexcept { return options_.client_; }

  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError>
  Request(std::span<const std::byte> p_body, std::chrono::milliseconds p_timeout)
  {
    if (pending_)
    {
      return std::unexpected(ClientError::Pending);
    }
    if (p_body.size() > options_.body_bytes_max_)
    {
      return std::unexpected(ClientError::TooLarge);
    }
    body_.assign(p_body.begin(), p_body.end());
    pending_ = true;
    return Wait(p_timeout);
  }

  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError>
  Resume(std::chrono::milliseconds p_timeout)
  {
    if (!pending_)
    {
      return std::unexpected(ClientError::NotPending);
    }
    return Wait(p_timeout);
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

  void Send(std::size_t p_replica)
  {
    if (!EnsureConnected(p_replica))
    {
      return;
    }
    // The socket does not block, so a large request can need several
    // sends. Wait for the socket at most one retry interval.
    const std::uint64_t deadline_ns = internal::MonotonicNs() + options_.retry_ms_ * kNsPerMs;
    std::size_t sent_bytes = 0;
    while (sent_bytes < message_.size())
    {
      const ssize_t count = ::send(sockets_[p_replica].Get(), message_.data() + sent_bytes,
                                   message_.size() - sent_bytes, MSG_NOSIGNAL);
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

  void Encode(std::uint64_t p_request, std::span<const std::byte> p_body)
  {
    Header header;
    header.command_ = Command::Request;
    header.cluster_ = options_.cluster_;
    header.client_ = options_.client_;
    header.request_ = p_request;
    message_.resize(kHeaderBytes + p_body.size());
    message_.resize(EncodeMessage(header, p_body, {}, message_));
    message_request_ = p_request;
  }

  enum class Received
  {
    Nothing = 0,
    Reply,
    Eviction,
  };

  // Reads one socket. Puts the body in `reply_` if the reply to the message
  // in progress arrived.
  [[nodiscard]] Received Receive(std::size_t p_replica)
  {
    Received received = Received::Nothing;
    const auto result = buffers_[p_replica].ReadFrom(sockets_[p_replica].Get());
    const bool valid = buffers_[p_replica].Drain(
        [this, &received](std::span<const std::byte> p_bytes)
        {
          const auto message = DecodeMessage(p_bytes);
          const bool for_this_client = message.has_value() &&
                                       message->header_.cluster_ == options_.cluster_ &&
                                       message->header_.client_ == options_.client_;
          if (!for_this_client || received != Received::Nothing)
          {
            return;
          }
          const Header &header = message->header_;
          if (header.command_ == Command::Reply && header.request_ == message_request_)
          {
            view_ = std::max(view_, header.view_);
            reply_.assign(message->body_.begin(), message->body_.end());
            received = Received::Reply;
          }
          // A replica that is behind does not know a new session. Its
          // notice has an older view than a reply that this client has seen.
          else if (header.command_ == Command::Eviction && header.view_ >= view_)
          {
            received = Received::Eviction;
          }
        });
    if (result == internal::ReadResult::Closed || !valid)
    {
      sockets_[p_replica].Reset();
    }
    return received;
  }

  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError>
  Wait(std::chrono::milliseconds p_timeout)
  {
    const std::uint64_t deadline_ns =
        internal::MonotonicNs() +
        static_cast<std::uint64_t>(std::max<std::int64_t>(p_timeout.count(), 0)) * kNsPerMs;
    // Two steps at most: the registration, then the operation.
    while (true)
    {
      if (!registered_)
      {
        Encode(0, {});
      }
      else if (message_request_ != request_ + 1)
      {
        Encode(request_ + 1, body_);
      }
      const Received received = Exchange(deadline_ns);
      if (received == Received::Nothing)
      {
        return std::unexpected(ClientError::Timeout);
      }
      if (received == Received::Eviction)
      {
        // The cluster has no record of this client any more, so the same
        // request could execute twice. Start a new session.
        options_.client_ = RandomClientId();
        registered_ = false;
        request_ = 0;
        message_request_ = kNoRequest;
        pending_ = false;
        return std::unexpected(ClientError::Evicted);
      }
      if (!registered_)
      {
        registered_ = true;
        continue;
      }
      request_ += 1;
      pending_ = false;
      return std::move(reply_);
    }
  }

  // Sends `message_` until its reply or an eviction notice arrives, or the
  // deadline passes.
  [[nodiscard]] Received Exchange(std::uint64_t p_deadline_ns)
  {
    std::uint64_t next_send_ns = internal::MonotonicNs();
    bool first_send = true;
    while (true)
    {
      std::uint64_t now_ns = internal::MonotonicNs();
      if (now_ns >= next_send_ns)
      {
        // The primary alone at first. If it does not answer, every replica:
        // the primary can be a different one now, and it replies on the
        // connection that carried the request.
        for (std::size_t replica = 0; replica < addresses_.size(); ++replica)
        {
          if (!first_send || replica == view_ % addresses_.size())
          {
            Send(replica);
          }
        }
        first_send = false;
        next_send_ns = now_ns + options_.retry_ms_ * kNsPerMs;
      }
      std::vector<pollfd> waiters;
      std::vector<std::size_t> replicas;
      for (std::size_t replica = 0; replica < sockets_.size(); ++replica)
      {
        if (sockets_[replica].IsOpen())
        {
          waiters.push_back(pollfd{sockets_[replica].Get(), POLLIN, 0});
          replicas.push_back(replica);
        }
      }
      now_ns = internal::MonotonicNs();
      const std::uint64_t wake_ns = std::min(p_deadline_ns, next_send_ns);
      const std::uint64_t wait_ns = wake_ns > now_ns ? wake_ns - now_ns : 0;
      const int wait_ms = static_cast<int>((wait_ns + kNsPerMs - 1) / kNsPerMs);
      const int ready_count = ::poll(waiters.data(), waiters.size(), wait_ms);
      for (std::size_t index = 0; ready_count > 0 && index < waiters.size(); ++index)
      {
        if (waiters[index].revents == 0)
        {
          continue;
        }
        const Received received = Receive(replicas[index]);
        if (received != Received::Nothing)
        {
          return received;
        }
      }
      if (internal::MonotonicNs() >= p_deadline_ns)
      {
        return Received::Nothing;
      }
    }
  }

  ClientOptions options_;
  std::vector<sockaddr_in> addresses_;
  std::vector<UniqueFd> sockets_;
  std::vector<internal::FrameBuffer> buffers_;
  static constexpr std::uint64_t kNoRequest = ~std::uint64_t{0};

  // The operation that waits for its reply.
  std::vector<std::byte> body_;
  // The encoded message in progress, and its request number.
  std::vector<std::byte> message_;
  std::uint64_t message_request_ = kNoRequest;
  std::vector<std::byte> reply_;
  // The last request number that got its reply.
  std::uint64_t request_ = 0;
  // The highest view in a reply. The primary of that view gets requests first.
  std::uint32_t view_ = 0;
  bool registered_ = false;
  bool pending_ = false;
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
  if (p_options.client_ == 0)
  {
    p_options.client_ = RandomClientId();
  }
  return Client{std::make_unique<Impl>(std::move(p_options), std::move(addresses))};
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
