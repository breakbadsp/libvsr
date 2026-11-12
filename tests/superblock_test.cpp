#include "test_support.hpp"
#include "vsr/internal/layout.hpp"
#include "vsr/internal/superblock.hpp"

#include <gtest/gtest.h>

namespace vsr::internal
{

// This checks copy selection by damaging each copy in turn.
TEST(Superblock, SelectsNewestValidCopy)
{
  Config config;
  test::MemoryStorage storage{2 * kSectorBytes};
  auto result = ReadSuperblock(storage);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), SuperblockError::NotFound);
  auto block = MakeSuperblock(config);
  block.view_ = 1;
  ASSERT_TRUE(WriteSuperblock(storage, block));
  block.view_ = 2;
  ASSERT_TRUE(WriteSuperblock(storage, block));
  block.view_ = 3;
  ASSERT_TRUE(WriteSuperblock(storage, block));
  result = ReadSuperblock(storage);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->view_, 3U);
  storage.bytes_[kSectorBytes] ^= std::byte{1};
  result = ReadSuperblock(storage);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->view_, 2U);
  storage.bytes_[0] ^= std::byte{1};
  result = ReadSuperblock(storage);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), SuperblockError::NotFound);
}

// This checks each layout field by changing it in the configuration.
TEST(Superblock, RejectsDifferentLayout)
{
  Config config;
  const auto block = MakeSuperblock(config);
  EXPECT_TRUE(MatchesConfig(block, config));
  auto changed = config;
  ++changed.cluster_;
  EXPECT_FALSE(MatchesConfig(block, changed));
  changed = config;
  ++changed.replica_;
  EXPECT_FALSE(MatchesConfig(block, changed));
  changed = config;
  ++changed.replica_count_;
  EXPECT_FALSE(MatchesConfig(block, changed));
  ++changed.snapshot_bytes_max_;
  EXPECT_FALSE(MatchesConfig(block, changed));
  changed = config;
  ++changed.log_slot_count_;
  EXPECT_FALSE(MatchesConfig(block, changed));
  changed = config;
  ++changed.body_bytes_max_;
  EXPECT_FALSE(MatchesConfig(block, changed));
  changed = config;
  ++changed.clients_max_;
  EXPECT_FALSE(MatchesConfig(block, changed));
  changed = config;
  ++changed.checkpoint_interval_;
  EXPECT_FALSE(MatchesConfig(block, changed));
}

} // namespace vsr::internal
