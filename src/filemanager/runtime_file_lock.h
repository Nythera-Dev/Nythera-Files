#pragma once

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <unistd.h>

namespace filemanager {
  // Use a separate inode because the data file is atomically replaced on every write.
  class RuntimeFileLock {
  public:
    explicit RuntimeFileLock(std::filesystem::path path) {
      if (path.empty())
        return;
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      if (error)
        return;
      path += ".lock";
      m_fd = ::open(path.c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600);
      if (m_fd < 0)
        return;
      int result;
      do {
        result = ::flock(m_fd, LOCK_EX);
      } while (result < 0 && errno == EINTR);
      if (result != 0) {
        ::close(m_fd);
        m_fd = -1;
      }
    }

    RuntimeFileLock(const RuntimeFileLock&) = delete;
    RuntimeFileLock& operator=(const RuntimeFileLock&) = delete;

    ~RuntimeFileLock() {
      if (m_fd >= 0)
        ::close(m_fd);
    }

    explicit operator bool() const noexcept { return m_fd >= 0; }

  private:
    int m_fd = -1;
  };
} // namespace filemanager
