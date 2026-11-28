// Adapted from Luciole's archive_support.h and archive_work.h; see the project LICENSE.
#pragma once

#include "archive_backend.h"

#include <chrono>
#include <clocale>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <utility>

struct archive;

namespace filemanager::archive_backend::detail {
  enum class FileErrorKind {
    Other,
    Cancelled,
    Unsupported,
    InvalidArgument,
    NameTooLong,
    NoSpace,
    Changed,
    EncryptedSevenZip
  };
  struct Error : std::runtime_error {
    FileErrorKind kind;
    Error(FileErrorKind reason, std::string message) : std::runtime_error(std::move(message)), kind(reason) {}
  };
  [[noreturn]] inline void throwFileError(FileErrorKind reason, std::string message) {
    throw Error(reason, std::move(message));
  }

  class Fd {
  public:
    explicit Fd(int fd = -1) : m_fd(fd) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Fd& operator=(Fd&& other) noexcept;
    int get() const { return m_fd; }
    int close();

  private:
    int m_fd;
  };

  class ArchiveLocale {
  public:
    ArchiveLocale();
    ~ArchiveLocale();
    ArchiveLocale(const ArchiveLocale&) = delete;
    ArchiveLocale& operator=(const ArchiveLocale&) = delete;

  private:
    locale_t m_locale{}, m_previous{};
  };

  struct Work {
    GCancellable* cancellable;
    const Callbacks& callbacks;
    std::exception_ptr failure;
    std::uint64_t bytes = 0;
    std::size_t count = 0;
    std::chrono::steady_clock::time_point nextProgress{};
    void check() const;
    void item();
    void report(std::uint64_t amount);
  };

  struct Stage {
    std::filesystem::path path;
    Fd directory;
    explicit Stage(const std::filesystem::path& parent);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    bool cleanup() noexcept;
  };

  using Entries = std::map<std::string, Entry, std::less<>>;
  using Projection = std::map<std::string, std::string, std::less<>>;
  struct Source {
    Fd parent, file;
    std::string name;
    struct stat revision;
  };

  Fd opened(int fd);
  Fd openDirectory(const std::filesystem::path& path);
  Source openSource(const std::filesystem::path& path);
  struct stat statFd(int fd);
  struct stat statAt(int fd, const std::string& name);
  bool sameIdentity(const struct stat& a, const struct stat& b);
  bool sameRevision(const struct stat& a, const struct stat& b);
  std::uint64_t availableSpace(int folder);
  [[noreturn]] void systemError(const char* operation);
  std::string bounded(const char* text);
  std::vector<std::string> parts(const std::string& name);
  std::string safePath(const std::string& name);
  void checkLinkTarget(const std::string& path, const std::string& target);
  void validateLinks(const std::map<std::string, std::string>& links, Work& work);
  std::string leaf(const std::string& path);
  std::string parentName(const std::string& path);
  [[noreturn]] void archiveError(struct archive* value, Work& work);
  void requireArchive(int status, struct archive* value, Work& work);

  int setFormat(struct archive* writer, Format format, Level level, Method method = Method::Default);
  int addFilter(struct archive* writer, Format format, Level level);
  bool nativeFilter(Format format);
  void writeArchive(
      std::span<const std::filesystem::path> sources, Stage& stage, Format format, Level level, Work& work,
      const EncryptionOptions& options
  );
  void writeEncryptedSevenZip(
      std::span<const std::filesystem::path> sources, Stage& stage, Level level, const EncryptionOptions& options,
      Work& work
  );
  void readEncryptedSevenZip(const Source& source, Stage& stage, Work& work);
  Entries inspectEncryptedSevenZip(const Source& source, Stage& stage, Work& work);
  void readSelectedSevenZip(
      const Source& source, const Catalogue& catalogue, const Projection& items, Stage& stage, Work& work
  );
  Entries inspectArchive(const Source& source, Work& work);
  void
  readSelected(const Source& source, const Catalogue& catalogue, const Projection& items, Stage& stage, Work& work);
  void readArchive(const std::filesystem::path& path, const Source& source, Stage& stage, Work& work);
} // namespace filemanager::archive_backend::detail
