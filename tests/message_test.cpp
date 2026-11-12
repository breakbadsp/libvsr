#include "test_support.hpp"
#include "vsr/message.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace vsr
{

namespace
{

using test::Bytes;

[[nodiscard]] std::vector<std::byte> MakeRequest(std::string_view p_body)
{
  Header header;
  header.command_ = Command::Request;
  header.cluster_ = 7;
  header.client_ = 3;
  header.request_ = 9;
  std::vector<std::byte> bytes(kHeaderBytes + p_body.size());
  const std::size_t message_bytes = EncodeMessage(header, Bytes(p_body), {}, bytes);
  bytes.resize(message_bytes);
  return bytes;
}

[[nodiscard]] std::vector<std::byte> MakePrepare(EntryHeader p_entry, std::string_view p_body)
{
  Header header;
  header.command_ = Command::Prepare;
  header.cluster_ = 7;
  std::vector<std::byte> bytes(MessageBytesMax(64));
  const auto entry_bytes = std::as_bytes(std::span<const EntryHeader, 1>{&p_entry, 1});
  const std::size_t message_bytes = EncodeMessage(header, entry_bytes, Bytes(p_body), bytes);
  bytes.resize(message_bytes);
  return bytes;
}

} // namespace

// A message that EncodeMessage wrote decodes to the same fields and body.
TEST(Message, DecodesWhatEncodeWrote)
{
  const auto bytes = MakeRequest("hello");
  const auto message = DecodeMessage(bytes);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->header_.command_, Command::Request);
  EXPECT_EQ(message->header_.cluster_, 7U);
  EXPECT_EQ(message->header_.client_, 3U);
  EXPECT_EQ(message->header_.request_, 9U);
  EXPECT_EQ(test::Text(message->body_), "hello");
}

// A change of one bit, at each byte of the message, makes the decode fail.
TEST(Message, RejectsEachSingleBitChange)
{
  const auto bytes = MakeRequest("hello");
  for (std::size_t index = 0; index < bytes.size(); ++index)
  {
    auto changed = bytes;
    changed[index] ^= std::byte{1};
    EXPECT_FALSE(DecodeMessage(changed).has_value()) << "byte " << index;
  }
}

// A message that is cut short, at each length, makes the decode fail.
TEST(Message, RejectsEachTruncation)
{
  const auto bytes = MakeRequest("hello");
  for (std::size_t length = 0; length < bytes.size(); ++length)
  {
    EXPECT_FALSE(DecodeMessage(std::span{bytes}.first(length)).has_value()) << length;
  }
}

// A Prepare is valid only if its body is one sealed entry of the same cluster.
TEST(Message, PrepareNeedsValidEntry)
{
  EntryHeader entry;
  entry.cluster_ = 7;
  entry.op_ = 1;
  SealEntry(entry, Bytes("body"));
  EXPECT_TRUE(DecodeMessage(MakePrepare(entry, "body")).has_value());

  // The message checksums are valid in each case below. Only the entry is wrong.
  EXPECT_FALSE(DecodeMessage(MakePrepare(entry, "other")).has_value());
  EXPECT_FALSE(DecodeMessage(MakePrepare(entry, "body and more")).has_value());

  EntryHeader other_cluster = entry;
  other_cluster.cluster_ = 8;
  SealEntry(other_cluster, Bytes("body"));
  EXPECT_FALSE(DecodeMessage(MakePrepare(other_cluster, "body")).has_value());

  EntryHeader unsealed = entry;
  unsealed.request_ += 1;
  EXPECT_FALSE(DecodeMessage(MakePrepare(unsealed, "body")).has_value());
}

// The entry checksum covers the parent, so equal checksums mean equal chains.
TEST(Message, EntryChecksumCoversParent)
{
  EntryHeader first;
  first.cluster_ = 7;
  first.op_ = 5;
  first.parent_ = 100;
  SealEntry(first, Bytes("x"));
  EntryHeader second = first;
  second.parent_ = 101;
  SealEntry(second, Bytes("x"));
  EXPECT_NE(first.checksum_, second.checksum_);
}

// A stream transport learns the message size from the header, and refuses a
// size above the limit before it allocates or waits for the body.
TEST(Message, PeekGivesSizeAndEnforcesLimit)
{
  const auto bytes = MakeRequest("hello");
  const auto size = PeekMessageBytes(bytes, 64);
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(*size, bytes.size());

  const auto large = MakeRequest(std::string(1000, 'x'));
  EXPECT_FALSE(PeekMessageBytes(large, 64).has_value());
  EXPECT_FALSE(PeekMessageBytes(std::span{bytes}.first(kHeaderBytes - 1), 64).has_value());
}

} // namespace vsr
