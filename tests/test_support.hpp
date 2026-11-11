#pragma once

#include "vsr/interfaces.hpp"
#include "vsr/message.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vsr::test
{

[[nodiscard]] inline std::span<const std::byte> Bytes(std::string_view p_text) noexcept
{
  return std::as_bytes(std::span{p_text.data(), p_text.size()});
}

[[nodiscard]] inline std::string Text(std::span<const std::byte> p_bytes)
{
  return std::string{reinterpret_cast<const char *>(p_bytes.data()), p_bytes.size()};
}

// Storage in memory. It keeps its content when a replica is opened again.
class MemoryStorage final : public Storage
{
public:
  explicit MemoryStorage(std::uint64_t p_bytes) : bytes_(p_bytes) {}

  [[nodiscard]] std::expected<void, StorageError> Read(std::uint64_t p_offset,
                                                       std::span<std::byte> p_out) override
  {
    std::memcpy(p_out.data(), bytes_.data() + p_offset, p_out.size());
    return {};
  }

  [[nodiscard]] std::expected<void, StorageError> Write(std::uint64_t p_offset,
                                                        std::span<const std::byte> p_bytes) override
  {
    std::memcpy(bytes_.data() + p_offset, p_bytes.data(), p_bytes.size());
    return {};
  }

  [[nodiscard]] std::expected<void, StorageError> Sync() override
  {
    sync_count_ += 1;
    return {};
  }

  std::vector<std::byte> bytes_;
  std::uint64_t sync_count_ = 0;
};

struct SentReply
{
  std::uint64_t client_ = 0;
  std::uint64_t request_ = 0;
  std::uint64_t op_ = 0;
  std::string body_;
};

// Records the replies and the evictions. A cluster of one replica sends
// nothing to replicas.
class RecordingBus final : public MessageBus
{
public:
  void SendToReplica(std::uint8_t p_replica, std::span<const std::byte> p_message) override
  {
    static_cast<void>(p_replica);
    static_cast<void>(p_message);
    replica_message_count_ += 1;
  }

  void SendToClient(std::uint64_t p_client, std::span<const std::byte> p_message) override
  {
    const auto message = DecodeMessage(p_message);
    if (!message)
    {
      invalid_reply_count_ += 1;
      return;
    }
    if (message->header_.command_ == Command::Eviction)
    {
      evicted_clients_.push_back(p_client);
      return;
    }
    replies_.push_back(
        SentReply{p_client, message->header_.request_, message->header_.op_, Text(message->body_)});
  }

  std::vector<SentReply> replies_;
  std::vector<std::uint64_t> evicted_clients_;
  std::uint64_t replica_message_count_ = 0;
  std::uint64_t invalid_reply_count_ = 0;
};

// A key-value store. A body "key=value" sets the key and replies with the
// value that the key had before. The snapshot is "key=value\n" lines in key
// order.
class KeyValueStateMachine final : public StateMachine
{
public:
  [[nodiscard]] std::size_t Execute(const Operation &p_operation, std::span<const std::byte> p_body,
                                    std::span<std::byte> p_reply) override
  {
    last_timestamp_ns_ = p_operation.timestamp_ns_;
    const std::string text = Text(p_body);
    const auto separator = text.find('=');
    if (separator == std::string::npos)
    {
      return 0;
    }
    auto &value = values_[text.substr(0, separator)];
    const std::string previous = value;
    value = text.substr(separator + 1);
    std::memcpy(p_reply.data(), previous.data(), previous.size());
    return previous.size();
  }

  [[nodiscard]] std::expected<std::size_t, std::monostate>
  Snapshot(std::span<std::byte> p_out) override
  {
    std::string text;
    for (const auto &[key, value] : values_)
    {
      text += key + "=" + value + "\n";
    }
    if (text.size() > p_out.size())
    {
      return std::unexpected(std::monostate{});
    }
    std::memcpy(p_out.data(), text.data(), text.size());
    return text.size();
  }

  [[nodiscard]] bool Restore(std::span<const std::byte> p_snapshot) override
  {
    values_.clear();
    const std::string text = Text(p_snapshot);
    std::size_t line_start = 0;
    while (line_start < text.size())
    {
      const auto line_end = text.find('\n', line_start);
      const auto separator = text.find('=', line_start);
      if (line_end == std::string::npos || separator == std::string::npos || separator > line_end)
      {
        return false;
      }
      values_[text.substr(line_start, separator - line_start)] =
          text.substr(separator + 1, line_end - separator - 1);
      line_start = line_end + 1;
    }
    return true;
  }

  std::map<std::string, std::string> values_;
  std::uint64_t last_timestamp_ns_ = 0;
};

} // namespace vsr::test
