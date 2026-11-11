#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <variant>

// The three interfaces that a host gives to a replica. They are virtual
// because one replica makes a small, fixed number of calls for each
// operation, and the host selects the implementations at run time.

namespace vsr
{

enum class StorageError
{
  Io = 0,
};

// A flat range of bytes. `Write` can return before the data is durable.
// After `Sync` returns, all completed writes survive a power loss.
class Storage
{
public:
  [[nodiscard]] virtual std::expected<void, StorageError> Read(std::uint64_t p_offset,
                                                               std::span<std::byte> p_out) = 0;
  [[nodiscard]] virtual std::expected<void, StorageError>
  Write(std::uint64_t p_offset, std::span<const std::byte> p_bytes) = 0;
  [[nodiscard]] virtual std::expected<void, StorageError> Sync() = 0;

protected:
  ~Storage() = default;
};

// Sends are best effort. The bus can drop, delay, repeat, or reorder
// messages. It must copy `p_message` before the call returns.
class MessageBus
{
public:
  virtual void SendToReplica(std::uint8_t p_replica, std::span<const std::byte> p_message) = 0;
  // `p_message` is a complete `Reply` or `Eviction` message. A host with
  // its own client protocol reads the fields that it needs with
  // `DecodeMessage`.
  virtual void SendToClient(std::uint64_t p_client, std::span<const std::byte> p_message) = 0;

protected:
  ~MessageBus() = default;
};

struct Operation
{
  // Increases with each operation. Some numbers do not reach the state
  // machine: the library uses them for client registration.
  std::uint64_t op_ = 0;
  // Assigned by the primary. It increases strictly with `op_` and is equal
  // on all replicas. Use it in place of the local clock.
  std::uint64_t timestamp_ns_ = 0;
  std::uint64_t client_ = 0;
  std::uint64_t request_ = 0;
};

// The replicated application. Each function must be deterministic: the same
// sequence of calls gives the same replies and the same snapshot bytes on
// all replicas. See docs/host-contract.md.
class StateMachine
{
public:
  // Applies one committed operation and writes the reply into `p_reply`.
  // Returns the reply size. The body is opaque to the library, so this
  // function must also give a reply for a body that it cannot parse.
  [[nodiscard]] virtual std::size_t Execute(const Operation &p_operation,
                                            std::span<const std::byte> p_body,
                                            std::span<std::byte> p_reply) = 0;
  // Writes the full state into `p_out`. Returns the size, or nothing if the
  // state does not fit.
  [[nodiscard]] virtual std::expected<std::size_t, std::monostate>
  Snapshot(std::span<std::byte> p_out) = 0;
  // Replaces the full state with a snapshot. Returns false if the bytes are
  // not a valid snapshot.
  [[nodiscard]] virtual bool Restore(std::span<const std::byte> p_snapshot) = 0;

protected:
  ~StateMachine() = default;
};

} // namespace vsr
