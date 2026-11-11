#include "test_support.hpp"
#include "vsr/internal/client_table.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace vsr::internal
{

namespace
{

using test::Bytes;

void Commit(ClientTable &p_table, std::uint64_t p_client, std::uint64_t p_request,
            std::uint64_t p_op, std::string_view p_reply)
{
  ClientEntry entry;
  entry.client_ = p_client;
  entry.request_ = p_request;
  entry.op_ = p_op;
  p_table.Commit(entry, Bytes(p_reply));
}

} // namespace

// The table gives the latest request and reply of a client.
TEST(ClientTable, KeepsLatestRequestAndReply)
{
  ClientTable table{4, 16};
  EXPECT_EQ(table.Find(5), nullptr);
  Commit(table, 5, 1, 10, "first");
  Commit(table, 5, 2, 11, "second");
  const auto *entry = table.Find(5);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->request_, 2U);
  EXPECT_EQ(test::Text(table.Reply(*entry)), "second");
}

// A new client in a full table replaces the client whose last commit is oldest.
TEST(ClientTable, FullTableRemovesOldestCommit)
{
  ClientTable table{2, 16};
  Commit(table, 7, 1, 20, "seven");
  Commit(table, 3, 1, 10, "three");
  Commit(table, 9, 1, 30, "nine");
  EXPECT_EQ(table.Find(3), nullptr);
  ASSERT_NE(table.Find(7), nullptr);
  ASSERT_NE(table.Find(9), nullptr);
  EXPECT_EQ(test::Text(table.Reply(*table.Find(7))), "seven");
  EXPECT_EQ(test::Text(table.Reply(*table.Find(9))), "nine");
}

// Two tables with the same content have the same bytes, also when the
// commits came in a different order. A copy from those bytes is equal.
TEST(ClientTable, SerializedFormDoesNotDependOnInsertOrder)
{
  ClientTable forward{4, 16};
  Commit(forward, 1, 4, 40, "one");
  Commit(forward, 2, 5, 41, "");
  Commit(forward, 3, 6, 42, "three");
  ClientTable backward{4, 16};
  Commit(backward, 3, 6, 42, "three");
  Commit(backward, 2, 5, 41, "");
  Commit(backward, 1, 4, 40, "one");

  std::vector<std::byte> forward_bytes(256);
  std::vector<std::byte> backward_bytes(256);
  forward_bytes.resize(forward.Serialize(forward_bytes));
  backward_bytes.resize(backward.Serialize(backward_bytes));
  EXPECT_EQ(forward_bytes, backward_bytes);

  ClientTable copy{4, 16};
  const auto consumed = copy.Deserialize(forward_bytes);
  ASSERT_TRUE(consumed.has_value());
  EXPECT_EQ(*consumed, forward_bytes.size());
  ASSERT_NE(copy.Find(3), nullptr);
  EXPECT_EQ(copy.Find(3)->request_, 6U);
  EXPECT_EQ(copy.Find(3)->op_, 42U);
  EXPECT_EQ(test::Text(copy.Reply(*copy.Find(3))), "three");
  EXPECT_EQ(test::Text(copy.Reply(*copy.Find(2))), "");
}

// Input that is cut short, or that has more clients than the limit, is refused.
TEST(ClientTable, RefusesInvalidInput)
{
  ClientTable table{4, 16};
  Commit(table, 1, 4, 40, "one");
  Commit(table, 2, 5, 41, "two");
  std::vector<std::byte> bytes(256);
  bytes.resize(table.Serialize(bytes));

  ClientTable copy{4, 16};
  for (std::size_t length = 0; length < bytes.size(); ++length)
  {
    EXPECT_FALSE(copy.Deserialize(std::span{bytes}.first(length)).has_value()) << length;
  }
  ClientTable small{1, 16};
  EXPECT_FALSE(small.Deserialize(bytes).has_value());
}

} // namespace vsr::internal
