#include "test_support.hpp"
#include "vsr/io/client.hpp"
#include "vsr/io/file_storage.hpp"
#include "vsr/io/server.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// The default storage, TCP network, runner, and client, with real sockets
// and files on this machine.

namespace vsr::io
{

namespace
{

using test::Bytes;
using namespace std::chrono_literals;

constexpr std::size_t kReplicaCount = 3;
constexpr std::chrono::seconds kRequestTimeout{5};

// A port that was free a moment ago: the kernel selects it, then the socket
// closes. Another process can take it before the server binds, which is
// improbable on a test machine.
[[nodiscard]] std::uint16_t FreePort()
{
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t address_bytes = sizeof(address);
  ::bind(fd, reinterpret_cast<const sockaddr *>(&address), address_bytes);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &address_bytes);
  ::close(fd);
  return ntohs(address.sin_port);
}

class Cluster : public ::testing::Test
{
protected:
  void SetUp() override
  {
    directory_ = std::filesystem::temp_directory_path() /
                 std::format("vsr_io_test_{}_{}", ::getpid(),
                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
    for (std::size_t replica = 0; replica < kReplicaCount; ++replica)
    {
      addresses_.push_back(std::format("127.0.0.1:{}", FreePort()));
    }
    for (std::size_t replica = 0; replica < kReplicaCount; ++replica)
    {
      Start(replica);
    }
  }

  void TearDown() override
  {
    for (std::size_t replica = 0; replica < kReplicaCount; ++replica)
    {
      // After a failure, the state of each replica shows where the cluster
      // stopped. The server thread must end before the read.
      if (HasFailure() && nodes_[replica].server_ != nullptr)
      {
        nodes_[replica].server_->Stop();
        nodes_[replica].thread_.join();
        const Replica &state = nodes_[replica].server_->GetReplica();
        ADD_FAILURE() << std::format(
            "replica {}: status {} view {} log_view {} op {} commit_min {} commit_max {} "
            "checkpoint {}",
            replica, std::to_underlying(state.GetStatus()), state.View(), state.LogView(),
            state.Op(), state.CommitMin(), state.CommitMax(), state.CheckpointOp());
        nodes_[replica].server_.reset();
      }
      Stop(replica);
    }
    std::filesystem::remove_all(directory_);
  }

  [[nodiscard]] static Config MakeConfig(std::size_t p_replica)
  {
    Config config;
    config.cluster_ = 77;
    config.replica_count_ = kReplicaCount;
    config.replica_ = static_cast<std::uint8_t>(p_replica);
    config.body_bytes_max_ = 4096;
    config.log_slot_count_ = 64;
    config.checkpoint_interval_ = 16;
    config.snapshot_bytes_max_ = 64 * 1024;
    return config;
  }

  // Starts a replica as a new process does: a new state machine, and the
  // storage file from the run before, if there is one.
  void Start(std::size_t p_replica)
  {
    auto &node = nodes_[p_replica];
    node.state_machine_ = std::make_unique<test::KeyValueStateMachine>();
    ServerOptions options;
    options.config_ = MakeConfig(p_replica);
    options.addresses_ = addresses_;
    options.data_path_ = (directory_ / std::format("replica{}.vsr", p_replica)).string();
    options.tick_ms_ = 2;
    auto server = Server::Create(std::move(options), *node.state_machine_);
    ASSERT_TRUE(server.has_value()) << server.error();
    node.server_ = std::make_unique<Server>(std::move(*server));
    // The thread gets the pointer by value. The server object lives until
    // Stop joins the thread.
    node.thread_ = std::jthread{[server_pointer = node.server_.get()]()
                                { static_cast<void>(server_pointer->Run()); }};
  }

  void Stop(std::size_t p_replica)
  {
    auto &node = nodes_[p_replica];
    if (node.server_ == nullptr)
    {
      return;
    }
    node.server_->Stop();
    node.thread_.join();
    node.server_.reset();
  }

  [[nodiscard]] Client MakeClient() const
  {
    ClientOptions options;
    options.cluster_ = 77;
    options.addresses_ = addresses_;
    options.body_bytes_max_ = 4096;
    options.retry_ms_ = 50;
    auto client = Client::Create(std::move(options));
    EXPECT_TRUE(client.has_value());
    return std::move(*client);
  }

  // Sets a key through the cluster, and checks that the reply is the value
  // that the last acknowledged write gave the key.
  void Set(Client &p_client, const std::string &p_key, const std::string &p_value)
  {
    const auto reply = p_client.Request(Bytes(p_key + "=" + p_value), kRequestTimeout);
    ASSERT_TRUE(reply.has_value()) << p_key;
    EXPECT_EQ(test::Text(*reply), expected_[p_key]) << p_key;
    expected_[p_key] = p_value;
  }

  struct Node
  {
    std::unique_ptr<test::KeyValueStateMachine> state_machine_;
    std::unique_ptr<Server> server_;
    std::jthread thread_;
  };

  std::filesystem::path directory_;
  std::vector<std::string> addresses_;
  std::array<Node, kReplicaCount> nodes_;
  std::map<std::string, std::string> expected_;
};

} // namespace

// A file that the storage made has the requested size and reads back what
// was written. A file of another size does not open.
TEST(FileStorage, MakesFileAndRefusesOtherSize)
{
  const auto path =
      std::filesystem::temp_directory_path() / std::format("vsr_file_storage_test_{}", ::getpid());
  std::filesystem::remove(path);
  {
    auto opened = FileStorage::Open(path.string(), 8192);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->created_);
    ASSERT_TRUE(opened->storage_.Write(4000, Bytes("hello")).has_value());
    ASSERT_TRUE(opened->storage_.Sync().has_value());
    EXPECT_FALSE(opened->storage_.Write(8190, Bytes("hello")).has_value());
  }
  {
    auto opened = FileStorage::Open(path.string(), 8192);
    ASSERT_TRUE(opened.has_value());
    EXPECT_FALSE(opened->created_);
    std::array<std::byte, 5> bytes{};
    ASSERT_TRUE(opened->storage_.Read(4000, bytes).has_value());
    EXPECT_EQ(test::Text(bytes), "hello");
  }
  EXPECT_EQ(std::filesystem::file_size(path), 8192U);
  EXPECT_FALSE(FileStorage::Open(path.string(), 4096).has_value());
  std::filesystem::remove(path);
}

// An ancestor that denies read access must not stop the open. Home
// directories often have this mode.
TEST(FileStorage, OpensUnderAncestorWithoutReadAccess)
{
  if (::geteuid() == 0)
  {
    // Root ignores directory modes, so the test proves nothing. The
    // AlmaLinux 8 GoogleTest has no GTEST_SKIP.
    return;
  }
  const auto root =
      std::filesystem::temp_directory_path() / std::format("vsr_ancestor_test_{}", ::getpid());
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "data");
  std::filesystem::permissions(root, std::filesystem::perms::owner_write |
                                         std::filesystem::perms::owner_exec);
  const auto opened = FileStorage::Open((root / "data" / "file").string(), 8192);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  EXPECT_TRUE(opened.has_value());
  std::filesystem::remove_all(root);
}

// A power loss after the file is durable and before the superblock is
// leaves a blank file. The next start formats it again.
TEST(Server, FormatsBlankFileFromInterruptedFirstStart)
{
  const auto path =
      std::filesystem::temp_directory_path() / std::format("vsr_blank_test_{}", ::getpid());
  std::filesystem::remove(path);
  test::KeyValueStateMachine state_machine;
  ServerOptions options;
  options.config_.cluster_ = 78;
  options.config_.replica_count_ = 1;
  options.config_.body_bytes_max_ = 4096;
  options.config_.log_slot_count_ = 64;
  options.config_.checkpoint_interval_ = 16;
  options.config_.snapshot_bytes_max_ = 64 * 1024;
  options.addresses_ = {std::format("127.0.0.1:{}", FreePort())};
  options.data_path_ = path.string();
  {
    auto blank = FileStorage::Open(path.string(), StorageBytes(options.config_));
    ASSERT_TRUE(blank.has_value());
    EXPECT_TRUE(blank->created_);
  }
  auto server = Server::Create(std::move(options), state_machine);
  EXPECT_TRUE(server.has_value()) << (server ? "" : server.error());
  std::filesystem::remove(path);
}

// A stop before loop entry must not be cleared by the worker thread.
TEST(Server, StopBeforeRunReturns)
{
  const auto path =
      std::filesystem::temp_directory_path() / std::format("vsr_server_stop_test_{}", ::getpid());
  std::filesystem::remove(path);
  test::KeyValueStateMachine state_machine;
  ServerOptions options;
  options.config_.cluster_ = 77;
  options.config_.replica_count_ = 1;
  options.config_.body_bytes_max_ = 4096;
  options.config_.log_slot_count_ = 64;
  options.config_.checkpoint_interval_ = 16;
  options.config_.snapshot_bytes_max_ = 64 * 1024;
  options.addresses_ = {std::format("127.0.0.1:{}", FreePort())};
  options.data_path_ = path.string();
  auto server = Server::Create(std::move(options), state_machine);
  ASSERT_TRUE(server.has_value()) << server.error();
  server->Stop();
  std::atomic<bool> finished{false};
  std::jthread worker{[server_pointer = &*server, finished_pointer = &finished]()
                      {
                        static_cast<void>(server_pointer->Run());
                        finished_pointer->store(true);
                      }};
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!finished.load() && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(finished.load());
  // A second stop releases a faulty implementation so the test can report it.
  server->Stop();
  worker.join();
  std::filesystem::remove(path);
}

// Three servers on TCP serve a client. Each reply shows the effect of every
// earlier request, so the requests executed once and in order.
TEST_F(Cluster, ServesRequests)
{
  Client client = MakeClient();
  for (int index = 0; index < 40 && !HasFatalFailure(); ++index)
  {
    Set(client, std::format("key{}", index % 7), std::format("value{}", index));
  }
}

// A caller that has no time limit gives the largest duration. The client
// must wait for the reply, not report a timeout at once.
TEST_F(Cluster, WaitsWithTheLargestTimeout)
{
  Client client = MakeClient();
  const auto reply = client.Request(Bytes("key=value"), std::chrono::milliseconds::max());
  EXPECT_TRUE(reply.has_value());
}

// A request body of the largest size goes through the network and the log.
TEST_F(Cluster, ServesLargestBody)
{
  Client client = MakeClient();
  const std::string value(4096 - 2, 'x');
  Set(client, "k", value);
  const auto reply = client.Request(Bytes("k=short"), kRequestTimeout);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(test::Text(*reply), value);
  EXPECT_FALSE(client.Request(Bytes(std::string(4097, 'x')), kRequestTimeout).has_value());
}

// Each replica stops and starts again in turn, while the client continues.
// The cluster serves with two replicas, and a replica that was away gets
// the operations that it missed: it is necessary for the quorum when the
// next replica stops. More operations run than the log ring holds, so a
// replica that starts again uses its checkpoint.
TEST_F(Cluster, ContinuesWhileEachReplicaRestarts)
{
  Client client = MakeClient();
  int value = 0;
  const auto run_requests = [this, &client, &value](int p_count)
  {
    for (int index = 0; index < p_count && !HasFatalFailure(); ++index)
    {
      value += 1;
      Set(client, std::format("key{}", value % 5), std::format("value{}", value));
    }
  };
  run_requests(30);
  for (std::size_t replica = 0; replica < kReplicaCount; ++replica)
  {
    Stop(replica);
    run_requests(40);
    Start(replica);
  }
  run_requests(10);

  // Backups execute an operation when the next message of the primary
  // tells them that it is committed.
  std::this_thread::sleep_for(500ms);
  for (std::size_t replica = 0; replica < kReplicaCount; ++replica)
  {
    Stop(replica);
    EXPECT_EQ(nodes_[replica].state_machine_->values_, expected_) << "replica " << replica;
  }
}

// A caller can wait in steps shorter than the retry interval. The client
// must still send to every replica, or it never finds a primary that is not
// the one it supposes.
TEST_F(Cluster, FindsThePrimaryWithShortWaits)
{
  Stop(0); // a new client supposes that replica 0 is the primary
  Client client = MakeClient();
  constexpr auto kShortWait = 10ms; // the retry interval is 50 ms
  auto reply = client.Request(Bytes("key=value"), kShortWait);
  const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;
  while (!reply.has_value() && reply.error() == ClientError::Timeout &&
         std::chrono::steady_clock::now() < deadline)
  {
    reply = client.Resume(kShortWait);
  }
  EXPECT_TRUE(reply.has_value());
}

// After every replica stops at the same time, the cluster starts again from
// its files with every acknowledged operation.
TEST_F(Cluster, KeepsStateWhenAllReplicasRestart)
{
  {
    Client client = MakeClient();
    for (int index = 0; index < 50 && !HasFatalFailure(); ++index)
    {
      Set(client, std::format("key{}", index % 5), std::format("value{}", index));
    }
  }
  for (std::size_t replica = 0; replica < kReplicaCount; ++replica)
  {
    Stop(replica);
  }
  for (std::size_t replica = 0; replica < kReplicaCount; ++replica)
  {
    Start(replica);
  }
  Client client = MakeClient();
  for (int index = 0; index < 5; ++index)
  {
    Set(client, std::format("key{}", index), "after");
  }
}

} // namespace vsr::io
