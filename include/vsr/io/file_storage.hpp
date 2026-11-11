#pragma once

#include "vsr/interfaces.hpp"
#include "vsr/io/unique_fd.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

namespace vsr::io
{

// The default storage: one file of fixed size. Writes go through the page
// cache, and `Sync` calls `fdatasync`.
class FileStorage final : public Storage
{
public:
  struct Opened;

  // Opens the file, or makes it with `p_bytes` zero bytes. An existing file
  // of another size is an error: it belongs to another configuration.
  [[nodiscard]] static std::expected<Opened, std::error_code> Open(const std::string &p_path,
                                                                   std::uint64_t p_bytes);

  [[nodiscard]] std::expected<void, StorageError> Read(std::uint64_t p_offset,
                                                       std::span<std::byte> p_out) override;
  [[nodiscard]] std::expected<void, StorageError>
  Write(std::uint64_t p_offset, std::span<const std::byte> p_bytes) override;
  [[nodiscard]] std::expected<void, StorageError> Sync() override;

private:
  FileStorage(UniqueFd p_fd, std::uint64_t p_bytes) noexcept : fd_(std::move(p_fd)), bytes_(p_bytes)
  {
  }

  UniqueFd fd_;
  std::uint64_t bytes_ = 0;
};

struct FileStorage::Opened
{
  FileStorage storage_;
  // True if this call made the file. The caller must format it.
  bool created_ = false;
};

} // namespace vsr::io
