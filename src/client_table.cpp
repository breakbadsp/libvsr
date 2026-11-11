#include "vsr/internal/client_table.hpp"

#include "vsr/assert.hpp"
#include "vsr/internal/bytes.hpp"

#include <algorithm>
#include <cstring>

namespace vsr::internal
{

ClientTable::ClientTable(std::uint32_t p_clients_max, std::uint32_t p_body_bytes_max)
    : clients_max_(p_clients_max), body_bytes_max_(p_body_bytes_max),
      replies_(std::size_t{p_clients_max} * p_body_bytes_max)
{
  entries_.reserve(clients_max_);
  free_slot_indexes_.reserve(clients_max_);
  Clear();
}

void ClientTable::Clear()
{
  entries_.clear();
  free_slot_indexes_.clear();
  for (std::uint32_t slot_index = clients_max_; slot_index > 0; --slot_index)
  {
    free_slot_indexes_.push_back(slot_index - 1);
  }
}

const ClientEntry *ClientTable::Find(std::uint64_t p_client) const noexcept
{
  const auto found = std::ranges::lower_bound(entries_, p_client, {}, &ClientEntry::client_);
  if (found == entries_.end() || found->client_ != p_client)
  {
    return nullptr;
  }
  return &*found;
}

std::span<std::byte> ClientTable::ReplySlot(std::uint32_t p_slot_index) noexcept
{
  VSR_ASSERT(p_slot_index < clients_max_);
  return std::span{replies_}.subspan(std::size_t{p_slot_index} * body_bytes_max_, body_bytes_max_);
}

std::span<const std::byte> ClientTable::Reply(const ClientEntry &p_entry) const noexcept
{
  VSR_ASSERT(p_entry.reply_slot_index_ < clients_max_);
  return std::span{replies_}.subspan(std::size_t{p_entry.reply_slot_index_} * body_bytes_max_,
                                     p_entry.reply_bytes_);
}

void ClientTable::Commit(const ClientEntry &p_commit, std::span<const std::byte> p_reply)
{
  VSR_ASSERT(p_reply.size() <= body_bytes_max_);
  auto position = std::ranges::lower_bound(entries_, p_commit.client_, {}, &ClientEntry::client_);
  if (position == entries_.end() || position->client_ != p_commit.client_)
  {
    if (entries_.size() == clients_max_)
    {
      // Every replica removes the same client, because `op_` is replicated.
      const auto oldest = std::ranges::min_element(entries_, {}, &ClientEntry::op_);
      free_slot_indexes_.push_back(oldest->reply_slot_index_);
      entries_.erase(oldest);
      position = std::ranges::lower_bound(entries_, p_commit.client_, {}, &ClientEntry::client_);
    }
    VSR_ASSERT(!free_slot_indexes_.empty());
    ClientEntry fresh;
    fresh.client_ = p_commit.client_;
    fresh.reply_slot_index_ = free_slot_indexes_.back();
    free_slot_indexes_.pop_back();
    position = entries_.insert(position, fresh);
  }
  position->session_ = p_commit.session_;
  position->request_ = p_commit.request_;
  position->op_ = p_commit.op_;
  position->reply_bytes_ = static_cast<std::uint32_t>(p_reply.size());
  if (!p_reply.empty())
  {
    std::memcpy(ReplySlot(position->reply_slot_index_).data(), p_reply.data(), p_reply.size());
  }
}

std::size_t ClientTable::Serialize(std::span<std::byte> p_out) const
{
  ByteWriter writer{p_out};
  writer.Put(static_cast<std::uint32_t>(entries_.size()));
  for (const auto &entry : entries_)
  {
    writer.Put(entry.client_);
    writer.Put(entry.session_);
    writer.Put(entry.request_);
    writer.Put(entry.op_);
    writer.Put(entry.reply_bytes_);
    writer.PutBytes(Reply(entry));
  }
  return writer.UsedBytes();
}

std::optional<std::size_t> ClientTable::Deserialize(std::span<const std::byte> p_in)
{
  Clear();
  ByteReader reader{p_in};
  const auto count = reader.Get<std::uint32_t>();
  if (!count || *count > clients_max_)
  {
    return std::nullopt;
  }
  std::uint64_t client_previous = 0;
  for (std::uint32_t entry_index = 0; entry_index < *count; ++entry_index)
  {
    const auto client = reader.Get<std::uint64_t>();
    const auto session = reader.Get<std::uint64_t>();
    const auto request = reader.Get<std::uint64_t>();
    const auto op = reader.Get<std::uint64_t>();
    const auto reply_bytes = reader.Get<std::uint32_t>();
    if (!client || !session || !request || !op || !reply_bytes || *reply_bytes > body_bytes_max_)
    {
      return std::nullopt;
    }
    const auto reply = reader.GetBytes(*reply_bytes);
    if (!reply)
    {
      return std::nullopt;
    }
    // Commit needs the sorted order, and a snapshot always has it.
    if (entry_index > 0 && *client <= client_previous)
    {
      return std::nullopt;
    }
    client_previous = *client;
    ClientEntry entry;
    entry.client_ = *client;
    entry.session_ = *session;
    entry.request_ = *request;
    entry.op_ = *op;
    Commit(entry, *reply);
  }
  return p_in.size() - reader.Rest().size();
}

} // namespace vsr::internal
