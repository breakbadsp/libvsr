#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vsr::io
{

struct ClientOptions
{
  std::uint64_t cluster_ = 0;
  std::vector<std::string> addresses_;
  // 0 selects a random identifier. A fixed identifier is valid only if the
  // request numbers also continue after a restart of the client.
  std::uint64_t client_ = 0;
  std::uint32_t body_bytes_max_ = 64 * 1024;
  std::uint32_t retry_ms_ = 200;
};

enum class ClientError
{
  // No reply yet. The outcome is not known. Call `Resume` to wait again.
  Timeout = 0,
  TooLarge,
  // `Request` while an earlier request has no reply yet.
  Pending,
  // `Resume` without a request that waits.
  NotPending,
  // The cluster removed the session of this client, because more clients
  // were active than `Config::clients_max_`. The outcome of the request is
  // not known, and it cannot be learned. The next `Request` starts a new
  // session.
  Evicted,
};

// The default client. One request is in progress at a time. To do more work
// in one round trip, put several operations in one request body.
class Client final
{
public:
  [[nodiscard]] static std::expected<Client, std::string> Create(ClientOptions p_options);

  Client(Client &&) noexcept;
  Client &operator=(Client &&) noexcept;
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;
  ~Client();

  // Sends one operation and waits for its reply. The operation executes at
  // most once, also when the client sends it again or the primary changes.
  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError>
  Request(std::span<const std::byte> p_body, std::chrono::milliseconds p_timeout);

  // Waits again for the reply to the request that timed out.
  [[nodiscard]] std::expected<std::vector<std::byte>, ClientError>
  Resume(std::chrono::milliseconds p_timeout);

  [[nodiscard]] std::uint64_t Id() const noexcept;

private:
  class Impl;
  explicit Client(std::unique_ptr<Impl> p_impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace vsr::io
