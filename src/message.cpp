#include "vsr/message.hpp"

#include "vsr/assert.hpp"
#include "vsr/checksum.hpp"

#include <cstring>
#include <utility>

namespace vsr
{

namespace
{

// The checksum field is first, and it covers all bytes after itself.
constexpr std::size_t kChecksumFieldBytes = sizeof(std::uint64_t);

template <typename THeader>
[[nodiscard]] std::uint64_t ChecksumAfterField(const THeader &p_header) noexcept
{
  const auto bytes = std::as_bytes(std::span<const THeader, 1>{&p_header, 1});
  return Checksum(bytes.subspan(kChecksumFieldBytes));
}

} // namespace

std::expected<Message, DecodeError> DecodeMessage(std::span<const std::byte> p_bytes)
{
  if (p_bytes.size() < kHeaderBytes)
  {
    return std::unexpected(DecodeError::Short);
  }
  Header header;
  std::memcpy(&header, p_bytes.data(), kHeaderBytes);
  if (header.checksum_ != ChecksumAfterField(header))
  {
    return std::unexpected(DecodeError::HeaderChecksum);
  }
  if (header.version_ != kProtocolVersion)
  {
    return std::unexpected(DecodeError::Version);
  }
  const auto command = std::to_underlying(header.command_);
  if (command == 0 || command >= kCommandCount)
  {
    return std::unexpected(DecodeError::Command);
  }
  if (header.size_ != p_bytes.size())
  {
    return std::unexpected(DecodeError::Size);
  }
  const auto body = p_bytes.subspan(kHeaderBytes);
  if (header.checksum_body_ != Checksum(body))
  {
    return std::unexpected(DecodeError::BodyChecksum);
  }
  if (header.command_ == Command::Prepare)
  {
    const auto entry = DecodeEntry(body);
    if (!entry)
    {
      return std::unexpected(DecodeError::Entry);
    }
    if (kEntryHeaderBytes + entry->body_.size() != body.size())
    {
      return std::unexpected(DecodeError::Entry);
    }
    if (entry->header_.cluster_ != header.cluster_)
    {
      return std::unexpected(DecodeError::Entry);
    }
  }
  return Message{header, body};
}

std::expected<Entry, DecodeError> DecodeEntry(std::span<const std::byte> p_bytes)
{
  if (p_bytes.size() < kEntryHeaderBytes)
  {
    return std::unexpected(DecodeError::Short);
  }
  EntryHeader header;
  std::memcpy(&header, p_bytes.data(), kEntryHeaderBytes);
  if (header.checksum_ != ChecksumAfterField(header))
  {
    return std::unexpected(DecodeError::HeaderChecksum);
  }
  if (header.version_ != kProtocolVersion)
  {
    return std::unexpected(DecodeError::Version);
  }
  if (header.op_ == 0)
  {
    return std::unexpected(DecodeError::Entry);
  }
  if (header.body_bytes_ > p_bytes.size() - kEntryHeaderBytes)
  {
    return std::unexpected(DecodeError::Size);
  }
  const auto body = p_bytes.subspan(kEntryHeaderBytes, header.body_bytes_);
  if (header.checksum_body_ != Checksum(body))
  {
    return std::unexpected(DecodeError::BodyChecksum);
  }
  return Entry{header, body};
}

std::size_t EncodeMessage(Header p_header, std::span<const std::byte> p_body_first,
                          std::span<const std::byte> p_body_second, std::span<std::byte> p_out)
{
  const std::size_t body_bytes = p_body_first.size() + p_body_second.size();
  const std::size_t message_bytes = kHeaderBytes + body_bytes;
  VSR_ASSERT(message_bytes <= p_out.size());
  VSR_ASSERT(std::in_range<std::uint32_t>(message_bytes));
  const auto body_out = p_out.subspan(kHeaderBytes, body_bytes);
  if (!p_body_first.empty())
  {
    std::memcpy(body_out.data(), p_body_first.data(), p_body_first.size());
  }
  if (!p_body_second.empty())
  {
    std::memcpy(body_out.data() + p_body_first.size(), p_body_second.data(), p_body_second.size());
  }
  Header header = p_header;
  header.version_ = kProtocolVersion;
  header.size_ = static_cast<std::uint32_t>(message_bytes);
  header.checksum_body_ = Checksum(body_out);
  header.checksum_ = ChecksumAfterField(header);
  std::memcpy(p_out.data(), &header, kHeaderBytes);
  return message_bytes;
}

void SealEntry(EntryHeader &p_header, std::span<const std::byte> p_body)
{
  VSR_ASSERT(std::in_range<std::uint32_t>(p_body.size()));
  p_header.version_ = kProtocolVersion;
  p_header.body_bytes_ = static_cast<std::uint32_t>(p_body.size());
  p_header.checksum_body_ = Checksum(p_body);
  p_header.checksum_ = ChecksumAfterField(p_header);
}

std::uint64_t RootChecksum(std::uint64_t p_cluster)
{
  EntryHeader root;
  root.cluster_ = p_cluster;
  return ChecksumAfterField(root);
}

std::expected<std::size_t, DecodeError> PeekMessageBytes(std::span<const std::byte> p_bytes,
                                                         std::uint32_t p_body_bytes_max)
{
  if (p_bytes.size() < kHeaderBytes)
  {
    return std::unexpected(DecodeError::Short);
  }
  Header header;
  std::memcpy(&header, p_bytes.data(), kHeaderBytes);
  if (header.checksum_ != ChecksumAfterField(header))
  {
    return std::unexpected(DecodeError::HeaderChecksum);
  }
  if (header.size_ < kHeaderBytes || header.size_ > MessageBytesMax(p_body_bytes_max))
  {
    return std::unexpected(DecodeError::Size);
  }
  return std::size_t{header.size_};
}

} // namespace vsr
