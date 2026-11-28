#include "filemanager/instance_coordinator.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace filemanager {

  namespace {

    constexpr std::size_t kMaxRequest = 16 * 1024;

    std::string sanitizeDisplayName(std::string_view display) {
      std::string out;
      out.reserve(display.size());
      for (const unsigned char c : display) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
          out.push_back(static_cast<char>(c));
        } else {
          out.push_back('_');
        }
      }
      return out.empty() ? std::string("default") : out;
    }

    bool fillSockAddr(const std::filesystem::path& socketPath, sockaddr_un& addr) {
      const std::string text = socketPath.string();
      if (text.empty() || text.size() >= sizeof(addr.sun_path)) {
        return false;
      }
      std::memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, text.data(), text.size());
      return true;
    }

    bool writeAll(int fd, std::string_view data) {
      ssize_t written;
      do {
        written = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
      } while (written < 0 && errno == EINTR);
      return written >= 0 && static_cast<std::size_t>(written) == data.size();
    }

  } // namespace

  InstanceCoordinator::~InstanceCoordinator() { stop(); }

  std::filesystem::path InstanceCoordinator::defaultSocketPath() {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      return {};
    }
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    const std::string suffix = sanitizeDisplayName(waylandDisplay != nullptr ? waylandDisplay : "wayland-0");
    return std::filesystem::path(runtimeDir) / ("nythera-files-" + suffix + ".sock");
  }

  bool InstanceCoordinator::start(std::filesystem::path socketPath, OpenDirectoryCallback onOpenDirectory) {
    stop();
    if (socketPath.empty()) {
      socketPath = defaultSocketPath();
    }
    if (socketPath.empty()) {
      return false;
    }

    sockaddr_un addr{};
    if (!fillSockAddr(socketPath, addr)) {
      return false;
    }

    auto lockPath = socketPath;
    lockPath += ".lock";
    const int lock = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0)
      return false;
    if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
      ::close(lock);
      return false;
    }

    struct stat entry{};
    if (::lstat(socketPath.c_str(), &entry) == 0
        && (!S_ISSOCK(entry.st_mode) || entry.st_uid != ::getuid() || ::unlink(socketPath.c_str()) != 0)) {
      ::close(lock);
      return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      ::close(lock);
      return false;
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      ::close(lock);
      return false;
    }
    if (::listen(fd, 16) != 0) {
      (void)::unlink(socketPath.c_str());
      ::close(fd);
      ::close(lock);
      return false;
    }

    m_lockFd = lock;
    m_listenFd = fd;
    m_socketPath = std::move(socketPath);
    m_onOpenDirectory = std::move(onOpenDirectory);
    return true;
  }

  void InstanceCoordinator::stop() {
    if (m_listenFd >= 0) {
      ::close(m_listenFd);
      m_listenFd = -1;
      if (!m_socketPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_socketPath, ec);
      }
    }
    if (m_lockFd >= 0)
      ::close(std::exchange(m_lockFd, -1));
    m_socketPath.clear();
    m_onOpenDirectory = nullptr;
  }

  void InstanceCoordinator::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_listenFd >= 0) {
      fds.push_back(pollfd{.fd = m_listenFd, .events = POLLIN, .revents = 0});
    }
  }

  void InstanceCoordinator::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (m_listenFd < 0 || startIdx >= fds.size()) {
      return;
    }
    if ((fds[startIdx].revents & POLLIN) != 0) {
      acceptPending();
    }
  }

  void InstanceCoordinator::acceptPending() {
    for (int i = 0; i < 16; ++i) {
      const int client = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (client < 0)
        break;

      struct ucred peer{};
      socklen_t peerSize = sizeof(peer);
      std::array<char, kMaxRequest> bytes{};
      pollfd pfd{.fd = client, .events = POLLIN, .revents = 0};
      bool ok = false;
      if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) == 0
          && peer.uid == ::getuid()
          && ::poll(&pfd, 1, 20) > 0) {
        const auto size = ::recv(client, bytes.data(), bytes.size(), MSG_TRUNC);
        if (size > 0 && static_cast<std::size_t>(size) <= bytes.size()) {
          constexpr std::string_view prefix = "OPEN_TAB\t";
          const auto request = std::string_view(bytes.data(), static_cast<std::size_t>(size));
          if (request.starts_with(prefix) && !request.contains('\0') && m_onOpenDirectory) {
            const std::filesystem::path directory(request.substr(prefix.size()));
            std::error_code error;
            if (directory.is_absolute() && std::filesystem::is_directory(directory, error))
              ok = m_onOpenDirectory(directory);
          }
        }
      }
      (void)writeAll(client, ok ? "OK" : "ERR");
      ::close(client);
    }
  }

  bool InstanceCoordinator::tryForwardOpenTab(
      const std::filesystem::path& directory, const std::filesystem::path& socketPath
  ) {
    const auto resolved = socketPath.empty() ? defaultSocketPath() : socketPath;
    if (resolved.empty() || directory.empty()) {
      return false;
    }
    sockaddr_un addr{};
    if (!fillSockAddr(resolved, addr)) {
      return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      return false;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      return false;
    }
    const std::string payload = "OPEN_TAB\t" + directory.string();
    if (payload.size() > kMaxRequest || !writeAll(fd, payload)) {
      ::close(fd);
      return false;
    }
    std::array<char, 64> reply{};
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    bool ok = false;
    if (::poll(&pfd, 1, 500) > 0) {
      const ssize_t n = ::read(fd, reply.data(), reply.size());
      if (n > 0 && std::string_view(reply.data(), static_cast<std::size_t>(n)) == "OK") {
        ok = true;
      }
    }
    ::close(fd);
    return ok;
  }

} // namespace filemanager
