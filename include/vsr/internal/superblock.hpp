#pragma once

#include "vsr/config.hpp"
#include "vsr/interfaces.hpp"

#include <cstdint>
#include <expected>
#include <type_traits>

namespace vsr::internal
{

// The durable protocol state of one replica. It has two copies on disk. A
// write replaces the older copy, so a torn write leaves the newer one.
struct Superblock
{
  std::uint64_t checksum_ = 0;
  std::uint64_t cluster_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t checkpoint_op_ = 0;
  // Checksum of the log entry at `checkpoint_op_`.
  std::uint64_t checkpoint_entry_checksum_ = 0;
  // Checksum and size of the snapshot blob. Size 0 means the initial state.
  std::uint64_t checkpoint_checksum_ = 0;
  std::uint64_t checkpoint_bytes_ = 0;
  std::uint64_t snapshot_bytes_max_ = 0;
  std::uint32_t view_ = 0;
  // The last view in which the log was a prefix of the log of the primary.
  std::uint32_t log_view_ = 0;
  std::uint32_t log_slot_count_ = 0;
  std::uint32_t body_bytes_max_ = 0;
  std::uint32_t clients_max_ = 0;
  std::uint32_t checkpoint_interval_ = 0;
  std::uint16_t version_ = 0;
  std::uint8_t replica_ = 0;
  std::uint8_t replica_count_ = 0;
  std::uint8_t checkpoint_slot_ = 0;
  std::uint8_t reserved_[3] = {};
};

static_assert(sizeof(Superblock) == 96, "the storage format is fixed");
static_assert(std::has_unique_object_representations_v<Superblock>);

enum class SuperblockError
{
  Io = 0,
  // No copy has a valid checksum: the storage is not formatted.
  NotFound,
};

[[nodiscard]] Superblock MakeSuperblock(const Config &p_config);

// True if the storage was formatted with the layout and identity in `p_config`.
[[nodiscard]] bool MatchesConfig(const Superblock &p_superblock, const Config &p_config);

[[nodiscard]] std::expected<Superblock, SuperblockError> ReadSuperblock(Storage &p_storage);

// Increases the sequence, writes the older copy, and syncs.
[[nodiscard]] std::expected<void, StorageError> WriteSuperblock(Storage &p_storage,
                                                                Superblock &p_superblock);

} // namespace vsr::internal
