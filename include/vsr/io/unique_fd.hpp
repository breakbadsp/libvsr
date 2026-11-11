#pragma once

#include <unistd.h>
#include <utility>

namespace vsr::io
{

// Owns one file descriptor and closes it.
class UniqueFd final
{
public:
  UniqueFd() noexcept = default;

  explicit UniqueFd(int p_fd) noexcept : fd_(p_fd) {}

  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&p_other) noexcept : fd_(std::exchange(p_other.fd_, -1)) {}

  UniqueFd &operator=(UniqueFd &&p_other) noexcept
  {
    if (this != &p_other)
    {
      Reset();
      fd_ = std::exchange(p_other.fd_, -1);
    }
    return *this;
  }

  void Reset() noexcept
  {
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }

  [[nodiscard]] int Get() const noexcept { return fd_; }

  [[nodiscard]] bool IsOpen() const noexcept { return fd_ >= 0; }

private:
  int fd_ = -1;
};

} // namespace vsr::io
