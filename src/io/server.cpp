#include "vsr/io/server.hpp"

#include "socket.hpp"
#include "vsr/assert.hpp"
#include "vsr/internal/layout.hpp"
#include "vsr/io/file_storage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <format>
#include <optional>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace vsr::io
{

namespace
{

constexpr std::uint64_t kListenToken = ~std::uint64_t{0};
constexpr std::uint64_t kWakeToken = kListenToken - 1;
constexpr std::uint64_t kNsPerMs = 1'000'000;
constexpr int kEventsPerWait = 64;
constexpr int kListenBacklog = 64;
// Reads for one connection in one loop turn, so that one busy connection
// does not starve the others or the timers.
constexpr int kReadsPerEvent = 8;
// Ticks that the loop runs at once after a stall. More than this are skipped.
constexpr int kTickCatchUpMax = 4;
constexpr std::uint64_t kReconnectTicks = 25;

struct Connection
{
  explicit Connection(std::uint32_t p_body_bytes_max) : in_(p_body_bytes_max) {}

  UniqueFd fd_;
  bool connecting_ = false;
  bool write_registered_ = false;
  // Closed, but the object stays until the loop turn ends, because a
  // message handler further up the stack can still use it.
  bool dead_ = false;
  // The client that sent a request on this connection. Zero if none.
  std::uint64_t client_ = 0;
  internal::FrameBuffer in_;
  std::vector<std::byte> out_;
  std::size_t out_offset_ = 0;
};

} // namespace

class Server::Impl final
{
public:
  Impl(ServerOptions p_options, std::vector<sockaddr_in> p_addresses, FileStorage p_storage,
       StateMachine &p_state_machine)
      : options_(std::move(p_options)), addresses_(std::move(p_addresses)),
        storage_(std::move(p_storage)), state_machine_(&p_state_machine), bus_(this),
        connections_(options_.config_.replica_count_ + options_.client_connections_max_)
  {
  }

  [[nodiscard]] std::expected<void, std::string> Start()
  {
    epoll_.Reset();
    epoll_ = UniqueFd{::epoll_create1(EPOLL_CLOEXEC)};
    wake_ = UniqueFd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    listener_ = internal::MakeSocket();
    if (!epoll_.IsOpen() || !wake_.IsOpen() || !listener_.IsOpen())
    {
      return std::unexpected(std::format("cannot make descriptors: {}", std::strerror(errno)));
    }
    const int enabled = 1;
    ::setsockopt(listener_.Get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    const auto &own = addresses_[options_.config_.replica_];
    // The sockets API takes the generic address type.
    const auto *const own_address = reinterpret_cast<const sockaddr *>(&own);
    if (::bind(listener_.Get(), own_address, sizeof(own)) != 0 ||
        ::listen(listener_.Get(), kListenBacklog) != 0)
    {
      return std::unexpected(std::format("cannot listen on {}: {}",
                                         options_.addresses_[options_.config_.replica_],
                                         std::strerror(errno)));
    }
    if (!Register(listener_.Get(), kListenToken, EPOLLIN) ||
        !Register(wake_.Get(), kWakeToken, EPOLLIN))
    {
      return std::unexpected(std::format("cannot use epoll: {}", std::strerror(errno)));
    }
    auto replica = Replica::Open(options_.config_, storage_, bus_, *state_machine_);
    if (!replica)
    {
      return std::unexpected(
          std::format("cannot open the replica: error {}", std::to_underlying(replica.error())));
    }
    replica_.emplace(std::move(*replica));
    next_tick_ns_ = internal::MonotonicNs();
    return {};
  }

  [[nodiscard]] std::expected<void, HaltReason> Run()
  {
    while (!stop_.load())
    {
      const std::uint64_t now_ns = internal::MonotonicNs();
      const std::uint64_t wait_ns = next_tick_ns_ > now_ns ? next_tick_ns_ - now_ns : 0;
      // Round up, so that the loop does not spin for the last part of a tick.
      const int wait_ms = static_cast<int>((wait_ns + kNsPerMs - 1) / kNsPerMs);
      std::array<epoll_event, kEventsPerWait> events{};
      const int event_count = ::epoll_wait(epoll_.Get(), events.data(), kEventsPerWait, wait_ms);
      messages_handled_ = false;
      for (int index = 0; index < event_count; ++index)
      {
        HandleEvent(events[static_cast<std::size_t>(index)]);
      }
      if (messages_handled_)
      {
        replica_->Flush();
      }
      RunTicks();
      RemoveDeadConnections();
      if (replica_->GetStatus() == Status::Halted)
      {
        return std::unexpected(replica_->GetHaltReason());
      }
    }
    return {};
  }

  void Stop() noexcept
  {
    stop_.store(true);
    const std::uint64_t one = 1;
    // The write only wakes the loop. If it fails, the loop wakes at the next tick.
    static_cast<void>(::write(wake_.Get(), &one, sizeof(one)));
  }

  [[nodiscard]] const Replica &GetReplica() const noexcept { return *replica_; }

private:
  class Bus final : public MessageBus
  {
  public:
    explicit Bus(Impl *p_impl) noexcept : impl_(p_impl) {}

    void SendToReplica(std::uint8_t p_replica, std::span<const std::byte> p_message) override
    {
      impl_->Queue(p_replica, p_message);
    }

    void SendToClient(std::uint64_t p_client, std::span<const std::byte> p_message) override
    {
      const auto found = impl_->client_slots_.find(p_client);
      if (found != impl_->client_slots_.end())
      {
        impl_->Queue(found->second, p_message);
      }
    }

  private:
    Impl *impl_;
  };

  [[nodiscard]] bool Register(int p_fd, std::uint64_t p_token, std::uint32_t p_events)
  {
    epoll_event event = {};
    event.events = p_events;
    event.data.u64 = p_token;
    return ::epoll_ctl(epoll_.Get(), EPOLL_CTL_ADD, p_fd, &event) == 0;
  }

  void SetWriteInterest(std::size_t p_slot, bool p_enabled)
  {
    auto &connection = *connections_[p_slot];
    if (connection.write_registered_ == p_enabled)
    {
      return;
    }
    epoll_event event = {};
    event.events = p_enabled ? EPOLLIN | EPOLLOUT : EPOLLIN;
    event.data.u64 = p_slot;
    if (::epoll_ctl(epoll_.Get(), EPOLL_CTL_MOD, connection.fd_.Get(), &event) == 0)
    {
      connection.write_registered_ = p_enabled;
    }
  }

  [[nodiscard]] bool IsLive(std::size_t p_slot) const
  {
    return connections_[p_slot].has_value() && !connections_[p_slot]->dead_;
  }

  void Close(std::size_t p_slot)
  {
    auto &connection = *connections_[p_slot];
    connection.dead_ = true;
    // Closing the descriptor also removes it from epoll.
    connection.fd_.Reset();
    const auto found = client_slots_.find(connection.client_);
    if (found != client_slots_.end() && found->second == p_slot)
    {
      client_slots_.erase(found);
    }
  }

  void RemoveDeadConnections()
  {
    for (auto &connection : connections_)
    {
      if (connection.has_value() && connection->dead_)
      {
        connection.reset();
      }
    }
  }

  void ConnectPeer(std::size_t p_replica)
  {
    VSR_ASSERT(!connections_[p_replica].has_value());
    Connection connection{options_.config_.body_bytes_max_};
    connection.fd_ = internal::MakeSocket();
    if (!connection.fd_.IsOpen() ||
        !internal::StartConnect(connection.fd_.Get(), addresses_[p_replica]))
    {
      return;
    }
    connection.connecting_ = true;
    connection.write_registered_ = true;
    if (!Register(connection.fd_.Get(), p_replica, EPOLLIN | EPOLLOUT))
    {
      return;
    }
    connections_[p_replica].emplace(std::move(connection));
  }

  void ConnectMissingPeers()
  {
    for (std::size_t replica = 0; replica < options_.config_.replica_count_; ++replica)
    {
      if (replica != options_.config_.replica_ && !connections_[replica].has_value())
      {
        ConnectPeer(replica);
      }
    }
  }

  void Accept()
  {
    for (int accepted_count = 0; accepted_count < kListenBacklog; ++accepted_count)
    {
      UniqueFd fd{::accept4(listener_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
      if (!fd.IsOpen())
      {
        return;
      }
      // Slots after the replicas are for accepted connections. With no
      // free slot, the new connection closes here.
      for (std::size_t slot = options_.config_.replica_count_; slot < connections_.size(); ++slot)
      {
        if (connections_[slot].has_value())
        {
          continue;
        }
        if (Register(fd.Get(), slot, EPOLLIN))
        {
          connections_[slot].emplace(options_.config_.body_bytes_max_);
          connections_[slot]->fd_ = std::move(fd);
        }
        break;
      }
    }
  }

  void HandleEvent(const epoll_event &p_event)
  {
    const std::uint64_t token = p_event.data.u64;
    if (token == kListenToken)
    {
      Accept();
      return;
    }
    if (token == kWakeToken)
    {
      std::uint64_t count = 0;
      static_cast<void>(::read(wake_.Get(), &count, sizeof(count)));
      return;
    }
    const auto slot = static_cast<std::size_t>(token);
    // An earlier event of this turn can close the connection.
    if (!IsLive(slot))
    {
      return;
    }
    if ((p_event.events & EPOLLOUT) != 0)
    {
      OnWritable(slot);
    }
    if (IsLive(slot) && (p_event.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0)
    {
      OnReadable(slot);
    }
  }

  void OnWritable(std::size_t p_slot)
  {
    auto &connection = *connections_[p_slot];
    if (connection.connecting_)
    {
      if (!internal::ConnectSucceeded(connection.fd_.Get()))
      {
        Close(p_slot);
        return;
      }
      connection.connecting_ = false;
    }
    FlushOut(p_slot);
  }

  void OnReadable(std::size_t p_slot)
  {
    for (int read_count = 0; read_count < kReadsPerEvent && IsLive(p_slot); ++read_count)
    {
      auto &connection = *connections_[p_slot];
      const auto result = connection.in_.ReadFrom(connection.fd_.Get());
      if (result == internal::ReadResult::WouldBlock)
      {
        return;
      }
      if (result == internal::ReadResult::Closed)
      {
        Close(p_slot);
        return;
      }
      const bool valid = connection.in_.Drain([this, p_slot](std::span<const std::byte> p_message)
                                              { OnMessage(p_slot, p_message); });
      if (!valid && IsLive(p_slot))
      {
        Close(p_slot);
      }
    }
  }

  void OnMessage(std::size_t p_slot, std::span<const std::byte> p_message)
  {
    Header header;
    std::memcpy(&header, p_message.data(), kHeaderBytes);
    auto &connection = *connections_[p_slot];
    // The reply to a request goes to the connection that sent the request
    // last. A request that a replica sent on does not name a client
    // connection.
    const bool from_client = header.command_ == Command::Request && header.forwarded_ == 0 &&
                             !connection.dead_ && p_slot >= options_.config_.replica_count_;
    if (from_client && connection.client_ != header.client_)
    {
      const auto old = client_slots_.find(connection.client_);
      if (old != client_slots_.end() && old->second == p_slot)
      {
        client_slots_.erase(old);
      }
      connection.client_ = header.client_;
    }
    if (from_client)
    {
      client_slots_[header.client_] = p_slot;
    }
    replica_->OnMessage(p_message);
    messages_handled_ = true;
  }

  void Queue(std::size_t p_slot, std::span<const std::byte> p_message)
  {
    if (!IsLive(p_slot))
    {
      return;
    }
    auto &connection = *connections_[p_slot];
    const std::size_t queued_bytes = connection.out_.size() - connection.out_offset_;
    if (queued_bytes + p_message.size() > options_.send_queue_bytes_max_)
    {
      // The replica sends again what the protocol needs.
      return;
    }
    // Reclaim each consumed prefix before appending. Otherwise a connection
    // that never drains retains bytes from every earlier partial send.
    if (connection.out_offset_ != 0)
    {
      std::memmove(connection.out_.data(), connection.out_.data() + connection.out_offset_,
                   queued_bytes);
      connection.out_.resize(queued_bytes);
      connection.out_offset_ = 0;
    }
    connection.out_.insert(connection.out_.end(), p_message.begin(), p_message.end());
    if (!connection.connecting_)
    {
      FlushOut(p_slot);
    }
  }

  void FlushOut(std::size_t p_slot)
  {
    auto &connection = *connections_[p_slot];
    while (connection.out_offset_ < connection.out_.size())
    {
      const ssize_t count =
          ::send(connection.fd_.Get(), connection.out_.data() + connection.out_offset_,
                 connection.out_.size() - connection.out_offset_, MSG_NOSIGNAL);
      if (count > 0)
      {
        connection.out_offset_ += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR)
      {
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
        SetWriteInterest(p_slot, true);
        return;
      }
      Close(p_slot);
      return;
    }
    connection.out_.clear();
    connection.out_offset_ = 0;
    SetWriteInterest(p_slot, false);
  }

  void RunTicks()
  {
    const std::uint64_t tick_ns = options_.tick_ms_ * kNsPerMs;
    std::uint64_t now_ns = internal::MonotonicNs();
    int ticks_run = 0;
    while (now_ns >= next_tick_ns_ && ticks_run < kTickCatchUpMax)
    {
      replica_->Tick(internal::RealtimeNs());
      next_tick_ns_ += tick_ns;
      tick_count_ += 1;
      ticks_run += 1;
      if (tick_count_ % kReconnectTicks == 1)
      {
        ConnectMissingPeers();
      }
    }
    if (ticks_run == 0)
    {
      return;
    }
    replica_->Flush();
    now_ns = internal::MonotonicNs();
    if (now_ns >= next_tick_ns_)
    {
      next_tick_ns_ = now_ns + tick_ns;
    }
  }

  ServerOptions options_;
  std::vector<sockaddr_in> addresses_;
  FileStorage storage_;
  StateMachine *state_machine_;
  Bus bus_;
  // Slot r, for r below the replica count, is the connection that this
  // server opened to replica r. The other slots are accepted connections.
  std::vector<std::optional<Connection>> connections_;
  std::unordered_map<std::uint64_t, std::size_t> client_slots_;
  std::optional<Replica> replica_;
  UniqueFd epoll_;
  UniqueFd wake_;
  UniqueFd listener_;
  std::atomic<bool> stop_{false};
  bool messages_handled_ = false;
  std::uint64_t next_tick_ns_ = 0;
  std::uint64_t tick_count_ = 0;
};

Server::Server(std::unique_ptr<Impl> p_impl) noexcept : impl_(std::move(p_impl)) {}

Server::Server(Server &&) noexcept = default;
Server &Server::operator=(Server &&) noexcept = default;
Server::~Server() = default;

namespace
{

// A power loss can stop the first start after the file exists on storage and
// before the superblock does. The replica acknowledged nothing then, so a
// file whose superblock sectors are all zero is formatted again.
[[nodiscard]] bool HasSuperblockData(FileStorage &p_storage)
{
  std::vector<std::byte> sectors(vsr::internal::kSuperblockCopyCount * vsr::internal::kSectorBytes);
  if (!p_storage.Read(0, sectors))
  {
    // Let `Replica::Open` report the fault.
    return true;
  }
  return std::ranges::any_of(sectors, [](std::byte p_byte) { return p_byte != std::byte{0}; });
}

} // namespace

std::expected<Server, std::string> Server::Create(ServerOptions p_options,
                                                  StateMachine &p_state_machine)
{
  if (!ValidateConfig(p_options.config_))
  {
    return std::unexpected(std::string{"the configuration is not valid"});
  }
  if (p_options.addresses_.size() != p_options.config_.replica_count_ || p_options.tick_ms_ == 0)
  {
    return std::unexpected(std::string{"one address for each replica is necessary"});
  }
  std::vector<sockaddr_in> addresses;
  for (const auto &text : p_options.addresses_)
  {
    const auto address = internal::ParseAddress(text);
    if (!address)
    {
      return std::unexpected(address.error());
    }
    addresses.push_back(*address);
  }
  auto opened = FileStorage::Open(p_options.data_path_, StorageBytes(p_options.config_));
  if (!opened)
  {
    return std::unexpected(
        std::format("cannot open '{}': {}", p_options.data_path_, opened.error().message()));
  }
  if ((opened->created_ || !HasSuperblockData(opened->storage_)) &&
      !Format(opened->storage_, p_options.config_))
  {
    return std::unexpected(std::format("cannot format '{}'", p_options.data_path_));
  }
  auto impl = std::make_unique<Impl>(std::move(p_options), std::move(addresses),
                                     std::move(opened->storage_), p_state_machine);
  if (const auto started = impl->Start(); !started)
  {
    return std::unexpected(started.error());
  }
  return Server{std::move(impl)};
}

std::expected<void, HaltReason> Server::Run() { return impl_->Run(); }

void Server::Stop() noexcept { impl_->Stop(); }

const Replica &Server::GetReplica() const noexcept { return impl_->GetReplica(); }

} // namespace vsr::io
