#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

// The wire format between replicas, and between the default client and a
// replica. The format is fixed by the library. The operation body and the
// reply body are opaque bytes that belong to the application.

namespace vsr
{

inline constexpr std::uint16_t kProtocolVersion = 2;

enum class Command : std::uint8_t
{
  Reserved = 0,
  Request,
  Prepare,
  PrepareOk,
  Reply,
  Commit,
  StartViewChange,
  DoViewChange,
  StartView,
  RequestPrepare,
  RequestSnapshot,
  Snapshot,
  Eviction,
};

inline constexpr std::uint8_t kCommandCount = 13;

// Each message starts with this header. A field that a command does not use
// is zero.
//
//   Request          client_, request_, session_; body: the operation. Request number
//                    0 registers the client and has no body.
//   Prepare          commit_, checkpoint_*; body: EntryHeader + operation.
//   PrepareOk        op_, log_checksum_: the sender holds the log up to op_.
//   Reply            client_, request_, session_, op_; body: the reply.
//   Commit           op_, log_checksum_: the head of the log of the primary;
//   StartView        commit_, checkpoint_*. StartView opens a view.
//   StartViewChange  only view_.
//   DoViewChange     log_view_, op_, log_checksum_, commit_.
//   RequestPrepare   op_: the first operation that the sender needs.
//   RequestSnapshot  checkpoint_op_, offset_.
//   Snapshot         checkpoint_op_, log_checksum_: the entry at the
//                    checkpoint; checkpoint_checksum_, total_bytes_, offset_;
//                    body: one chunk.
//   Eviction         client_: the client has no session. To a client.
struct Header
{
  std::uint64_t checksum_ = 0;
  std::uint64_t checksum_body_ = 0;
  std::uint64_t cluster_ = 0;
  std::uint64_t client_ = 0;
  std::uint64_t request_ = 0;
  // The committed registration operation. Zero only for registration.
  std::uint64_t session_ = 0;
  std::uint64_t op_ = 0;
  std::uint64_t commit_ = 0;
  std::uint64_t log_checksum_ = 0;
  std::uint64_t checkpoint_op_ = 0;
  std::uint64_t checkpoint_checksum_ = 0;
  std::uint64_t offset_ = 0;
  std::uint64_t total_bytes_ = 0;
  std::uint32_t view_ = 0;
  std::uint32_t log_view_ = 0;
  // Header and body.
  std::uint32_t size_ = 0;
  std::uint16_t version_ = kProtocolVersion;
  Command command_ = Command::Reserved;
  std::uint8_t replica_ = 0;
  // Request only: 1 if a replica sent the request on to the primary.
  std::uint8_t forwarded_ = 0;
  std::uint8_t reserved_[7] = {};
  // The head of the log of the primary when it opened the view. In each
  // message that only a primary sends.
  std::uint64_t view_start_op_ = 0;
};

static_assert(sizeof(Header) == 136, "the wire format is fixed");
static_assert(std::has_unique_object_representations_v<Header>, "no padding in the wire format");

// One log entry. It is immutable after the primary makes it: `parent_` links
// it to the entry before it, so equal checksums mean equal log prefixes.
struct EntryHeader
{
  std::uint64_t checksum_ = 0;
  std::uint64_t checksum_body_ = 0;
  std::uint64_t parent_ = 0;
  std::uint64_t cluster_ = 0;
  std::uint64_t client_ = 0;
  std::uint64_t request_ = 0;
  // The committed registration operation. Zero only for registration.
  std::uint64_t session_ = 0;
  std::uint64_t op_ = 0;
  std::uint64_t timestamp_ns_ = 0;
  // The view in which the primary made the entry.
  std::uint32_t view_ = 0;
  std::uint32_t body_bytes_ = 0;
  std::uint16_t version_ = kProtocolVersion;
  std::uint8_t reserved_[6] = {};
};

static_assert(sizeof(EntryHeader) == 88, "the log format is fixed");
static_assert(std::has_unique_object_representations_v<EntryHeader>, "no padding in the log");

inline constexpr std::size_t kHeaderBytes = sizeof(Header);
inline constexpr std::size_t kEntryHeaderBytes = sizeof(EntryHeader);

// Largest message for a given body limit: a Prepare.
[[nodiscard]] constexpr std::size_t MessageBytesMax(std::uint32_t p_body_bytes_max) noexcept
{
  return kHeaderBytes + kEntryHeaderBytes + p_body_bytes_max;
}

struct Message
{
  Header header_;
  std::span<const std::byte> body_;
};

struct Entry
{
  EntryHeader header_;
  std::span<const std::byte> body_;
};

enum class DecodeError
{
  Short = 0,
  Size,
  Version,
  Command,
  HeaderChecksum,
  BodyChecksum,
  Entry,
};

// Checks a message from the network. The body span refers to `p_bytes`.
[[nodiscard]] std::expected<Message, DecodeError> DecodeMessage(std::span<const std::byte> p_bytes);

// Checks a log entry: a Prepare body or the content of a log slot. Bytes
// after the entry are permitted, because a log slot has a fixed size.
[[nodiscard]] std::expected<Entry, DecodeError> DecodeEntry(std::span<const std::byte> p_bytes);

// Sets the size and both checksums, then writes the header and the body
// parts to `p_out`. Returns the message size. `p_out` must be large enough.
[[nodiscard]] std::size_t EncodeMessage(Header p_header, std::span<const std::byte> p_body_first,
                                        std::span<const std::byte> p_body_second,
                                        std::span<std::byte> p_out);

// Sets both checksums of a log entry header.
void SealEntry(EntryHeader &p_header, std::span<const std::byte> p_body);

// Operation 0 is not a real entry. Its checksum starts the hash chain.
[[nodiscard]] std::uint64_t RootChecksum(std::uint64_t p_cluster);

// Size of the complete message that starts with this header, if the first
// `kHeaderBytes` of a stream are available. For stream transports.
[[nodiscard]] std::expected<std::size_t, DecodeError>
PeekMessageBytes(std::span<const std::byte> p_bytes, std::uint32_t p_body_bytes_max);

} // namespace vsr
