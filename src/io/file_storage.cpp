#include "vsr/io/file_storage.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace vsr::io
{

namespace
{

[[nodiscard]] std::error_code LastError() noexcept
{
  return std::error_code{errno, std::generic_category()};
}

[[nodiscard]] std::expected<void, std::error_code> SyncParentDirectories(const std::string &p_path)
{
  std::error_code error;
  const auto absolute = std::filesystem::absolute(p_path, error);
  if (error)
  {
    return std::unexpected(error);
  }
  const auto resolved = std::filesystem::canonical(p_path, error);
  if (error)
  {
    return std::unexpected(error);
  }
  // Persist the name in the directory of the path, and in the directory of the
  // resolved file when the caller uses a symlink. An ancestor can deny read
  // access to this process, so the host makes its own directories durable.
  for (const auto &directory : {absolute.parent_path(), resolved.parent_path()})
  {
    UniqueFd fd{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (!fd.IsOpen() || ::fsync(fd.Get()) != 0)
    {
      return std::unexpected(LastError());
    }
  }
  return {};
}

} // namespace

std::expected<FileStorage::Opened, std::error_code> FileStorage::Open(const std::string &p_path,
                                                                      std::uint64_t p_bytes)
{
  constexpr mode_t kOwnerReadWrite = 0600;
  UniqueFd fd{::open(p_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, kOwnerReadWrite)};
  if (!fd.IsOpen())
  {
    return std::unexpected(LastError());
  }
  struct stat status = {};
  if (::fstat(fd.Get(), &status) != 0)
  {
    return std::unexpected(LastError());
  }
  const bool created = status.st_size == 0;
  if (created)
  {
    // Reserve the blocks now. A full disk is then an error at startup, not
    // a failed write during replication.
    const int reserved = ::posix_fallocate(fd.Get(), 0, static_cast<off_t>(p_bytes));
    if (reserved != 0)
    {
      return std::unexpected(std::error_code{reserved, std::generic_category()});
    }
    if (::fsync(fd.Get()) != 0)
    {
      return std::unexpected(LastError());
    }
  }
  else if (std::cmp_not_equal(status.st_size, p_bytes))
  {
    return std::unexpected(std::make_error_code(std::errc::file_too_large));
  }
  if (const auto synced = SyncParentDirectories(p_path); !synced)
  {
    return std::unexpected(synced.error());
  }
  return Opened{FileStorage{std::move(fd), p_bytes}, created};
}

std::expected<void, StorageError> FileStorage::Read(std::uint64_t p_offset,
                                                    std::span<std::byte> p_out)
{
  if (p_offset + p_out.size() > bytes_)
  {
    return std::unexpected(StorageError::Io);
  }
  std::size_t done_bytes = 0;
  while (done_bytes < p_out.size())
  {
    const ssize_t count = ::pread(fd_.Get(), p_out.data() + done_bytes, p_out.size() - done_bytes,
                                  static_cast<off_t>(p_offset + done_bytes));
    if (count < 0 && errno == EINTR)
    {
      continue;
    }
    if (count <= 0)
    {
      return std::unexpected(StorageError::Io);
    }
    done_bytes += static_cast<std::size_t>(count);
  }
  return {};
}

std::expected<void, StorageError> FileStorage::Write(std::uint64_t p_offset,
                                                     std::span<const std::byte> p_bytes)
{
  if (p_offset + p_bytes.size() > bytes_)
  {
    return std::unexpected(StorageError::Io);
  }
  std::size_t done_bytes = 0;
  while (done_bytes < p_bytes.size())
  {
    const ssize_t count =
        ::pwrite(fd_.Get(), p_bytes.data() + done_bytes, p_bytes.size() - done_bytes,
                 static_cast<off_t>(p_offset + done_bytes));
    if (count < 0 && errno == EINTR)
    {
      continue;
    }
    if (count <= 0)
    {
      return std::unexpected(StorageError::Io);
    }
    done_bytes += static_cast<std::size_t>(count);
  }
  return {};
}

std::expected<void, StorageError> FileStorage::Sync()
{
  // After a failed fdatasync the kernel can drop the dirty pages and report
  // success the next time. The replica halts on this error and does not
  // call Sync again, which is the only safe reaction.
  if (::fdatasync(fd_.Get()) != 0)
  {
    return std::unexpected(StorageError::Io);
  }
  return {};
}

} // namespace vsr::io
