#include "vsr/internal/journal.hpp"

#include "vsr/assert.hpp"
#include "vsr/checksum.hpp"

#include <array>
#include <cstring>

namespace vsr::internal
{

Journal::Journal(Storage *p_storage, const Layout &p_layout)
    : storage_(p_storage), layout_(p_layout), headers_(p_layout.slot_count_),
      slot_buffer_(p_layout.slot_bytes_)
{
  VSR_ASSERT(storage_ != nullptr);
  VSR_ASSERT(layout_.slot_count_ > 0);
}

std::expected<bool, StorageError> Journal::ReadSlot(std::uint64_t p_op, EntryHeader &p_header)
{
  if (const auto read = storage_->Read(layout_.SlotOffset(p_op), slot_buffer_); !read)
  {
    return std::unexpected(read.error());
  }
  // The slot content is not trusted: a crash can leave a torn write.
  const auto entry = DecodeEntry(slot_buffer_);
  if (!entry || entry->header_.op_ != p_op)
  {
    return false;
  }
  p_header = entry->header_;
  return true;
}

std::expected<void, StorageError> Journal::Recover(std::uint64_t p_checkpoint_op,
                                                   std::uint64_t p_checkpoint_checksum)
{
  floor_op_ = p_checkpoint_op;
  floor_checksum_ = p_checkpoint_checksum;
  head_op_ = p_checkpoint_op;
  dirty_ = false;

  // Forward from the checkpoint. The ring cannot hold more than one lap.
  std::uint64_t parent = p_checkpoint_checksum;
  while (head_op_ - p_checkpoint_op < layout_.slot_count_)
  {
    EntryHeader header;
    const auto found = ReadSlot(head_op_ + 1, header);
    if (!found)
    {
      return std::unexpected(found.error());
    }
    if (!*found || header.parent_ != parent)
    {
      break;
    }
    head_op_ += 1;
    headers_[head_op_ % layout_.slot_count_] = header;
    parent = header.checksum_;
  }

  // Backward from the checkpoint. These entries are committed. They let this
  // replica send old entries to a replica that is behind.
  while (floor_op_ > 0 && head_op_ - floor_op_ < layout_.slot_count_)
  {
    EntryHeader header;
    const auto found = ReadSlot(floor_op_, header);
    if (!found)
    {
      return std::unexpected(found.error());
    }
    if (!*found || header.checksum_ != floor_checksum_)
    {
      break;
    }
    headers_[floor_op_ % layout_.slot_count_] = header;
    floor_op_ -= 1;
    floor_checksum_ = header.parent_;
  }
  VSR_ASSERT(floor_op_ <= p_checkpoint_op);
  VSR_ASSERT(head_op_ >= p_checkpoint_op);
  VSR_ASSERT(head_op_ - floor_op_ <= layout_.slot_count_);
  return {};
}

std::uint64_t Journal::ChecksumAt(std::uint64_t p_op) const noexcept
{
  VSR_ASSERT(p_op >= floor_op_);
  VSR_ASSERT(p_op <= head_op_);
  if (p_op == floor_op_)
  {
    return floor_checksum_;
  }
  return headers_[p_op % layout_.slot_count_].checksum_;
}

const EntryHeader &Journal::HeaderAt(std::uint64_t p_op) const noexcept
{
  VSR_ASSERT(p_op > floor_op_);
  VSR_ASSERT(p_op <= head_op_);
  const auto &header = headers_[p_op % layout_.slot_count_];
  VSR_ASSERT(header.op_ == p_op);
  return header;
}

std::expected<void, StorageError> Journal::Append(const EntryHeader &p_header,
                                                  std::span<const std::byte> p_body)
{
  VSR_ASSERT(p_header.op_ == head_op_ + 1);
  VSR_ASSERT(p_header.parent_ == HeadChecksum());
  VSR_ASSERT(p_header.body_bytes_ == p_body.size());
  VSR_ASSERT(kEntryHeaderBytes + p_body.size() <= slot_buffer_.size());

  // The new entry takes the slot of the entry one lap before it. That entry
  // becomes the floor, so keep its checksum.
  if (p_header.op_ > floor_op_ + layout_.slot_count_)
  {
    const std::uint64_t replaced_op = p_header.op_ - layout_.slot_count_;
    floor_checksum_ = ChecksumAt(replaced_op);
    floor_op_ = replaced_op;
  }
  // The ring holds at most one lap: the new entry and the floor fit.
  VSR_ASSERT(p_header.op_ - floor_op_ <= layout_.slot_count_);

  std::memcpy(slot_buffer_.data(), &p_header, kEntryHeaderBytes);
  if (!p_body.empty())
  {
    std::memcpy(slot_buffer_.data() + kEntryHeaderBytes, p_body.data(), p_body.size());
  }
  const std::span<const std::byte> slot{slot_buffer_.data(), kEntryHeaderBytes + p_body.size()};
  if (const auto written = storage_->Write(layout_.SlotOffset(p_header.op_), slot); !written)
  {
    return written;
  }
  head_op_ = p_header.op_;
  headers_[head_op_ % layout_.slot_count_] = p_header;
  dirty_ = true;
  // Read back the new head. The next entry links to this checksum.
  VSR_ASSERT(HeadChecksum() == p_header.checksum_);
  return {};
}

std::expected<void, StorageError> Journal::EraseSlot(std::uint64_t p_op)
{
  // A zero header fails the entry checks, so recovery stops the chain here.
  const std::array<std::byte, kEntryHeaderBytes> zeros{};
  if (const auto written = storage_->Write(layout_.SlotOffset(p_op), zeros); !written)
  {
    return written;
  }
  dirty_ = true;
  return Sync();
}

std::expected<void, StorageError> Journal::TruncateTo(std::uint64_t p_op)
{
  VSR_ASSERT(p_op >= floor_op_);
  VSR_ASSERT(p_op <= head_op_);
  if (p_op == head_op_)
  {
    return {};
  }
  // Later entries can stay on storage: they link to the erased entry, or to
  // an entry that this slot will not hold again, so recovery ignores them.
  if (const auto erased = EraseSlot(p_op + 1); !erased)
  {
    return erased;
  }
  head_op_ = p_op;
  VSR_ASSERT(!dirty_);
  return {};
}

std::expected<void, StorageError> Journal::Reset(std::uint64_t p_op, std::uint64_t p_checksum)
{
  if (const auto erased = EraseSlot(p_op + 1); !erased)
  {
    return erased;
  }
  floor_op_ = p_op;
  floor_checksum_ = p_checksum;
  head_op_ = p_op;
  VSR_ASSERT(!dirty_);
  return {};
}

std::expected<std::span<const std::byte>, JournalReadError>
Journal::ReadBody(std::uint64_t p_op, std::span<std::byte> p_out)
{
  const auto &header = HeaderAt(p_op);
  VSR_ASSERT(header.body_bytes_ <= p_out.size());
  const auto body = p_out.first(header.body_bytes_);
  if (!storage_->Read(layout_.SlotOffset(p_op) + kEntryHeaderBytes, body))
  {
    return std::unexpected(JournalReadError::Io);
  }
  if (Checksum(body) != header.checksum_body_)
  {
    return std::unexpected(JournalReadError::Corrupt);
  }
  return std::span<const std::byte>{body};
}

std::expected<void, StorageError> Journal::Sync()
{
  if (!dirty_)
  {
    return {};
  }
  if (const auto synced = storage_->Sync(); !synced)
  {
    return synced;
  }
  dirty_ = false;
  return {};
}

} // namespace vsr::internal
