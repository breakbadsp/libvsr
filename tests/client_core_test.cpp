#include "test_support.hpp"
#include "vsr/client_core.hpp"
#include "vsr/message.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The client protocol alone: no socket, and a clock that the test moves.

namespace vsr
{

namespace
{

using test::Bytes;
using test::Text;

constexpr std::uint64_t kCluster = 77;
constexpr std::uint8_t kReplicaCount = 3;
constexpr std::uint64_t kRetryInterval = 10;
constexpr std::uint32_t kBodyBytesMax = 64;

[[nodiscard]] ClientCore MakeCore(std::uint64_t p_seed = 1, std::uint64_t p_client = 0)
{
  ClientCoreOptions options;
  options.cluster_ = kCluster;
  options.client_ = p_client;
  options.seed_ = p_seed;
  options.replica_count_ = kReplicaCount;
  options.body_bytes_max_ = kBodyBytesMax;
  options.retry_interval_ = kRetryInterval;
  return ClientCore{options};
}

[[nodiscard]] std::vector<std::byte> Encode(const Header &p_header, std::string_view p_body = {})
{
  std::vector<std::byte> bytes(kHeaderBytes + p_body.size());
  bytes.resize(EncodeMessage(p_header, Bytes(p_body), {}, bytes));
  return bytes;
}

struct ReplyFields
{
  std::uint64_t client_ = 0;
  std::uint64_t request_ = 0;
  std::uint64_t session_ = 0;
  std::uint32_t view_ = 0;
};

[[nodiscard]] std::vector<std::byte> MakeReply(ReplyFields p_fields, std::string_view p_body = {})
{
  Header header;
  header.command_ = Command::Reply;
  header.cluster_ = kCluster;
  header.client_ = p_fields.client_;
  header.request_ = p_fields.request_;
  header.session_ = p_fields.session_;
  header.view_ = p_fields.view_;
  return Encode(header, p_body);
}

[[nodiscard]] std::vector<std::byte> MakeEviction(std::uint64_t p_client, std::uint32_t p_view)
{
  Header header;
  header.command_ = Command::Eviction;
  header.cluster_ = kCluster;
  header.client_ = p_client;
  header.view_ = p_view;
  return Encode(header);
}

// The message that the core wants to send, decoded.
struct Sent
{
  Header header_;
  std::string body_;
  std::uint8_t replica_begin_ = 0;
  std::uint8_t replica_end_ = 0;

  [[nodiscard]] bool IsNothing() const noexcept { return replica_begin_ == replica_end_; }
  [[nodiscard]] bool IsTo(std::uint8_t p_replica) const noexcept
  {
    return replica_begin_ == p_replica && replica_end_ == p_replica + 1;
  }
  [[nodiscard]] bool IsToEveryReplica() const noexcept
  {
    return replica_begin_ == 0 && replica_end_ == kReplicaCount;
  }
};

[[nodiscard]] Sent Poll(ClientCore &p_core, std::uint64_t p_now)
{
  const ClientSend send = p_core.Poll(p_now);
  Sent sent;
  sent.replica_begin_ = send.replica_begin_;
  sent.replica_end_ = send.replica_end_;
  if (sent.IsNothing())
  {
    return sent;
  }
  const auto message = DecodeMessage(send.message_);
  EXPECT_TRUE(message.has_value());
  if (message.has_value())
  {
    sent.header_ = message->header_;
    sent.body_ = Text(message->body_);
  }
  return sent;
}

[[nodiscard]] ClientEventKind Give(ClientCore &p_core, const std::vector<std::byte> &p_bytes)
{
  return p_core.OnMessage(p_bytes).kind_;
}

constexpr std::uint64_t kSession = 5;

// Starts a request at time 0 and completes its registration with session
// `kSession`. The operation is then the message in progress, not yet sent.
void StartRegistered(ClientCore &p_core, std::string_view p_body, std::uint32_t p_view = 0)
{
  ASSERT_TRUE(p_core.Request(Bytes(p_body), kClientNever).has_value());
  ASSERT_FALSE(Poll(p_core, 0).IsNothing());
  ASSERT_EQ(Give(p_core, MakeReply({p_core.Id(), 0, kSession, p_view})), ClientEventKind::None);
}

// Sends the next request at `p_now` and completes it.
void Complete(ClientCore &p_core, std::string_view p_body, std::uint64_t p_now,
              std::uint32_t p_view = 0)
{
  if (!p_core.IsPending())
  {
    ASSERT_TRUE(p_core.Request(Bytes(p_body), kClientNever).has_value());
  }
  const Sent sent = Poll(p_core, p_now);
  ASSERT_FALSE(sent.IsNothing());
  const ClientEvent event =
      p_core.OnMessage(MakeReply({p_core.Id(), sent.header_.request_, kSession, p_view}, "reply"));
  ASSERT_EQ(event.kind_, ClientEventKind::Reply);
}

TEST(ClientCore, RegistersBeforeTheFirstRequest)
{
  ClientCore core = MakeCore();
  EXPECT_NE(core.Id(), 0U);
  ASSERT_TRUE(core.Request(Bytes("first"), kClientNever).has_value());
  EXPECT_TRUE(core.IsPending());

  const Sent registration = Poll(core, 0);
  EXPECT_TRUE(registration.IsTo(0));
  EXPECT_EQ(registration.header_.command_, Command::Request);
  EXPECT_EQ(registration.header_.cluster_, kCluster);
  EXPECT_EQ(registration.header_.client_, core.Id());
  EXPECT_EQ(registration.header_.request_, 0U);
  EXPECT_EQ(registration.header_.session_, 0U);
  EXPECT_EQ(registration.body_, "");

  // A reply to an operation does not answer the registration.
  EXPECT_EQ(Give(core, MakeReply({core.Id(), 1, kSession, 0}, "x")), ClientEventKind::None);
  EXPECT_TRUE(Poll(core, 0).IsNothing());

  // The registration reply does not complete the request. The operation
  // goes out at once, with the session of the reply.
  EXPECT_EQ(Give(core, MakeReply({core.Id(), 0, kSession, 0})), ClientEventKind::None);
  EXPECT_TRUE(core.IsPending());
  const Sent operation = Poll(core, 0);
  EXPECT_TRUE(operation.IsTo(0));
  EXPECT_EQ(operation.header_.client_, core.Id());
  EXPECT_EQ(operation.header_.request_, 1U);
  EXPECT_EQ(operation.header_.session_, kSession);
  EXPECT_EQ(operation.body_, "first");
}

TEST(ClientCore, CompletesARequestWithItsReply)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  ASSERT_TRUE(Poll(core, 0).IsTo(0));

  const std::vector<std::byte> reply = MakeReply({core.Id(), 1, kSession, 0}, "one");
  const ClientEvent event = core.OnMessage(reply);
  EXPECT_EQ(event.kind_, ClientEventKind::Reply);
  EXPECT_EQ(Text(event.reply_), "one");
  EXPECT_FALSE(core.IsPending());
  EXPECT_FALSE(core.IsWaiting());
  EXPECT_EQ(core.WakeTime(), kClientNever);
  EXPECT_TRUE(Poll(core, 100).IsNothing());

  // The session continues: no second registration, and the next number.
  ASSERT_TRUE(core.Request(Bytes("second"), kClientNever).has_value());
  const Sent second = Poll(core, 100);
  EXPECT_TRUE(second.IsTo(0));
  EXPECT_EQ(second.header_.request_, 2U);
  EXPECT_EQ(second.header_.session_, kSession);
  EXPECT_EQ(second.body_, "second");
}

TEST(ClientCore, RefusesRequestsThatItCannotStart)
{
  ClientCore core = MakeCore();
  EXPECT_EQ(core.Resume(kClientNever).error(), ClientCoreError::NotPending);
  const std::string large(kBodyBytesMax + 1, 'x');
  EXPECT_EQ(core.Request(Bytes(large), kClientNever).error(), ClientCoreError::TooLarge);
  EXPECT_FALSE(core.IsPending());

  const std::string largest(kBodyBytesMax, 'x');
  ASSERT_TRUE(core.Request(Bytes(largest), kClientNever).has_value());
  EXPECT_EQ(core.Request(Bytes("other"), kClientNever).error(), ClientCoreError::Pending);
  // The refused request did not replace the operation in progress.
  ASSERT_FALSE(Poll(core, 0).IsNothing());
  ASSERT_EQ(Give(core, MakeReply({core.Id(), 0, kSession, 0})), ClientEventKind::None);
  EXPECT_EQ(Poll(core, 0).body_, largest);
}

TEST(ClientCore, SendsAgainToEveryReplicaAfterTheRetryInterval)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  constexpr std::uint64_t kStart = 100;
  const Sent first = Poll(core, kStart);
  EXPECT_TRUE(first.IsTo(0));

  EXPECT_TRUE(Poll(core, kStart + kRetryInterval - 1).IsNothing());
  EXPECT_EQ(core.WakeTime(), kStart + kRetryInterval);

  // The supposed primary did not answer. A different replica can be the
  // primary now, so each replica gets the same request.
  const Sent second = Poll(core, kStart + kRetryInterval);
  EXPECT_TRUE(second.IsToEveryReplica());
  EXPECT_EQ(second.header_.checksum_, first.header_.checksum_);
  EXPECT_EQ(second.header_.request_, 1U);

  EXPECT_TRUE(Poll(core, kStart + 2 * kRetryInterval - 1).IsNothing());
  EXPECT_TRUE(Poll(core, kStart + 2 * kRetryInterval).IsToEveryReplica());

  // The reply can come from each replica. The next request goes to one
  // replica again.
  ASSERT_EQ(Give(core, MakeReply({core.Id(), 1, kSession, 0})), ClientEventKind::Reply);
  ASSERT_TRUE(core.Request(Bytes("second"), kClientNever).has_value());
  EXPECT_TRUE(Poll(core, kStart + 2 * kRetryInterval).IsTo(0));
}

TEST(ClientCore, SendsTheRegistrationAgainToEveryReplica)
{
  ClientCore core = MakeCore();
  ASSERT_TRUE(core.Request(Bytes("first"), kClientNever).has_value());
  EXPECT_TRUE(Poll(core, 0).IsTo(0));
  const Sent again = Poll(core, kRetryInterval);
  EXPECT_TRUE(again.IsToEveryReplica());
  EXPECT_EQ(again.header_.request_, 0U);
}

TEST(ClientCore, TimeoutKeepsTheRequestInProgress)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  ASSERT_TRUE(core.Expire(kClientNever)); // end the wait that has no deadline
  constexpr std::uint64_t kDeadline = 3;
  ASSERT_TRUE(core.Resume(kDeadline).has_value());
  EXPECT_TRUE(Poll(core, 0).IsTo(0));
  EXPECT_EQ(core.WakeTime(), kDeadline);

  EXPECT_FALSE(core.Expire(kDeadline - 1));
  EXPECT_TRUE(core.Expire(kDeadline));
  EXPECT_FALSE(core.Expire(kDeadline)); // one decision for each wait
  EXPECT_TRUE(core.IsPending());
  EXPECT_FALSE(core.IsWaiting());
  EXPECT_EQ(core.WakeTime(), kClientNever);
  EXPECT_EQ(core.Request(Bytes("other"), kClientNever).error(), ClientCoreError::Pending);

  // Between two waits the core is silent, and a reply stays with the cluster.
  EXPECT_TRUE(Poll(core, kRetryInterval).IsNothing());
  const std::vector<std::byte> reply = MakeReply({core.Id(), 1, kSession, 0}, "one");
  EXPECT_EQ(Give(core, reply), ClientEventKind::None);
  EXPECT_TRUE(core.IsPending());

  // The retry state belongs to the message: waits shorter than the retry
  // interval also reach every replica.
  ASSERT_TRUE(core.Resume(kRetryInterval + 2).has_value());
  const Sent again = Poll(core, kRetryInterval);
  EXPECT_TRUE(again.IsToEveryReplica());
  EXPECT_EQ(again.header_.request_, 1U);
  EXPECT_EQ(again.body_, "first");
  const ClientEvent event = core.OnMessage(reply);
  EXPECT_EQ(event.kind_, ClientEventKind::Reply);
  EXPECT_EQ(Text(event.reply_), "one");
}

TEST(ClientCore, DeadlineOfNowStillSendsOnce)
{
  ClientCore core = MakeCore();
  ASSERT_TRUE(core.Request(Bytes("first"), 50).has_value());
  EXPECT_TRUE(Poll(core, 50).IsTo(0));
  EXPECT_TRUE(core.Expire(50));
}

TEST(ClientCore, RetryTimeDoesNotOverflow)
{
  ClientCore core = MakeCore();
  ASSERT_TRUE(core.Request(Bytes("first"), kClientNever).has_value());
  EXPECT_TRUE(Poll(core, kClientNever - 1).IsTo(0));
  EXPECT_EQ(core.WakeTime(), kClientNever);
}

TEST(ClientCore, IgnoresMessagesThatDoNotAnswerTheRequest)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  Complete(core, "first", 0);
  ASSERT_TRUE(core.Request(Bytes("second"), kClientNever).has_value());
  ASSERT_EQ(Poll(core, 0).header_.request_, 2U);
  const std::uint64_t id = core.Id();

  EXPECT_EQ(Give(core, MakeReply({id, 1, kSession, 0}, "old")), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeReply({id, 3, kSession, 0}, "new")), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeReply({id, 0, kSession, 0})), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeReply({id, 2, kSession + 1, 0}, "x")), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeReply({id, 2, 0, 0}, "x")), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeReply({id + 1, 2, kSession, 0}, "x")), ClientEventKind::None);

  Header other_cluster;
  other_cluster.command_ = Command::Reply;
  other_cluster.cluster_ = kCluster + 1;
  other_cluster.client_ = id;
  other_cluster.request_ = 2;
  other_cluster.session_ = kSession;
  EXPECT_EQ(Give(core, Encode(other_cluster, "x")), ClientEventKind::None);

  // A message of a replica, with the fields of the right reply.
  Header not_reply = other_cluster;
  not_reply.cluster_ = kCluster;
  not_reply.command_ = Command::Commit;
  EXPECT_EQ(Give(core, Encode(not_reply)), ClientEventKind::None);

  std::vector<std::byte> damaged = MakeReply({id, 2, kSession, 0}, "two");
  damaged.back() ^= std::byte{1};
  EXPECT_EQ(Give(core, damaged), ClientEventKind::None);
  EXPECT_EQ(Give(core, {}), ClientEventKind::None);
  const std::vector<std::byte> right = MakeReply({id, 2, kSession, 0}, "two");
  const std::vector<std::byte> header_only{right.begin(),
                                           right.begin() + std::ptrdiff_t{kHeaderBytes}};
  EXPECT_EQ(Give(core, header_only), ClientEventKind::None);

  EXPECT_TRUE(core.IsPending());
  const ClientEvent event = core.OnMessage(right);
  EXPECT_EQ(event.kind_, ClientEventKind::Reply);
  EXPECT_EQ(Text(event.reply_), "two");
}

TEST(ClientCore, CompletesARequestOnceWhenTheReplyRepeats)
{
  ClientCore core = MakeCore();
  ASSERT_TRUE(core.Request(Bytes("first"), kClientNever).has_value());
  ASSERT_TRUE(Poll(core, 0).IsTo(0));
  const std::vector<std::byte> registration = MakeReply({core.Id(), 0, kSession, 0});
  ASSERT_EQ(Give(core, registration), ClientEventKind::None);
  // A second registration reply must not start the operation again, or
  // complete it.
  EXPECT_EQ(Give(core, registration), ClientEventKind::None);
  ASSERT_TRUE(Poll(core, 0).IsTo(0));
  EXPECT_TRUE(Poll(core, 0).IsNothing());

  const std::vector<std::byte> first = MakeReply({core.Id(), 1, kSession, 0}, "one");
  EXPECT_EQ(Give(core, first), ClientEventKind::Reply);
  EXPECT_EQ(Give(core, first), ClientEventKind::None);

  // The repeated reply must not complete the next request.
  ASSERT_TRUE(core.Request(Bytes("second"), kClientNever).has_value());
  ASSERT_EQ(Poll(core, 0).header_.request_, 2U);
  EXPECT_EQ(Give(core, first), ClientEventKind::None);
  EXPECT_TRUE(core.IsPending());
  EXPECT_EQ(Give(core, MakeReply({core.Id(), 2, kSession, 0}, "two")), ClientEventKind::Reply);
}

TEST(ClientCore, ViewOfAReplyMovesTheTargetToTheNewPrimary)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  EXPECT_EQ(core.View(), 0U);
  Complete(core, "first", 0, 4);
  EXPECT_EQ(core.View(), 4U);

  ASSERT_TRUE(core.Request(Bytes("second"), kClientNever).has_value());
  EXPECT_TRUE(Poll(core, 0).IsTo(4 % kReplicaCount));

  // A reply from an older view is still the reply: the operation is
  // committed. It must not move the target back to an old primary.
  EXPECT_EQ(Give(core, MakeReply({core.Id(), 2, kSession, 2}, "two")), ClientEventKind::Reply);
  EXPECT_EQ(core.View(), 4U);
  ASSERT_TRUE(core.Request(Bytes("third"), kClientNever).has_value());
  EXPECT_TRUE(Poll(core, 0).IsTo(4 % kReplicaCount));
}

TEST(ClientCore, ViewOfTheRegistrationReplyMovesTheTarget)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first", 2);
  EXPECT_EQ(core.View(), 2U);
  EXPECT_TRUE(Poll(core, 0).IsTo(2));
}

TEST(ClientCore, MessagesThatItIgnoresDoNotMoveTheTarget)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  ASSERT_TRUE(Poll(core, 0).IsTo(0));
  ASSERT_EQ(Give(core, MakeReply({core.Id(), 9, kSession, 7}, "x")), ClientEventKind::None);
  EXPECT_EQ(core.View(), 0U);
}

TEST(ClientCore, EvictionStartsANewSession)
{
  constexpr std::uint32_t kView = 4;
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  Complete(core, "first", 0, kView);
  ASSERT_TRUE(core.Request(Bytes("second"), kClientNever).has_value());
  ASSERT_EQ(Poll(core, 0).header_.request_, 2U);
  const std::uint64_t old_id = core.Id();

  const ClientEvent event = core.OnMessage(MakeEviction(old_id, kView));
  EXPECT_EQ(event.kind_, ClientEventKind::Evicted);
  EXPECT_TRUE(event.reply_.empty());
  EXPECT_FALSE(core.IsPending());
  EXPECT_FALSE(core.IsWaiting());
  EXPECT_EQ(core.Resume(kClientNever).error(), ClientCoreError::NotPending);
  EXPECT_TRUE(Poll(core, kRetryInterval).IsNothing());
  const std::uint64_t new_id = core.Id();
  EXPECT_NE(new_id, old_id);
  EXPECT_NE(new_id, 0U);

  // The next request registers the new identifier. The view is still known.
  ASSERT_TRUE(core.Request(Bytes("third"), kClientNever).has_value());
  const Sent registration = Poll(core, kRetryInterval);
  EXPECT_TRUE(registration.IsTo(kView % kReplicaCount));
  EXPECT_EQ(registration.header_.client_, new_id);
  EXPECT_EQ(registration.header_.request_, 0U);
  EXPECT_EQ(registration.header_.session_, 0U);

  // Messages for the old identifier have no effect.
  EXPECT_EQ(Give(core, MakeReply({old_id, 0, kSession, kView})), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeEviction(old_id, 9)), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeReply({old_id, 2, kSession, kView}, "two")), ClientEventKind::None);
  EXPECT_EQ(core.Id(), new_id);

  constexpr std::uint64_t kNewSession = 40;
  ASSERT_EQ(Give(core, MakeReply({new_id, 0, kNewSession, kView})), ClientEventKind::None);
  const Sent operation = Poll(core, kRetryInterval);
  EXPECT_EQ(operation.header_.client_, new_id);
  EXPECT_EQ(operation.header_.request_, 1U);
  EXPECT_EQ(operation.header_.session_, kNewSession);
  EXPECT_EQ(operation.body_, "third");
  // A reply with the session number of the old session is not the reply.
  EXPECT_EQ(Give(core, MakeReply({new_id, 1, kSession, kView}, "x")), ClientEventKind::None);
  EXPECT_EQ(Give(core, MakeReply({new_id, 1, kNewSession, kView}, "y")), ClientEventKind::Reply);
}

TEST(ClientCore, IgnoresEvictionFromAnOlderView)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first", 3);
  ASSERT_TRUE(Poll(core, 0).IsTo(0));
  const std::uint64_t id = core.Id();
  // A replica that is behind does not know the session.
  EXPECT_EQ(Give(core, MakeEviction(id, 2)), ClientEventKind::None);
  EXPECT_TRUE(core.IsPending());
  EXPECT_EQ(core.Id(), id);
  EXPECT_EQ(Give(core, MakeReply({id, 1, kSession, 3}, "one")), ClientEventKind::Reply);
}

TEST(ClientCore, IgnoresEvictionWhileNoRequestWaits)
{
  ClientCore core = MakeCore();
  StartRegistered(core, "first");
  Complete(core, "first", 0);
  const std::uint64_t id = core.Id();
  EXPECT_EQ(Give(core, MakeEviction(id, 0)), ClientEventKind::None);
  EXPECT_EQ(core.Id(), id);
}

TEST(ClientCore, SeedSelectsTheIdentifiers)
{
  constexpr std::uint64_t kSeed = 1234;
  ClientCore first = MakeCore(kSeed);
  ClientCore second = MakeCore(kSeed);
  ClientCore other = MakeCore(kSeed + 1);
  EXPECT_EQ(first.Id(), second.Id());
  EXPECT_NE(first.Id(), other.Id());

  for (ClientCore *core : {&first, &second})
  {
    ASSERT_TRUE(core->Request(Bytes("first"), kClientNever).has_value());
    ASSERT_FALSE(Poll(*core, 0).IsNothing());
    ASSERT_EQ(Give(*core, MakeEviction(core->Id(), 0)), ClientEventKind::Evicted);
  }
  EXPECT_EQ(first.Id(), second.Id());

  constexpr std::uint64_t kFixedId = 99;
  ClientCore fixed = MakeCore(kSeed, kFixedId);
  EXPECT_EQ(fixed.Id(), kFixedId);
}

// A generator for the random test. SplitMix64.
class Random final
{
public:
  explicit Random(std::uint64_t p_seed) noexcept : state_(p_seed) {}

  [[nodiscard]] std::uint64_t Next() noexcept
  {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t mixed = state_;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31);
  }

  // A value in [0, p_limit). Requires p_limit > 0.
  [[nodiscard]] std::uint64_t Below(std::uint64_t p_limit) noexcept { return Next() % p_limit; }
  [[nodiscard]] bool Percent(std::uint64_t p_percent) noexcept { return Below(100) < p_percent; }

private:
  std::uint64_t state_;
};

// A model of what a cluster shows to a client: sessions, at-most-once
// execution, stored replies, view changes, and eviction. It also checks
// that the client obeys the rules for request numbers.
class ModelCluster final
{
public:
  explicit ModelCluster(Random *p_random) noexcept : random_(p_random) {}

  // One request message at one replica. Returns the answer, if there is one.
  [[nodiscard]] std::optional<std::vector<std::byte>> OnRequest(std::uint8_t p_replica,
                                                                std::span<const std::byte> p_bytes)
  {
    const auto message = DecodeMessage(p_bytes);
    if (!message.has_value() || message->header_.command_ != Command::Request ||
        message->header_.cluster_ != kCluster || message->header_.client_ == 0)
    {
      ADD_FAILURE() << "the client sent a message that is not a valid request";
      return std::nullopt;
    }
    const Header &header = message->header_;
    if (p_replica != view_ % kReplicaCount)
    {
      // A replica that is behind supposes that it is the primary, and does
      // not know the client.
      if (random_->Percent(kStaleEvictionPercent))
      {
        return MakeEviction(header.client_, static_cast<std::uint32_t>(random_->Below(view_ + 1)));
      }
      // A backup sends the request on to the primary, or the message is lost.
      if (random_->Percent(kForwardLossPercent))
      {
        return std::nullopt;
      }
    }
    return Handle(header, Text(message->body_));
  }

  void ChangeView() noexcept { view_ += 1 + static_cast<std::uint32_t>(random_->Below(2)); }

  // Removes one session, as a full client table does.
  void EvictOne()
  {
    if (!sessions_.empty())
    {
      auto victim = sessions_.begin();
      std::advance(victim, static_cast<std::ptrdiff_t>(random_->Below(sessions_.size())));
      sessions_.erase(victim);
    }
  }

  // How many times the operation with this body executed.
  [[nodiscard]] std::uint64_t ExecutionCount(const std::string &p_body) const
  {
    const auto found = executions_.find(p_body);
    return found == executions_.end() ? 0 : found->second;
  }

  [[nodiscard]] static std::string ReplyFor(std::string_view p_body)
  {
    return std::format("reply to {}", p_body);
  }

private:
  struct Session
  {
    std::uint64_t session_ = 0;
    std::uint64_t request_ = 0;
    std::string reply_;
  };

  [[nodiscard]] std::optional<std::vector<std::byte>> Handle(const Header &p_header,
                                                             const std::string &p_body)
  {
    const auto found = sessions_.find(p_header.client_);
    if (p_header.request_ == 0)
    {
      EXPECT_EQ(p_header.session_, 0U);
      EXPECT_TRUE(p_body.empty());
      if (found == sessions_.end())
      {
        op_ += 1;
        sessions_[p_header.client_] = Session{op_, 0, {}};
        return MakeReply({p_header.client_, 0, op_, view_});
      }
      if (found->second.request_ == 0)
      {
        return MakeReply({p_header.client_, 0, found->second.session_, view_});
      }
      return std::nullopt;
    }
    if (found == sessions_.end() || found->second.session_ != p_header.session_)
    {
      return MakeEviction(p_header.client_, view_);
    }
    Session &session = found->second;
    if (p_header.request_ == session.request_ + 1)
    {
      op_ += 1;
      executions_[p_body] += 1;
      session.request_ = p_header.request_;
      session.reply_ = ReplyFor(p_body);
      bodies_[{p_header.client_, p_header.request_}] = p_body;
    }
    EXPECT_LE(p_header.request_, session.request_) << "the client skipped a request number";
    if (p_header.request_ != session.request_)
    {
      return std::nullopt;
    }
    // The identifier and the request number identify one operation.
    EXPECT_EQ((bodies_[{p_header.client_, p_header.request_}]), p_body);
    return MakeReply({p_header.client_, p_header.request_, session.session_, view_},
                     session.reply_);
  }

  static constexpr std::uint64_t kStaleEvictionPercent = 2;
  static constexpr std::uint64_t kForwardLossPercent = 50;

  Random *random_;
  std::uint32_t view_ = 0;
  std::uint64_t op_ = 0;
  std::map<std::uint64_t, Session> sessions_;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> bodies_;
  std::map<std::string, std::uint64_t> executions_;
};

struct LossyRunResult
{
  std::uint64_t replies_ = 0;
  std::uint64_t evictions_ = 0;
  std::uint64_t timeouts_ = 0;
  std::uint64_t final_id_ = 0;

  [[nodiscard]] bool operator==(const LossyRunResult &) const = default;
};

// Runs one core against the model through a network that drops, repeats,
// delays, and reorders messages in both directions.
class LossyRun final
{
public:
  explicit LossyRun(std::uint64_t p_seed)
      : random_(p_seed), cluster_(&random_), core_(MakeCore(p_seed))
  {
    ids_.insert(core_.Id());
  }

  [[nodiscard]] LossyRunResult Run()
  {
    for (now_ = 0; now_ < kFaultTicks + kQuietTicks && !::testing::Test::HasFailure(); ++now_)
    {
      faults_ = now_ < kFaultTicks;
      if (faults_ && random_.Percent(1))
      {
        cluster_.ChangeView();
      }
      if (faults_ && random_.Below(kEvictionTicks) == 0)
      {
        cluster_.EvictOne();
      }
      StepApplication();
      const ClientSend send = core_.Poll(now_);
      for (std::uint8_t replica = send.replica_begin_; replica < send.replica_end_; ++replica)
      {
        Enqueue(Packet{0, false, replica, {send.message_.begin(), send.message_.end()}});
      }
      Deliver();
      if (core_.Expire(now_))
      {
        result_.timeouts_ += 1;
      }
    }
    // After the faults stop, the request in progress gets its result.
    EXPECT_FALSE(core_.IsPending());
    EXPECT_GT(result_.replies_, 0U);
    result_.final_id_ = core_.Id();
    return result_;
  }

private:
  struct Packet
  {
    std::uint64_t deliver_tick_ = 0;
    bool to_client_ = false;
    std::uint8_t replica_ = 0;
    std::vector<std::byte> bytes_;
  };

  void StepApplication()
  {
    const std::uint64_t deadline =
        random_.Percent(50) ? kClientNever : now_ + 1 + random_.Below(3 * kRetryInterval);
    if (core_.IsWaiting())
    {
      return;
    }
    if (core_.IsPending())
    {
      ASSERT_TRUE(core_.Resume(deadline).has_value());
      return;
    }
    // Do not start a request that the quiet phase cannot complete.
    if (now_ >= kFaultTicks + kQuietTicks / 2)
    {
      return;
    }
    request_count_ += 1;
    body_ = std::format("operation {}", request_count_);
    results_ = 0;
    ASSERT_TRUE(core_.Request(Bytes(body_), deadline).has_value());
  }

  void Enqueue(Packet p_packet)
  {
    if (faults_ && random_.Percent(kLossPercent))
    {
      return;
    }
    const std::uint64_t copies = faults_ && random_.Percent(kRepeatPercent) ? 2 : 1;
    for (std::uint64_t copy = 0; copy < copies; ++copy)
    {
      p_packet.deliver_tick_ = now_ + 1 + (faults_ ? random_.Below(kDelayTicksMax) : 0);
      packets_.push_back(p_packet);
    }
  }

  void Deliver()
  {
    // A packet that this loop adds has a later delivery tick.
    std::vector<Packet> due;
    const auto first_due =
        std::stable_partition(packets_.begin(), packets_.end(), [this](const Packet &p_packet)
                              { return p_packet.deliver_tick_ > now_; });
    due.assign(std::make_move_iterator(first_due), std::make_move_iterator(packets_.end()));
    packets_.erase(first_due, packets_.end());
    for (const Packet &packet : due)
    {
      if (packet.to_client_)
      {
        OnClientPacket(packet);
        continue;
      }
      auto answer = cluster_.OnRequest(packet.replica_, packet.bytes_);
      if (answer.has_value())
      {
        Enqueue(Packet{0, true, packet.replica_, std::move(*answer)});
      }
    }
  }

  void OnClientPacket(const Packet &p_packet)
  {
    const bool was_pending = core_.IsPending();
    const ClientEvent event = core_.OnMessage(p_packet.bytes_);
    if (event.kind_ == ClientEventKind::None)
    {
      return;
    }
    EXPECT_TRUE(was_pending) << "a result without a request";
    results_ += 1;
    EXPECT_EQ(results_, 1U) << "the request completed twice: " << body_;
    EXPECT_FALSE(core_.IsPending());
    if (event.kind_ == ClientEventKind::Evicted)
    {
      result_.evictions_ += 1;
      EXPECT_TRUE(ids_.insert(core_.Id()).second) << "an identifier came back after an eviction";
      return;
    }
    result_.replies_ += 1;
    EXPECT_EQ(Text(event.reply_), ModelCluster::ReplyFor(body_))
        << "the request completed with the reply of a different request";
    EXPECT_EQ(cluster_.ExecutionCount(body_), 1U) << body_;
  }

  static constexpr std::uint64_t kFaultTicks = 4000;
  static constexpr std::uint64_t kQuietTicks = 400;
  static constexpr std::uint64_t kEvictionTicks = 300;
  static constexpr std::uint64_t kLossPercent = 25;
  static constexpr std::uint64_t kRepeatPercent = 20;
  static constexpr std::uint64_t kDelayTicksMax = 4 * kRetryInterval;

  Random random_;
  ModelCluster cluster_;
  ClientCore core_;
  std::vector<Packet> packets_;
  std::uint64_t now_ = 0;
  bool faults_ = true;
  // The request in progress, or the last one: its body, and how many
  // results it has.
  std::uint64_t request_count_ = 0;
  std::string body_;
  std::uint64_t results_ = 0;
  // Each identifier that the core had.
  std::set<std::uint64_t> ids_;
  LossyRunResult result_;
};

// Many seeds. One request never completes twice, and never with the reply
// of a different request. No operation executes twice.
TEST(ClientCore, LossyNetworkNeverCompletesARequestTwice)
{
  constexpr std::uint64_t kSeedCount = 300;
  LossyRunResult total;
  for (std::uint64_t seed = 1; seed <= kSeedCount && !HasFailure(); ++seed)
  {
    SCOPED_TRACE(std::format("seed {}", seed));
    const LossyRunResult result = LossyRun{seed}.Run();
    total.replies_ += result.replies_;
    total.evictions_ += result.evictions_;
    total.timeouts_ += result.timeouts_;
  }
  // The run must reach each path, or it proves nothing about that path.
  EXPECT_GT(total.replies_, kSeedCount);
  EXPECT_GT(total.evictions_, 0U);
  EXPECT_GT(total.timeouts_, 0U);
  RecordProperty("replies", static_cast<std::int64_t>(total.replies_));
  RecordProperty("evictions", static_cast<std::int64_t>(total.evictions_));
  RecordProperty("timeouts", static_cast<std::int64_t>(total.timeouts_));
}

// One seed gives one behaviour.
TEST(ClientCore, SameSeedGivesTheSameRun)
{
  constexpr std::uint64_t kSeed = 42;
  const LossyRunResult first = LossyRun{kSeed}.Run();
  const LossyRunResult second = LossyRun{kSeed}.Run();
  EXPECT_EQ(first, second);
  const LossyRunResult other = LossyRun{kSeed + 1}.Run();
  EXPECT_NE(first, other);
}

} // namespace

} // namespace vsr
