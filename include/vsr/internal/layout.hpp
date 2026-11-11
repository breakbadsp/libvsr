#pragma once

#include "vsr/config.hpp"
#include "vsr/message.hpp"

#include <cstdint>

namespace vsr::internal
{

inline constexpr std::uint64_t kSectorBytes = 4096;
inline constexpr std::uint64_t kSuperblockCopyCount = 2;
inline constexpr std::uint64_t kSnapshotSlotCount = 2;

[[nodiscard]] constexpr std::uint64_t RoundUpToSector(std::uint64_t p_bytes) noexcept
{
  return (p_bytes + kSectorBytes - 1) / kSectorBytes * kSectorBytes;
}

// Bytes of one client table entry without its reply.
inline constexpr std::uint64_t kClientEntryBytes = 36;

[[nodiscard]] constexpr std::uint64_t ClientTableBytesMax(const Config &p_config) noexcept
{
  return sizeof(std::uint32_t) +
         std::uint64_t{p_config.clients_max_} * (kClientEntryBytes + p_config.body_bytes_max_);
}

// A snapshot blob is: timestamp, client table, state machine snapshot.
[[nodiscard]] constexpr std::uint64_t SnapshotBlobBytesMax(const Config &p_config) noexcept
{
  return sizeof(std::uint64_t) + ClientTableBytesMax(p_config) + p_config.snapshot_bytes_max_;
}

// The storage file: two superblock copies, the log ring, two snapshot slots.
struct Layout
{
  std::uint64_t slot_bytes_ = 0;
  std::uint64_t slot_count_ = 0;
  std::uint64_t log_offset_ = 0;
  std::uint64_t snapshot_slot_bytes_ = 0;
  std::uint64_t snapshot_offset_ = 0;
  std::uint64_t total_bytes_ = 0;

  [[nodiscard]] constexpr std::uint64_t SlotOffset(std::uint64_t p_op) const noexcept
  {
    return log_offset_ + (p_op % slot_count_) * slot_bytes_;
  }

  [[nodiscard]] constexpr std::uint64_t SnapshotOffset(std::uint64_t p_slot_index) const noexcept
  {
    return snapshot_offset_ + p_slot_index * snapshot_slot_bytes_;
  }
};

[[nodiscard]] constexpr Layout MakeLayout(const Config &p_config) noexcept
{
  Layout layout;
  layout.slot_bytes_ = RoundUpToSector(kEntryHeaderBytes + p_config.body_bytes_max_);
  layout.slot_count_ = p_config.log_slot_count_;
  layout.log_offset_ = kSuperblockCopyCount * kSectorBytes;
  layout.snapshot_slot_bytes_ = RoundUpToSector(SnapshotBlobBytesMax(p_config));
  layout.snapshot_offset_ = layout.log_offset_ + layout.slot_count_ * layout.slot_bytes_;
  layout.total_bytes_ = layout.snapshot_offset_ + kSnapshotSlotCount * layout.snapshot_slot_bytes_;
  return layout;
}

} // namespace vsr::internal
