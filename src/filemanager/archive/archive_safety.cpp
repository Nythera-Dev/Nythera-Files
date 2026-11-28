// Adapted from Luciole archive_support.cpp; see the project LICENSE.
#include "archive_input.h"

#include <algorithm>
#include <archive.h>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <ranges>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  bool asciiAlpha(char value) { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); }
  bool absolute(const std::string& name) {
    return !name.empty()
        && (name.front() == '/' || name.front() == '\\' || (name.size() >= 2 && asciiAlpha(name[0]) && name[1] == ':'));
  }
} // namespace

namespace filemanager::archive_backend::detail {
  [[noreturn]] void systemError(const char* operation) {
    const int code = errno;
    throwFileError(
        code == ENOSPC || code == EDQUOT ? FileErrorKind::NoSpace : FileErrorKind::Other,
        std::string(operation) + ": " + std::strerror(code)
    );
  }
  Fd opened(int fd) {
    if (fd < 0) {
      systemError("Could not open archive item");
    }
    return Fd(fd);
  }
  struct stat statAt(int fd, const std::string& name) {
    struct stat value{};
    if (::fstatat(fd, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
      systemError("Could not inspect archive source");
    }
    return value;
  }
  bool sameRevision(const struct stat& a, const struct stat& b) {
    // NFS can refresh ctime after a write has closed, even when the archive bytes are unchanged.
    return sameIdentity(a, b)
        && a.st_mode == b.st_mode
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
  }

  std::uint64_t availableSpace(int folder) {
    struct statvfs info{};
    if (::fstatvfs(folder, &info) != 0)
      systemError("Could not inspect available archive storage");
    const auto block = info.f_frsize ? info.f_frsize : info.f_bsize;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return !block || info.f_bavail > maximum / block ? maximum : info.f_bavail * block;
  }
  std::string bounded(const char* text) {
    if (!text) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive entry has no pathname.");
    }
    const auto size = ::strnlen(text, 4097);
    if (size > 4096) {
      throwFileError(FileErrorKind::NameTooLong, "Archive pathname is too long.");
    }
    return {text, size};
  }
  std::vector<std::string> parts(const std::string& name) {
    if (absolute(name) || name.contains('\\')) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive contains an absolute or nonportable pathname.");
    }
    std::vector<std::string> result;
    for (std::size_t start = 0; start < name.size();) {
      const auto end = name.find('/', start);
      auto component = name.substr(start, end == std::string::npos ? end : end - start);
      if (component == "..") {
        throwFileError(FileErrorKind::InvalidArgument, "Archive contains a parent-path traversal.");
      }
      if (!component.empty() && component != ".") {
        result.push_back(std::move(component));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return result;
  }
  std::string join(const std::vector<std::string>& components) {
    std::string result;
    for (const auto& component : components) {
      if (!result.empty()) {
        result += '/';
      }
      result += component;
    }
    return result;
  }
  std::string safePath(const std::string& name) { return join(parts(name)); }

  void checkLinkTarget(const std::string& path, const std::string& target) {
    if (target.empty() || absolute(target) || target.contains('\\')) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes the extraction folder.");
    }
    auto resolved = parts(path);
    resolved.pop_back();
    for (std::size_t start = 0; start < target.size();) {
      const auto end = target.find('/', start);
      const auto component = target.substr(start, end == std::string::npos ? end : end - start);
      if (component == "..") {
        if (resolved.empty()) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes the extraction folder.");
        }
        resolved.pop_back();
      } else if (!component.empty() && component != ".") {
        resolved.push_back(component);
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
  }

  void validateLinks(const std::map<std::string, std::string>& links, Work& work) {
    for (const auto& [path, target] : links) {
      auto resolved = parts(path);
      resolved.pop_back();
      std::deque<std::string> remaining;
      const auto append = [&](const std::string& text) {
        if (absolute(text) || text.contains('\\')) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes the extraction folder.");
        }
        std::vector<std::string> tokens;
        for (std::size_t start = 0; start < text.size();) {
          const auto end = text.find('/', start);
          auto token = text.substr(start, end == std::string::npos ? end : end - start);
          if (!token.empty() && token != ".") {
            tokens.push_back(std::move(token));
          }
          if (end == std::string::npos) {
            break;
          }
          start = end + 1;
        }
        for (const auto& token : std::views::reverse(tokens)) {
          remaining.push_front(token);
        }
      };
      append(target);
      unsigned hops = 0;
      while (!remaining.empty()) {
        work.check();
        auto token = std::move(remaining.front());
        remaining.pop_front();
        if (token == "..") {
          if (resolved.empty()) {
            throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes through another link.");
          }
          resolved.pop_back();
          continue;
        }
        resolved.push_back(std::move(token));
        if (const auto found = links.find(join(resolved)); found != links.end()) {
          if (++hops > 40) {
            throwFileError(FileErrorKind::InvalidArgument, "Archive has a cyclic link chain.");
          }
          resolved.pop_back();
          append(found->second);
        }
      }
    }
  }
  std::string leaf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }
  std::string parentName(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
  }
  [[noreturn]] void archiveError(struct archive* value, Work& work) {
    if (work.failure) {
      std::rethrow_exception(work.failure);
    }
    throwFileError(
        FileErrorKind::Other, archive_error_string(value) ? archive_error_string(value) : "Archive backend failed."
    );
  }
  void requireArchive(int status, struct archive* value, Work& work) {
    if (status != ARCHIVE_OK) {
      archiveError(value, work);
    }
  }

  Fd::~Fd() { close(); }
  int Fd::close() { return m_fd < 0 ? 0 : ::close(std::exchange(m_fd, -1)); }
  Fd& Fd::operator=(Fd&& other) noexcept {
    close();
    m_fd = std::exchange(other.m_fd, -1);
    return *this;
  }

  ArchiveLocale::ArchiveLocale() {
    m_locale = ::newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
    if (!m_locale)
      throwFileError(FileErrorKind::Unsupported, "A UTF-8 archive locale is unavailable.");
    m_previous = ::uselocale(m_locale);
  }

  ArchiveLocale::~ArchiveLocale() {
    ::uselocale(m_previous);
    ::freelocale(m_locale);
  }

  struct stat statFd(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0)
      systemError("Could not inspect an open archive item");
    return value;
  }

  bool sameIdentity(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
  }

  Fd openDirectory(const std::filesystem::path& path) {
    if (!path.is_absolute() || path.native().contains('\0'))
      throwFileError(FileErrorKind::InvalidArgument, "Archive operations require absolute native paths.");
    return opened(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  }

  Source openSource(const std::filesystem::path& path) {
    auto parent = openDirectory(path.parent_path());
    const auto name = path.filename().native();
    if (name.empty() || name == "." || name == ".." || name.contains('\0'))
      throwFileError(FileErrorKind::InvalidArgument, "Invalid archive input name.");
    auto file = opened(::openat(parent.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    const auto revision = statFd(file.get());
    if (!S_ISREG(revision.st_mode) || revision.st_size <= 0)
      throwFileError(FileErrorKind::Unsupported, "Archive input must be a nonempty regular file.");
    return {std::move(parent), std::move(file), name, revision};
  }

  std::size_t Input::read(std::span<char> buffer) {
    m_work.check();
    const auto length = std::min(buffer.size(), static_cast<std::size_t>(m_source.revision.st_size - m_position));
    std::size_t total = 0;
    while (total < length) {
      const auto count = ::pread(m_source.file.get(), buffer.data() + total, length - total, m_position);
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        systemError("Could not read the archive");
      if (count == 0)
        throwFileError(FileErrorKind::Changed, "Archive input ended prematurely.");
      total += static_cast<std::size_t>(count);
      m_position += count;
    }
    return total;
  }

  std::int64_t Input::seek(std::int64_t offset, int whence) {
    const auto size = m_source.revision.st_size;
    const auto base = whence == SEEK_END ? size : whence == SEEK_CUR ? m_position : 0;
    if ((whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) || offset < -base || offset > size - base)
      throwFileError(FileErrorKind::InvalidArgument, "Archive seeks outside the input.");
    return m_position = base + offset;
  }

  void Input::unchanged() const {
    m_work.check();
    if (!sameRevision(statFd(m_source.file.get()), m_source.revision)
        || !sameRevision(statAt(m_source.parent.get(), m_source.name), m_source.revision))
      throwFileError(FileErrorKind::Changed, "Archive input changed while reading.");
  }

  void Work::check() const {
    if (callbacks.checkpoint)
      callbacks.checkpoint();
    if (cancellable && g_cancellable_is_cancelled(cancellable))
      throwFileError(FileErrorKind::Cancelled, "Archive operation cancelled.");
  }

  void Work::item() {
    check();
    ++count;
    report(0);
  }

  void Work::report(std::uint64_t amount) {
    bytes += std::min(amount, std::numeric_limits<std::uint64_t>::max() - bytes);
    if (callbacks.progress && std::chrono::steady_clock::now() >= nextProgress) {
      callbacks.progress(bytes, count);
      nextProgress = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
  }

  Stage::Stage(const fs::path& parent) {
    auto pattern = (parent / ".nythera-archive-XXXXXX").native();
    if (!::mkdtemp(pattern.data()))
      systemError("Could not create private archive staging");
    path = std::move(pattern);
    try {
      directory = opened(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    } catch (...) {
      cleanup();
      throw;
    }
  }

  Stage::~Stage() { cleanup(); }

  bool Stage::cleanup() noexcept {
    if (path.empty())
      return true;
    directory.close();
    std::error_code error;
    fs::remove_all(path, error);
    if (error)
      return false;
    path.clear();
    return true;
  }
} // namespace filemanager::archive_backend::detail
