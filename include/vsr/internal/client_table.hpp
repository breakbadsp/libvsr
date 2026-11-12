#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vsr::internal
{

struct ClientEntry
{
  std::uint64_t client_ = 0;
  std::uint64_t session_ = 0;
  // The latest request of the client that is committed.
  std::uint64_t request_ = 0;
  // The operation that committed it.
  std::uint64_t op_ = 0;
  std::uint32_t reply_bytes_ = 0;
  std::uint32_t reply_slot_index_ = 0;
};

// The latest committed request and its reply, for each client. It is part of
// the replicated state: it changes only when an operation commits, so it is
// equal on all replicas at the same operation.
class ClientTable final
{
public:
  ClientTable(std::uint32_t p_clients_max, std::uint32_t p_body_bytes_max);

  [[nodiscard]] const ClientEntry *Find(std::uint64_t p_client) const noexcept;
  [[nodiscard]] std::span<const std::byte> Reply(const ClientEntry &p_entry) const noexcept;

  // Records a committed request. If the table is full and the client is
  // new, the client with the oldest committed operation is removed.
  void Commit(const ClientEntry &p_commit, std::span<const std::byte> p_reply);

  // Writes the table to the start of `p_out` in a fixed order. Returns the size.
  [[nodiscard]] std::size_t Serialize(std::span<std::byte> p_out) const;
  // Replaces the table. Returns the number of bytes read, or nothing if the
  // input is not a valid table.
  [[nodiscard]] std::optional<std::size_t> Deserialize(std::span<const std::byte> p_in);

private:
  [[nodiscard]] std::span<std::byte> ReplySlot(std::uint32_t p_slot_index) noexcept;
  void Clear();

  std::uint32_t clients_max_;
  std::uint32_t body_bytes_max_;
  // Sorted by client, so that lookup is a binary search and the serialized
  // form does not depend on the order of insertion.
  std::vector<ClientEntry> entries_;
  std::vector<std::byte> replies_;
  std::vector<std::uint32_t> free_slot_indexes_;
};

} // namespace vsr::internal
