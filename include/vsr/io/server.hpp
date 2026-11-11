#pragma once

#include "vsr/config.hpp"
#include "vsr/interfaces.hpp"
#include "vsr/replica.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace vsr::io
{

struct ServerOptions
{
  Config config_;
  // One "a.b.c.d:port" for each replica. The index is the replica number.
  std::vector<std::string> addresses_;
  // The storage file. The server makes and formats it if it does not exist.
  std::string data_path_;
  std::uint32_t tick_ms_ = 10;
  // Messages to a connection that is slower than this are dropped.
  std::size_t send_queue_bytes_max_ = 16 * 1024 * 1024;
  std::uint32_t client_connections_max_ = 256;
};

// One replica with the default storage, TCP network, and clock. It runs the
// replica on the thread that calls `Run`.
class Server final
{
public:
  // The state machine must be in its initial state and must live longer
  // than the server.
  [[nodiscard]] static std::expected<Server, std::string> Create(ServerOptions p_options,
                                                                 StateMachine &p_state_machine);

  Server(Server &&) noexcept;
  Server &operator=(Server &&) noexcept;
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  ~Server();

  // Serves until `Stop` is called. Returns the reason if the replica halts.
  [[nodiscard]] std::expected<void, HaltReason> Run();

  // Makes `Run` return. Safe to call from another thread.
  void Stop() noexcept;

  // Only from the thread that calls `Run`, or while `Run` is not active.
  [[nodiscard]] const Replica &GetReplica() const noexcept;

private:
  class Impl;
  explicit Server(std::unique_ptr<Impl> p_impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace vsr::io
