#include "test_support.hpp"
#include "vsr/internal/journal.hpp"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>

namespace vsr::internal
{
namespace
{
void Append(Journal &p_journal, std::string_view p_body)
{
  EntryHeader header;
  header.op_ = p_journal.Head() + 1;
  header.parent_ = p_journal.HeadChecksum();
  header.body_bytes_ = static_cast<std::uint32_t>(p_body.size());
  SealEntry(header, test::Bytes(p_body));
  ASSERT_TRUE(p_journal.Append(header, test::Bytes(p_body)));
}
} // namespace

// This checks recovery and body reads after a fresh journal opens storage.
TEST(Journal, RecoversEntries)
{
  Config config;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  const auto root = RootChecksum(config.cluster_);
  ASSERT_TRUE(journal.Recover(0, root));
  Append(journal, "one");
  Append(journal, "two");
  Journal recovered{&storage, layout};
  ASSERT_TRUE(recovered.Recover(0, root));
  EXPECT_EQ(recovered.Head(), 2U);
  EXPECT_EQ(recovered.Floor(), 0U);
  EXPECT_EQ(recovered.HeadChecksum(), journal.HeadChecksum());
  EXPECT_EQ(recovered.ChecksumAt(0), root);
  EXPECT_EQ(recovered.ChecksumAt(1), journal.ChecksumAt(1));
  std::array<std::byte, 16> first_body{};
  const auto first = recovered.ReadBody(1, first_body);
  ASSERT_TRUE(first);
  EXPECT_EQ(test::Text(*first), "one");
  std::array<std::byte, 16> body{};
  const auto read = recovered.ReadBody(2, body);
  ASSERT_TRUE(read);
  EXPECT_EQ(test::Text(*read), "two");
}

// This checks truncation by reopening storage after replacing an entry.
TEST(Journal, TruncateStopsOldChain)
{
  Config config;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  const auto root = RootChecksum(config.cluster_);
  ASSERT_TRUE(journal.Recover(0, root));
  Append(journal, "one");
  Append(journal, "two");
  Append(journal, "three");
  ASSERT_TRUE(journal.TruncateTo(1));
  Journal recovered{&storage, layout};
  ASSERT_TRUE(recovered.Recover(0, root));
  EXPECT_EQ(recovered.Head(), 1U);
  Append(recovered, "different");
  Journal again{&storage, layout};
  ASSERT_TRUE(again.Recover(0, root));
  EXPECT_EQ(again.Head(), 2U);
}
} // namespace vsr::internal

namespace vsr::internal
{
// This checks damaged headers and bodies by reopening the storage.
TEST(Journal, RecoveryStopsAtDamage)
{
  for (const auto offset : {std::uint64_t{0}, std::uint64_t{kEntryHeaderBytes}})
  {
    Config config;
    const auto layout = MakeLayout(config);
    test::MemoryStorage storage{layout.total_bytes_};
    Journal journal{&storage, layout};
    const auto root = RootChecksum(config.cluster_);
    ASSERT_TRUE(journal.Recover(0, root));
    Append(journal, "one");
    Append(journal, "two");
    storage.bytes_[layout.SlotOffset(2) + offset] ^= std::byte{1};
    Journal recovered{&storage, layout};
    ASSERT_TRUE(recovered.Recover(0, root));
    EXPECT_EQ(recovered.Head(), 1U);
  }
}

// This checks that truncation removes each later entry. The journal gets
// the entry after the new head again, and the old entry after that one must
// not be a part of the log when the storage opens again.
TEST(Journal, TruncateErasesLaterEntries)
{
  Config config;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  const auto root = RootChecksum(config.cluster_);
  ASSERT_TRUE(journal.Recover(0, root));
  Append(journal, "one");
  Append(journal, "two");
  Append(journal, "three");
  ASSERT_TRUE(journal.TruncateTo(1));
  Append(journal, "two");
  ASSERT_TRUE(journal.Sync());
  Journal recovered{&storage, layout};
  ASSERT_TRUE(recovered.Recover(0, root));
  EXPECT_EQ(recovered.Head(), 2U);
}

// This checks recovery after a power loss that lost one entry and kept the
// entry after it. The journal gets the lost entry again, and the old entry
// after it must not be a part of the log when the storage opens again.
TEST(Journal, RecoveryErasesEntryAfterLostEntry)
{
  Config config;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  const auto root = RootChecksum(config.cluster_);
  ASSERT_TRUE(journal.Recover(0, root));
  Append(journal, "one");
  Append(journal, "two");
  Append(journal, "three");
  std::fill_n(storage.bytes_.begin() + static_cast<std::ptrdiff_t>(layout.SlotOffset(2)),
              kEntryHeaderBytes, std::byte{0});
  Journal recovered{&storage, layout};
  ASSERT_TRUE(recovered.Recover(0, root));
  ASSERT_EQ(recovered.Head(), 1U);
  Append(recovered, "two");
  ASSERT_TRUE(recovered.Sync());
  Journal again{&storage, layout};
  ASSERT_TRUE(again.Recover(0, root));
  EXPECT_EQ(again.Head(), 2U);
}

// This checks recovery when the head is the checkpoint and an old entry one
// full lap after it sits in the slot of the checkpoint. A power loss lost the
// entries between them. The journal gets those entries again, and the old
// entry must not be a part of the log when the storage opens again.
TEST(Journal, RecoveryErasesEntryOneLapAfterCheckpoint)
{
  Config config;
  config.log_slot_count_ = 8;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  const auto root = RootChecksum(config.cluster_);
  ASSERT_TRUE(journal.Recover(0, root));
  constexpr std::uint64_t checkpoint = 4;
  constexpr std::uint64_t stale = checkpoint + 8;
  std::array<std::uint64_t, stale + 1> checksums{};
  for (std::uint64_t op = 1; op <= stale; ++op)
  {
    Append(journal, "body");
    checksums[op] = journal.HeadChecksum();
  }
  ASSERT_TRUE(journal.Sync());
  for (std::uint64_t op = checkpoint + 1; op < stale; ++op)
  {
    std::fill_n(storage.bytes_.begin() + static_cast<std::ptrdiff_t>(layout.SlotOffset(op)),
                kEntryHeaderBytes, std::byte{0});
  }
  Journal recovered{&storage, layout};
  ASSERT_TRUE(recovered.Recover(checkpoint, checksums[checkpoint]));
  ASSERT_EQ(recovered.Head(), checkpoint);
  for (std::uint64_t op = checkpoint + 1; op < stale; ++op)
  {
    Append(recovered, "body");
  }
  ASSERT_TRUE(recovered.Sync());
  Journal again{&storage, layout};
  ASSERT_TRUE(again.Recover(checkpoint, checksums[checkpoint]));
  EXPECT_EQ(again.Head(), stale - 1);
}

// This checks ring recovery on both sides of a checkpoint after multiple laps.
TEST(Journal, WrapRecoversAroundCheckpoint)
{
  Config config;
  config.log_slot_count_ = 8;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  const auto root = RootChecksum(config.cluster_);
  ASSERT_TRUE(journal.Recover(0, root));
  std::array<std::uint64_t, 21> checksums{};
  checksums[0] = root;
  for (std::size_t op = 1; op <= 20; ++op)
  {
    Append(journal, "body");
    checksums[op] = journal.HeadChecksum();
  }
  EXPECT_EQ(journal.Floor(), 12U);
  EXPECT_EQ(journal.ChecksumAt(12), checksums[12]);
  Journal recovered{&storage, layout};
  ASSERT_TRUE(recovered.Recover(16, checksums[16]));
  EXPECT_EQ(recovered.Floor(), 12U);
  EXPECT_EQ(recovered.Head(), 20U);
  for (std::uint64_t op = 13; op <= 20; ++op)
  {
    EXPECT_EQ(recovered.ChecksumAt(op), checksums[op]);
    std::array<std::byte, 16> body{};
    const auto read = recovered.ReadBody(op, body);
    ASSERT_TRUE(read);
    EXPECT_EQ(test::Text(*read), "body");
  }
}

// This checks that reset removes a valid next entry before recovery.
TEST(Journal, ResetErasesNextEntry)
{
  Config config;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  const auto root = RootChecksum(config.cluster_);
  ASSERT_TRUE(journal.Recover(0, root));
  Append(journal, "one");
  ASSERT_TRUE(journal.Reset(0, root));
  Journal recovered{&storage, layout};
  ASSERT_TRUE(recovered.Recover(0, root));
  EXPECT_EQ(recovered.Head(), 0U);
  EXPECT_EQ(recovered.Floor(), 0U);
}

// This checks that a changed body produces a corruption error on read.
TEST(Journal, ReadBodyDetectsDamage)
{
  Config config;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  ASSERT_TRUE(journal.Recover(0, RootChecksum(config.cluster_)));
  Append(journal, "one");
  storage.bytes_[layout.SlotOffset(1) + kEntryHeaderBytes] ^= std::byte{1};
  std::array<std::byte, 16> body{};
  const auto read = journal.ReadBody(1, body);
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error(), JournalReadError::Corrupt);
}

// This checks recovery from a checkpoint that has no stored entry.
TEST(Journal, MissingCheckpoint)
{
  Config config;
  const auto layout = MakeLayout(config);
  test::MemoryStorage storage{layout.total_bytes_};
  Journal journal{&storage, layout};
  ASSERT_TRUE(journal.Recover(100, 123));
  EXPECT_EQ(journal.Head(), 100U);
  EXPECT_EQ(journal.Floor(), 100U);
  EXPECT_EQ(journal.HeadChecksum(), 123U);
}
} // namespace vsr::internal
