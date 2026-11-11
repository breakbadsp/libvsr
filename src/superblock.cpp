#include "vsr/internal/superblock.hpp"

#include "vsr/checksum.hpp"
#include "vsr/internal/layout.hpp"
#include "vsr/message.hpp"

#include <array>
#include <cstring>
#include <optional>

namespace vsr::internal
{

namespace
{

constexpr std::uint16_t kStorageVersion = 1;

[[nodiscard]] std::uint64_t SuperblockChecksum(const Superblock &p_superblock) noexcept
{
  const auto bytes = std::as_bytes(std::span<const Superblock, 1>{&p_superblock, 1});
  return Checksum(bytes.subspan(sizeof(std::uint64_t)));
}

} // namespace

Superblock MakeSuperblock(const Config &p_config)
{
  Superblock superblock;
  superblock.cluster_ = p_config.cluster_;
  superblock.checkpoint_entry_checksum_ = RootChecksum(p_config.cluster_);
  superblock.snapshot_bytes_max_ = p_config.snapshot_bytes_max_;
  superblock.log_slot_count_ = p_config.log_slot_count_;
  superblock.body_bytes_max_ = p_config.body_bytes_max_;
  superblock.clients_max_ = p_config.clients_max_;
  superblock.checkpoint_interval_ = p_config.checkpoint_interval_;
  superblock.version_ = kStorageVersion;
  superblock.replica_ = p_config.replica_;
  superblock.replica_count_ = p_config.replica_count_;
  return superblock;
}

bool MatchesConfig(const Superblock &p_superblock, const Config &p_config)
{
  return p_superblock.version_ == kStorageVersion && p_superblock.cluster_ == p_config.cluster_ &&
         p_superblock.replica_ == p_config.replica_ &&
         p_superblock.replica_count_ == p_config.replica_count_ &&
         p_superblock.snapshot_bytes_max_ == p_config.snapshot_bytes_max_ &&
         p_superblock.log_slot_count_ == p_config.log_slot_count_ &&
         p_superblock.body_bytes_max_ == p_config.body_bytes_max_ &&
         p_superblock.clients_max_ == p_config.clients_max_ &&
         p_superblock.checkpoint_interval_ == p_config.checkpoint_interval_;
}

std::expected<Superblock, SuperblockError> ReadSuperblock(Storage &p_storage)
{
  std::optional<Superblock> newest;
  for (std::uint64_t copy_index = 0; copy_index < kSuperblockCopyCount; ++copy_index)
  {
    std::array<std::byte, sizeof(Superblock)> bytes{};
    if (!p_storage.Read(copy_index * kSectorBytes, bytes))
    {
      return std::unexpected(SuperblockError::Io);
    }
    Superblock copy;
    std::memcpy(&copy, bytes.data(), sizeof(Superblock));
    if (copy.checksum_ != SuperblockChecksum(copy))
    {
      continue;
    }
    if (!newest || copy.sequence_ > newest->sequence_)
    {
      newest = copy;
    }
  }
  if (!newest)
  {
    return std::unexpected(SuperblockError::NotFound);
  }
  return *newest;
}

std::expected<void, StorageError> WriteSuperblock(Storage &p_storage, Superblock &p_superblock)
{
  p_superblock.sequence_ += 1;
  p_superblock.checksum_ = SuperblockChecksum(p_superblock);
  // A full sector, so that the device writes the copy in one unit.
  std::array<std::byte, kSectorBytes> sector{};
  std::memcpy(sector.data(), &p_superblock, sizeof(Superblock));
  const std::uint64_t copy_index = p_superblock.sequence_ % kSuperblockCopyCount;
  if (const auto written = p_storage.Write(copy_index * kSectorBytes, sector); !written)
  {
    return written;
  }
  return p_storage.Sync();
}

} // namespace vsr::internal
