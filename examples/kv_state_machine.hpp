#pragma once

#include "vsr/interfaces.hpp"

#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace kv
{

// A key-value store, as an example of a replicated application.
//
// Operation "key=value" sets the key. Operation "key" reads it. The reply is
// the value before the operation. The rules that make it safe to replicate:
// - `Execute` uses only its arguments and the map. It reads no clock, no
//   random number, and no file.
// - `std::map` iterates in key order, so the snapshot bytes are equal on
//   all replicas. A hash map would give a different order on each replica.
// - An operation that it cannot parse still gets a reply, and changes nothing.
class StateMachine final : public vsr::StateMachine
{
public:
  [[nodiscard]] std::size_t Execute(const vsr::Operation &p_operation,
                                    std::span<const std::byte> p_body,
                                    std::span<std::byte> p_reply) override
  {
    static_cast<void>(p_operation);
    const std::string_view text{reinterpret_cast<const char *>(p_body.data()), p_body.size()};
    if (text.find('\n') != std::string_view::npos)
    {
      // A line break would make the snapshot ambiguous.
      return 0;
    }
    const auto separator = text.find('=');
    const std::string key{text.substr(0, separator)};
    const auto found = values_.find(key);
    const std::string previous = found == values_.end() ? std::string{} : found->second;
    if (separator != std::string_view::npos)
    {
      values_[key] = std::string{text.substr(separator + 1)};
    }
    // The value came from a request body, so it fits in a reply.
    std::memcpy(p_reply.data(), previous.data(), previous.size());
    return previous.size();
  }

  [[nodiscard]] std::expected<std::size_t, std::monostate>
  Snapshot(std::span<std::byte> p_out) override
  {
    std::size_t used_bytes = 0;
    for (const auto &[key, value] : values_)
    {
      const std::string line = key + "=" + value + "\n";
      if (line.size() > p_out.size() - used_bytes)
      {
        return std::unexpected(std::monostate{});
      }
      std::memcpy(p_out.data() + used_bytes, line.data(), line.size());
      used_bytes += line.size();
    }
    return used_bytes;
  }

  [[nodiscard]] bool Restore(std::span<const std::byte> p_snapshot) override
  {
    values_.clear();
    std::string_view text{reinterpret_cast<const char *>(p_snapshot.data()), p_snapshot.size()};
    while (!text.empty())
    {
      const auto line_end = text.find('\n');
      const auto separator = text.find('=');
      if (line_end == std::string_view::npos || separator > line_end)
      {
        return false;
      }
      values_[std::string{text.substr(0, separator)}] =
          std::string{text.substr(separator + 1, line_end - separator - 1)};
      text.remove_prefix(line_end + 1);
    }
    return true;
  }

private:
  std::map<std::string, std::string> values_;
};

} // namespace kv
