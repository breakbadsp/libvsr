#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

// The client side of the protocol, without input and output. The core has
// no socket, no thread, and no clock. The host gives it each request, each
// received message, and the time. The core says which message to send to
// which replica, and when a request is complete. The same inputs give the
// same outputs, so a simulator can run the code that `vsr::io::Client` runs.
//
// The rules that the core obeys are in `docs/host-contract.md`, "Clients".

namespace vsr
{

// A time that never comes. As a deadline: no timeout.
inline constexpr std::uint64_t kClientNever = std::numeric_limits<std::uint64_t>::max();

// Times and intervals are in one unit that the host selects, for example
// nanoseconds or ticks. The core only compares and adds them.
struct ClientCoreOptions
{
  std::uint64_t cluster_ = 0;
  // 0 lets the core take an identifier from `seed_`. A fixed identifier is
  // valid only if the request numbers also continue after a restart of the
  // client.
  std::uint64_t client_ = 0;
  // The source of each identifier that the core selects: the first one if
  // `client_` is 0, and a new one after each eviction. One seed gives one
  // sequence of identifiers.
  std::uint64_t seed_ = 0;
  // Not zero.
  std::uint8_t replica_count_ = 0;
  std::uint32_t body_bytes_max_ = 64 * 1024;
  // The interval after which a message without an answer goes out again.
  // Not zero.
  std::uint64_t retry_interval_ = 0;
};

enum class ClientCoreError
{
  TooLarge = 0,
  // `Request` while an earlier request has no result yet.
  Pending,
  // `Resume` without a request in progress.
  NotPending,
};

enum class ClientEventKind : std::uint8_t
{
  // The message did not complete the request.
  None = 0,
  // The reply to the request in progress.
  Reply,
  // The cluster removed the session of this client. The outcome of the
  // request is not known, and it cannot be learned. The core has a new
  // identifier, and the next `Request` starts a new session.
  Evicted,
};

struct ClientEvent
{
  ClientEventKind kind_ = ClientEventKind::None;
  // For `Reply`: the reply body. It refers to the bytes that the host gave
  // to `OnMessage`.
  std::span<const std::byte> reply_;
};

// Send `message_` to each replica in [`replica_begin_`, `replica_end_`). An
// empty range: send nothing. `message_` is valid until the next call of a
// member function that is not `const`.
struct ClientSend
{
  std::span<const std::byte> message_;
  std::uint8_t replica_begin_ = 0;
  std::uint8_t replica_end_ = 0;
};

// One request is in progress at a time. The host loop for one request:
//
//   core.Request(body, deadline)        or core.Resume(deadline)
//   loop:
//       send = core.Poll(now)           give send.message_ to each named replica
//       wait for a message, or until core.WakeTime()
//       for each message: event = core.OnMessage(bytes)
//                         stop if event.kind_ is not None
//       stop if core.Expire(now)        timeout: the request stays in progress
//
// The network can drop, delay, repeat, and reorder messages in both
// directions. The core sends a message again until its answer arrives.
class ClientCore final
{
public:
  // Allocates the two buffers. No other member function allocates.
  explicit ClientCore(const ClientCoreOptions &p_options);

  // Starts one operation, and a wait for its result until `p_deadline`.
  // The first request of a session makes the core register first.
  [[nodiscard]] std::expected<void, ClientCoreError> Request(std::span<const std::byte> p_body,
                                                             std::uint64_t p_deadline);

  // Starts a new wait for the request in progress, after `Expire` returned
  // true.
  [[nodiscard]] std::expected<void, ClientCoreError> Resume(std::uint64_t p_deadline);

  // What to send at `p_now`. The first time, the message goes to the
  // primary of the highest view that a reply showed. After each retry
  // interval without an answer it goes to every replica: the primary can be
  // a different one now. Sends nothing while no wait is in progress.
  [[nodiscard]] ClientSend Poll(std::uint64_t p_now);

  // Gives the core one message from the network. The core ignores a message
  // that is not valid, that is for a different client, or that does not
  // answer the message in progress. It also ignores each message while no
  // wait is in progress: the cluster keeps the reply and gives it again.
  [[nodiscard]] ClientEvent OnMessage(std::span<const std::byte> p_bytes);

  // The timeout decision. Returns true if a wait is in progress and its
  // deadline is not after `p_now`. The wait then ends, and the request
  // stays in progress: its outcome is not known. Call `Resume` to wait
  // again.
  [[nodiscard]] bool Expire(std::uint64_t p_now) noexcept;

  // The next time at which `Poll` or `Expire` has work. `kClientNever`
  // while no wait is in progress.
  [[nodiscard]] std::uint64_t WakeTime() const noexcept;

  [[nodiscard]] std::uint64_t Id() const noexcept { return client_; }
  // A request has no result yet.
  [[nodiscard]] bool IsPending() const noexcept { return stage_ != Stage::Idle; }
  [[nodiscard]] bool IsWaiting() const noexcept { return waiting_; }
  // The highest view in a reply that the core accepted.
  [[nodiscard]] std::uint32_t View() const noexcept { return view_; }

private:
  enum class Stage : std::uint8_t
  {
    Idle = 0,
    // The registration is the message in progress. The operation waits in
    // `body_`.
    Registering,
    // The operation is the message in progress.
    Requesting,
  };

  [[nodiscard]] std::uint64_t NextClientId() noexcept;
  void Encode(std::uint64_t p_request, std::span<const std::byte> p_body);
  void StartWait(std::uint64_t p_deadline) noexcept;
  void StartSession() noexcept;

  std::uint64_t cluster_;
  std::uint64_t client_;
  std::uint64_t random_state_;
  std::uint64_t retry_interval_;
  std::uint32_t body_bytes_max_;
  std::uint8_t replica_count_;

  Stage stage_ = Stage::Idle;
  // The application waits for the result of the request in progress.
  bool waiting_ = false;
  // The cluster holds a session for `client_`, as far as the core knows.
  bool registered_ = false;
  // The message in progress went to the supposed primary only, or not yet.
  // This state belongs to the message, not to the wait: a host that waits
  // in steps shorter than the retry interval must also reach every replica.
  bool primary_only_ = true;
  // The highest view in a reply. The primary of that view gets requests first.
  std::uint32_t view_ = 0;
  std::uint64_t session_ = 0;
  // The last request number that got its reply.
  std::uint64_t request_ = 0;
  // When to send the message in progress again. 0: now.
  std::uint64_t next_send_ = 0;
  std::uint64_t deadline_ = kClientNever;
  // The operation that waits for its reply.
  std::vector<std::byte> body_;
  // The encoded message in progress.
  std::vector<std::byte> message_;
};

} // namespace vsr
