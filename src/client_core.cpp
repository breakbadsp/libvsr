#include "vsr/client_core.hpp"

#include "vsr/assert.hpp"
#include "vsr/message.hpp"

#include <algorithm>

namespace vsr
{

ClientCore::ClientCore(const ClientCoreOptions &p_options)
    : cluster_(p_options.cluster_), client_(p_options.client_), random_state_(p_options.seed_),
      retry_interval_(p_options.retry_interval_), body_bytes_max_(p_options.body_bytes_max_),
      replica_count_(p_options.replica_count_)
{
  VSR_ASSERT(replica_count_ > 0);
  VSR_ASSERT(retry_interval_ > 0);
  body_.reserve(body_bytes_max_);
  message_.reserve(kHeaderBytes + body_bytes_max_);
  if (client_ == 0)
  {
    client_ = NextClientId();
  }
}

// SplitMix64: the same seed gives the same identifiers on each platform.
std::uint64_t ClientCore::NextClientId() noexcept
{
  std::uint64_t id = 0;
  while (id == 0)
  {
    random_state_ += 0x9E3779B97F4A7C15ULL;
    id = random_state_;
    id = (id ^ (id >> 30)) * 0xBF58476D1CE4E5B9ULL;
    id = (id ^ (id >> 27)) * 0x94D049BB133111EBULL;
    id ^= id >> 31;
  }
  return id;
}

void ClientCore::Encode(std::uint64_t p_request, std::span<const std::byte> p_body)
{
  Header header;
  header.command_ = Command::Request;
  header.cluster_ = cluster_;
  header.client_ = client_;
  header.request_ = p_request;
  header.session_ = p_request == 0 ? 0 : session_;
  // Not more than the capacity that the constructor reserved.
  message_.resize(kHeaderBytes + p_body.size());
  message_.resize(EncodeMessage(header, p_body, {}, message_));
  primary_only_ = true;
  next_send_ = 0;
}

void ClientCore::StartWait(std::uint64_t p_deadline) noexcept
{
  deadline_ = p_deadline;
  waiting_ = true;
}

std::expected<void, ClientCoreError> ClientCore::Request(std::span<const std::byte> p_body,
                                                         std::uint64_t p_deadline)
{
  if (stage_ != Stage::Idle)
  {
    return std::unexpected(ClientCoreError::Pending);
  }
  if (p_body.size() > body_bytes_max_)
  {
    return std::unexpected(ClientCoreError::TooLarge);
  }
  body_.assign(p_body.begin(), p_body.end());
  if (registered_)
  {
    stage_ = Stage::Requesting;
    Encode(request_ + 1, body_);
  }
  else
  {
    stage_ = Stage::Registering;
    Encode(0, {});
  }
  StartWait(p_deadline);
  return {};
}

std::expected<void, ClientCoreError> ClientCore::Resume(std::uint64_t p_deadline)
{
  if (stage_ == Stage::Idle)
  {
    return std::unexpected(ClientCoreError::NotPending);
  }
  // The message in progress and its send time continue.
  StartWait(p_deadline);
  return {};
}

ClientSend ClientCore::Poll(std::uint64_t p_now)
{
  if (!waiting_ || p_now < next_send_)
  {
    return {};
  }
  ClientSend send;
  send.message_ = message_;
  if (primary_only_)
  {
    send.replica_begin_ = static_cast<std::uint8_t>(view_ % replica_count_);
    send.replica_end_ = static_cast<std::uint8_t>(send.replica_begin_ + 1);
  }
  else
  {
    send.replica_end_ = replica_count_;
  }
  primary_only_ = false;
  next_send_ = retry_interval_ > kClientNever - p_now ? kClientNever : p_now + retry_interval_;
  return send;
}

void ClientCore::StartSession() noexcept
{
  // The cluster has no record of this client any more, so the same request
  // could execute twice. A new identifier starts a new session.
  client_ = NextClientId();
  registered_ = false;
  request_ = 0;
  stage_ = Stage::Idle;
  waiting_ = false;
}

ClientEvent ClientCore::OnMessage(std::span<const std::byte> p_bytes)
{
  if (!waiting_)
  {
    return {};
  }
  const auto message = DecodeMessage(p_bytes);
  if (!message.has_value())
  {
    return {};
  }
  const Header &header = message->header_;
  if (header.cluster_ != cluster_ || header.client_ != client_)
  {
    return {};
  }
  if (header.command_ == Command::Eviction)
  {
    // A replica that is behind does not know a new session. Its notice has
    // an older view than a reply that this client has seen.
    if (header.view_ < view_)
    {
      return {};
    }
    StartSession();
    return ClientEvent{ClientEventKind::Evicted, {}};
  }
  if (header.command_ != Command::Reply)
  {
    return {};
  }
  if (stage_ == Stage::Registering)
  {
    if (header.request_ != 0)
    {
      return {};
    }
    view_ = std::max(view_, header.view_);
    session_ = header.session_;
    registered_ = true;
    stage_ = Stage::Requesting;
    Encode(request_ + 1, body_);
    return {};
  }
  if (header.request_ != request_ + 1 || header.session_ != session_)
  {
    return {};
  }
  view_ = std::max(view_, header.view_);
  request_ += 1;
  stage_ = Stage::Idle;
  waiting_ = false;
  return ClientEvent{ClientEventKind::Reply, message->body_};
}

bool ClientCore::Expire(std::uint64_t p_now) noexcept
{
  if (!waiting_ || p_now < deadline_)
  {
    return false;
  }
  waiting_ = false;
  return true;
}

std::uint64_t ClientCore::WakeTime() const noexcept
{
  return waiting_ ? std::min(deadline_, next_send_) : kClientNever;
}

} // namespace vsr
