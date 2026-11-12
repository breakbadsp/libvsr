#pragma once

#include "vsr/interfaces.hpp"
#include "vsr/internal/layout.hpp"
#include "vsr/message.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace vsr::internal
{

enum class JournalReadError
{
  Io = 0,
  Corrupt,
};

// The log of one replica: a ring of slots on storage, and the entry headers
// in memory.
//
// The entries are the operations in (Floor(), Head()]. They are contiguous
// and hash-chained: each entry names the checksum of the entry before it.
// The entry at Floor() is gone, but its checksum is known, so the chain has
// a start.
class Journal final
{
public:
  Journal(Storage *p_storage, const Layout &p_layout);

  // Finds the chain that goes through the checkpoint. Entries that do not
  // link to it, or that fail a checksum, are not part of the log.
  [[nodiscard]] std::expected<void, StorageError> Recover(std::uint64_t p_checkpoint_op,
                                                          std::uint64_t p_checkpoint_checksum);

  [[nodiscard]] std::uint64_t Head() const noexcept { return head_op_; }

  [[nodiscard]] std::uint64_t Floor() const noexcept { return floor_op_; }

  [[nodiscard]] bool Dirty() const noexcept { return dirty_; }

  [[nodiscard]] std::uint64_t HeadChecksum() const noexcept { return ChecksumAt(head_op_); }

  // Requires Floor() <= p_op <= Head().
  [[nodiscard]] std::uint64_t ChecksumAt(std::uint64_t p_op) const noexcept;

  // Requires Floor() < p_op <= Head().
  [[nodiscard]] const EntryHeader &HeaderAt(std::uint64_t p_op) const noexcept;

  // Requires an entry for Head() + 1 that links to HeadChecksum(). The write
  // is not durable until Sync().
  [[nodiscard]] std::expected<void, StorageError> Append(const EntryHeader &p_header,
                                                         std::span<const std::byte> p_body);

  // Removes the entries after `p_op`, durably. Requires Floor() <= p_op <= Head().
  [[nodiscard]] std::expected<void, StorageError> TruncateTo(std::uint64_t p_op);

  // Replaces the log with an empty log that starts after `p_op`, durably.
  [[nodiscard]] std::expected<void, StorageError> Reset(std::uint64_t p_op,
                                                        std::uint64_t p_checksum);

  // Reads the body of the entry at `p_op` into the start of `p_out`.
  [[nodiscard]] std::expected<std::span<const std::byte>, JournalReadError>
  ReadBody(std::uint64_t p_op, std::span<std::byte> p_out);

  [[nodiscard]] std::expected<void, StorageError> Sync();

private:
  [[nodiscard]] std::expected<bool, StorageError> ReadSlot(std::uint64_t p_op,
                                                           EntryHeader &p_header);
  [[nodiscard]] std::expected<void, StorageError> EraseSlot(std::uint64_t p_op);

  Storage *storage_;
  Layout layout_;
  std::vector<EntryHeader> headers_;
  std::vector<std::byte> slot_buffer_;
  std::uint64_t floor_op_ = 0;
  std::uint64_t floor_checksum_ = 0;
  std::uint64_t head_op_ = 0;
  bool dirty_ = false;
};

} // namespace vsr::internal
