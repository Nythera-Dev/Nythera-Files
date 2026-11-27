#include "filemanager/search_content.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  constexpr std::uint64_t kExtractionInputLimit = 128 * 1024 * 1024;
  constexpr std::uint64_t kLargePlainThreshold = 128 * 1024 * 1024;
  constexpr off_t kExtractionOutputLimit = 512 * 1024 * 1024;
  constexpr std::size_t kMaxCapturedBytes = 64 * 1024;
  constexpr auto kDeadline = 20s;

  struct Fd {
    int value = -1;
    ~Fd() {
      if (value >= 0)
        close(value);
    }
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset() {
      if (value >= 0)
        close(std::exchange(value, -1));
    }
  };

  struct Pipe {
    Fd read;
    Fd write;
    bool open() {
      int fds[2];
      if (pipe2(fds, O_CLOEXEC) != 0)
        return false;
      read.value = fds[0];
      write.value = fds[1];
      return fcntl(read.value, F_SETFL, O_NONBLOCK) == 0;
    }
  };

  struct ProcessResult {
    std::string output;
    std::string errors;
    int status = -1;
    bool limited = false;
    bool timedOut = false;
    [[nodiscard]] bool ok() const { return !limited && !timedOut && (status == 0 || status == 1); }
  };

  // The fork child only performs async-signal-safe setup before exec. Keep the
  // leader unreaped until the whole process group has been signalled.
  ProcessResult
  run(const std::vector<std::string>& arguments, GCancellable* cancel, bool rga = false,
      const std::function<bool(std::string_view)>& consume = {}) {
    ProcessResult result;
    if (g_cancellable_is_cancelled(cancel))
      return result;
    Pipe output, errors;
    Fd input(rga ? memfd_create("nythera-rga-defaults", MFD_CLOEXEC) : open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (input.value < 0
        || !output.open()
        || !errors.open()
        || (rga && (write(input.value, "{}", 2) != 2 || lseek(input.value, 0, SEEK_SET) < 0)))
      return result;

    std::vector<char*> argv;
    for (const auto& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    g_auto(GStrv) env = g_get_environ();
    for (const auto* variable : {"RGA_CONFIG", "RIPGREP_CONFIG_PATH", "RUST_LOG", "RUST_BACKTRACE"})
      env = g_environ_unsetenv(env, variable);
    env = g_environ_setenv(env, "TOKIO_WORKER_THREADS", "1", true);
    env = g_environ_setenv(env, "LC_ALL", "C", true);
    // Register before spawning so cancellation can signal the whole group
    // before shutdown returns, even if the worker has not woken from poll.
    struct ChildGroup {
      std::mutex spawn;
      pid_t pid = -1;
    } group;
    const auto killOnCancel = g_cancellable_connect(
        cancel, G_CALLBACK(+[](GCancellable*, gpointer data) {
          auto& child = *static_cast<ChildGroup*>(data);
          const std::lock_guard lock(child.spawn);
          if (child.pid > 0) {
            kill(-child.pid, SIGKILL);
            kill(child.pid, SIGKILL);
          }
        }),
        &group, nullptr
    );
    std::unique_lock spawnLock(group.spawn);
    const auto pid = g_cancellable_is_cancelled(cancel) ? -1 : fork();
    if (pid < 0) {
      spawnLock.unlock();
      g_cancellable_disconnect(cancel, killOnCancel);
      return result;
    }
    if (pid == 0) {
      constexpr rlimit memory{768 * 1024 * 1024, 768 * 1024 * 1024};
      constexpr rlimit fileSize{512 * 1024 * 1024, 512 * 1024 * 1024};
      constexpr rlimit cpu{15, 16};
      constexpr rlimit noCore{0, 0};
      if (setpgid(0, 0) != 0
          || dup2(input.value, STDIN_FILENO) < 0
          || dup2(output.write.value, STDOUT_FILENO) < 0
          || dup2(errors.write.value, STDERR_FILENO) < 0
          || setrlimit(RLIMIT_AS, &memory) != 0
          || setrlimit(RLIMIT_CPU, &cpu) != 0
          || setrlimit(RLIMIT_FSIZE, &fileSize) != 0
          || setrlimit(RLIMIT_CORE, &noCore) != 0)
        _exit(126);
      // Avoid retaining unrelated descriptors across exec even if another
      // subsystem acquired one without CLOEXEC.
      syscall(SYS_close_range, 3U, ~0U, 0U);
      execve(argv[0], argv.data(), env);
      _exit(127);
    }

    setpgid(pid, pid);
    group.pid = pid;
    spawnLock.unlock();
    output.write.reset();
    errors.write.reset();
    Fd child(static_cast<int>(syscall(SYS_pidfd_open, pid, 0U)));
    GPollFD cancellation{};
    const bool watchingCancellation = g_cancellable_make_pollfd(cancel, &cancellation);
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    const auto read = [&](Fd& fd, std::string& captured, bool stream) {
      if (fd.value < 0)
        return;
      std::array<char, 4096> buffer;
      for (int chunk = 0; chunk < 16; ++chunk) {
        const auto length = ::read(fd.value, buffer.data(), buffer.size());
        if (length < 0 && errno == EINTR)
          continue;
        if (length == 0 || (length < 0 && errno != EAGAIN))
          fd.reset();
        if (length <= 0)
          return;
        if (stream) {
          if (!(consume(std::string_view(buffer.data(), static_cast<std::size_t>(length))))) {
            result.limited = true;
            return;
          }
          continue;
        }
        if (captured.size() + static_cast<std::size_t>(length) > kMaxCapturedBytes) {
          result.limited = true;
          return;
        }
        captured.append(buffer.data(), static_cast<std::size_t>(length));
      }
    };

    bool exited = false;
    while (child.value >= 0 && watchingCancellation && !g_cancellable_is_cancelled(cancel) && !result.limited) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining <= 0ms) {
        result.timedOut = true;
        break;
      }
      std::array<pollfd, 4> events{
          {{child.value, POLLIN, 0},
           {cancellation.fd, POLLIN, 0},
           {output.read.value, POLLIN, 0},
           {errors.read.value, POLLIN, 0}}
      };
      const int ready = poll(events.data(), events.size(), static_cast<int>(remaining.count()));
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0 || events[1].revents)
        break;
      if (events[2].revents)
        read(output.read, result.output, bool(consume));
      if (events[3].revents)
        read(errors.read, result.errors, false);
      if (events[0].revents & POLLIN) {
        exited = true;
        break;
      }
    }
    if (watchingCancellation)
      g_cancellable_release_fd(cancel);
    kill(-pid, SIGKILL);
    if (!exited)
      kill(pid, SIGKILL);
    g_cancellable_disconnect(cancel, killOnCancel);
    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (exited && waited == pid) {
      if (WIFEXITED(status))
        result.status = WEXITSTATUS(status);
      else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGXCPU)
        result.timedOut = true;
    }
    // A bounded plain batch can still emit complete, attributable records before
    // a timeout. Drain its pipe after stopping the group, even on that path.
    if (!result.limited && !g_cancellable_is_cancelled(cancel)) {
      read(output.read, result.output, bool(consume));
      if (!result.limited)
        read(errors.read, result.errors, false);
    }
    return result;
  }

  std::string executable(const char* name) {
    g_autofree char* found = g_find_program_in_path(name);
    return found ? found : "";
  }

  std::string digest(std::string_view data) {
    g_autofree char* value =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(data.data()), data.size());
    return value;
  }

  std::string fdPath(int fd) { return "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(fd); }

  struct AliasDirectory {
    fs::path path;
    Fd lock;
    std::vector<std::string> names;

    ~AliasDirectory() {
      for (const auto& name : names)
        unlinkat(lock.value, name.c_str(), 0);
    }

    bool open(const fs::path& cache, const fs::path& source, GCancellable* cancel) {
      // Shard by source file so different inputs can extract concurrently.
      // The lease also coordinates multiple Files instances using the same alias.
      path = cache.parent_path() / "inputs-v2" / digest(source.native()).substr(0, 2);
      if (g_mkdir_with_parents(path.c_str(), 0700) != 0)
        return false;
      lock.value = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      struct stat info{};
      if (lock.value < 0 || fstat(lock.value, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0077) != 0)
        return false;
      while (!g_cancellable_is_cancelled(cancel)) {
        if (flock(lock.value, LOCK_EX | LOCK_NB) == 0)
          return true;
        if (errno != EWOULDBLOCK && errno != EINTR)
          break;
        g_usleep(10'000);
      }
      return false;
    }

    std::string
    add(const filemanager::ContentCandidate& candidate, const struct stat& info, std::string_view extension) {
      std::string key = candidate.entry.absPath.native();
      key += '\0';
      key += std::to_string(info.st_dev)
          + ":"
          + std::to_string(info.st_ino)
          + ":"
          + std::to_string(info.st_ctim.tv_sec)
          + ":"
          + std::to_string(info.st_ctim.tv_nsec)
          + ":"
          + std::to_string(info.st_mtim.tv_sec)
          + ":"
          + std::to_string(info.st_mtim.tv_nsec)
          + ":"
          + std::to_string(info.st_size);
      const auto name = digest(key) + std::string(extension);
      // A previous crash can leave an alias in the shard. Replace it only while
      // holding the directory lease and only if it is a symlink.
      struct stat old{};
      if (fstatat(lock.value, name.c_str(), &old, AT_SYMLINK_NOFOLLOW) == 0
          && (!S_ISLNK(old.st_mode) || unlinkat(lock.value, name.c_str(), 0) != 0))
        return {};
      if (symlinkat(fdPath(candidate.fd).c_str(), lock.value, name.c_str()) != 0)
        return {};
      names.push_back(name);
      return (path / name).native();
    }
  };

  bool sameRevision(int fd, const struct stat& before) {
    struct stat after{};
    return fstat(fd, &after) == 0
        && before.st_dev == after.st_dev
        && before.st_ino == after.st_ino
        && before.st_size == after.st_size
        && before.st_ctim.tv_sec == after.st_ctim.tv_sec
        && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec
        && before.st_mtim.tv_sec == after.st_mtim.tv_sec
        && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
  }

  // Retain the full preprocessor stream for ripgrep, except for known generated
  // records. Spool it on disk so long lines and large extractions stay bounded.
  class ExtractedText {
  public:
    explicit ExtractedText(const fs::path& directory, bool media, bool archive)
        : m_media(media), m_archive(archive), m_file(open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600)) {
      if (m_file.value < 0) {
        g_autofree char* name = nullptr;
        m_file.value = g_file_open_tmp("nythera-extraction-XXXXXX", &name, nullptr);
        if (m_file.value >= 0 && unlink(name) != 0)
          m_file.reset();
      }
    }

    [[nodiscard]] int fd() const { return m_file.value; }
    [[nodiscard]] bool truncated() const { return m_truncated; }
    [[nodiscard]] bool hasUnsupportedMember() const { return m_unsupported; }

    bool append(std::string_view chunk) {
      m_received += static_cast<off_t>(chunk.size());
      if (m_received > kExtractionOutputLimit)
        return false;
      while (!chunk.empty()) {
        const auto newline = chunk.find('\n');
        const auto line = chunk.substr(0, newline);
        m_head.append(line.substr(0, kMarkerBytes - m_head.size()));
        if (line.size() >= kMarkerBytes)
          m_tail.assign(line.substr(line.size() - kMarkerBytes));
        else {
          m_tail.append(line);
          if (m_tail.size() > kMarkerBytes)
            m_tail.erase(0, m_tail.size() - kMarkerBytes);
        }
        if (!writeText(line))
          return false;
        if (newline == chunk.npos)
          break;
        if (!finishLine(true))
          return false;
        chunk.remove_prefix(newline + 1);
      }
      return true;
    }

    bool finish() { return m_head.empty() || finishLine(false); }

  private:
    bool writeText(std::string_view text) {
      while (!text.empty()) {
        const auto count = write(m_file.value, text.data(), text.size());
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          return false;
        m_written += count;
        text.remove_prefix(static_cast<std::size_t>(count));
      }
      return true;
    }

    bool finishLine(bool newline) {
      const auto marker = [this](std::string_view text) {
        return m_head == text
            || (m_tail.ends_with(text)
                && m_tail.size() >= text.size() + 2
                && std::string_view(m_tail).substr(m_tail.size() - text.size() - 2, 2) == ": ");
      };
      const bool depth = m_archive && marker("[rga: max archive recursion reached (3)]");
      const bool unsupported = m_archive
          && (marker("[rga: skipping video in archive]")
              || marker("[rga: skipping sqlite in archive]")
              || marker("[rga: binary data]"));
      m_truncated |= depth;
      m_unsupported |= unsupported;
      const bool discard = depth
          || unsupported
          || (m_media
              && (m_head.starts_with("metadata: format.filename=")
                  || m_head.starts_with("metadata: error.")
                  || m_head == ": WEBVTT"));
      if (discard) {
        if (ftruncate(m_file.value, m_lineStart) != 0 || lseek(m_file.value, m_lineStart, SEEK_SET) < 0)
          return false;
        m_written = m_lineStart;
      } else if (newline && !writeText("\n")) {
        return false;
      }
      m_lineStart = m_written;
      m_head.clear();
      m_tail.clear();
      return true;
    }

    static constexpr std::size_t kMarkerBytes = 96;
    bool m_media;
    bool m_archive;
    Fd m_file;
    off_t m_received = 0;
    off_t m_written = 0;
    off_t m_lineStart = 0;
    std::string m_head;
    std::string m_tail;
    bool m_truncated = false;
    bool m_unsupported = false;
  };

  enum class TextProbe { Text, Binary, Failed };

  TextProbe probeText(const std::string& path) {
    Fd fd(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    std::array<char, 8192> prefix;
    const auto length = fd.value < 0 ? -1 : pread(fd.value, prefix.data(), prefix.size(), 0);
    if (length < 0)
      return TextProbe::Failed;
    const std::string_view data(prefix.data(), static_cast<std::size_t>(length));
    // Leave UTF-16 BOM decoding to ripgrep.
    return !data.contains('\0') || data.starts_with("\xff\xfe") || data.starts_with("\xfe\xff") ? TextProbe::Text
                                                                                                : TextProbe::Binary;
  }

  std::vector<std::string> patternArgs(const filemanager::ContentFilter& filter) {
    std::vector<std::string> args{
        "--no-config", "--no-mmap", "--color=never", "--threads=1",
        filter.caseSensitive ? "--case-sensitive" : "--ignore-case"
    };
    args.emplace_back(filter.regex ? "--engine=auto" : "--fixed-strings");
    // --regexp= keeps a leading dash (including rga's own switches) in the pattern.
    args.push_back("--regexp=" + filter.query);
    return args;
  }
} // namespace

namespace filemanager {

  void ContentIssues::merge(const ContentIssues& other) {
    unsupported += other.unsupported;
    sizeRestricted += other.sizeRestricted;
    inaccessible += other.inaccessible;
    extractionFailed += other.extractionFailed;
    archiveTruncated += other.archiveTruncated;
    extractionTimeouts += other.extractionTimeouts;
    plainTimeouts += other.plainTimeouts;
    backendError |= other.backendError;
  }

  bool ContentIssues::partial() const {
    return sizeRestricted
        || inaccessible
        || extractionFailed
        || archiveTruncated
        || extractionTimeouts
        || plainTimeouts
        || backendError;
  }

  bool ContentBackends::supports(ContentMode mode) const {
    return !rg.empty() && (mode != ContentMode::Extracted || !rga.empty());
  }

  ContentBackends findContentBackends() {
    ContentBackends backends{executable("rg"), executable("rga")};
    if (!backends.rga.empty()) {
      std::error_code error;
      const auto path = fs::canonical(backends.rga, error);
      if (error || access((path.parent_path() / "rga-preproc").c_str(), X_OK) != 0)
        backends.rga.clear();
    }
    return backends;
  }

  fs::path searchExtractionCache() {
    return fs::path(g_get_user_cache_dir()) / "nythera-files" / "search-content" / "ripgrep-all-v2";
  }

  ContentCandidate::ContentCandidate(FileEntry result, int descriptor) : entry(std::move(result)), fd(descriptor) {}
  ContentCandidate::~ContentCandidate() {
    if (fd >= 0)
      close(fd);
  }
  ContentCandidate::ContentCandidate(ContentCandidate&& other) noexcept
      : entry(std::move(other.entry)), fd(std::exchange(other.fd, -1)) {}

  ContentSearch::ContentSearch(ContentFilter filter, ContentBackends backends)
      : m_filter(std::move(filter)), m_backends(std::move(backends)), m_cache(searchExtractionCache()) {}

  std::future<ContentResult> ContentSearch::queueExtraction(
      const std::shared_ptr<ContentSearch>& search, ContentCandidate candidate, GCancellable* cancel
  ) {
    using Job = std::packaged_task<ContentResult()>;
    static GThreadPool* pool = g_thread_pool_new(
        [](gpointer data, gpointer) {
          const std::unique_ptr<Job> job(static_cast<Job*>(data));
          (*job)();
        },
        nullptr, 4, false, nullptr
    );
    auto job = std::make_unique<Job>(
        [search, candidate = std::move(candidate),
         cancellation = std::shared_ptr<GCancellable>(G_CANCELLABLE(g_object_ref(cancel)), g_object_unref)]() mutable {
          return search->match(std::span(&candidate, 1), cancellation.get());
        }
    );
    auto future = job->get_future();
    if (pool)
      g_thread_pool_push(pool, job.release(), nullptr);
    else
      (*job)();
    return future;
  }

  void ContentSearch::prepare(GCancellable* cancel) {
    if (!m_backends.supports(m_filter.mode)) {
      m_invalid = "Content search dependencies are unavailable";
      return;
    }
    if (m_filter.regex) {
      auto args = patternArgs(m_filter);
      args.insert(args.begin(), m_backends.rg);
      args.emplace_back("-");
      const auto check = run(args, cancel);
      if (!check.ok()) {
        m_invalid = check.errors.contains("regex parse error") ? "Invalid content regular expression"
                                                               : "Could not validate the content regular expression";
        return;
      }
    }
    if (m_backends.rga.empty())
      return;

    std::error_code error;
    const auto rga = fs::canonical(m_backends.rga, error);
    if (!error)
      m_preprocessor = (rga.parent_path() / "rga-preproc").native();
    const auto list = run({m_backends.rga, "--rga-config-file=/proc/self/fd/0", "--rga-list-adapters"}, cancel, true);
    if (!list.ok() || !list.output.starts_with("Adapters:"))
      return;

    std::string name;
    bool available = false;
    bool enabled = true;
    std::istringstream lines(list.output);
    for (std::string line; std::getline(lines, line);) {
      const auto first = line.find_first_not_of(' ');
      if (first == line.npos)
        continue;
      std::string_view field(line);
      field.remove_prefix(first);
      if (field.starts_with("The following adapters are disabled"))
        enabled = false;
      if (field.starts_with("- **") && field.ends_with("**")) {
        name = field.substr(4, field.size() - 6);
        available = enabled;
        if (name == "ffmpeg") {
          for (const auto* tool : {"ffprobe", "ffmpeg"}) {
            if (enabled && executable(tool).empty()) {
              available = false;
              m_missing.emplace_back(name, tool);
            }
          }
        }
      } else if (field.starts_with("Runs: ") && enabled) {
        const auto invocation = field.substr(6);
        const std::string command(invocation.substr(0, invocation.find(' ')));
        if (executable(command.c_str()).empty()) {
          available = false;
          m_missing.emplace_back(name, command);
        }
      } else if (field.starts_with("Extensions: ") && name != "postprocpagebreaks") {
        std::istringstream extensions(std::string(field.substr(12)));
        for (std::string extension; extensions >> extension;) {
          if (extension.ends_with(','))
            extension.pop_back();
          m_adapters.try_emplace(std::move(extension), Adapter{name, enabled, available});
        }
      }
    }
    m_rgaReady = enabled == false && !m_adapters.empty();
  }

  const ContentSearch::Adapter* ContentSearch::adapterFor(const FileEntry& entry) const {
    auto extension = entry.absPath.extension().native();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
    const auto adapter = m_adapters.find(extension);
    return adapter == m_adapters.end() ? nullptr : &adapter->second;
  }

  ContentLane ContentSearch::lane(const FileEntry& entry, GCancellable* cancel) {
    std::call_once(m_prepare, [&] { prepare(cancel); });
    if (adapterFor(entry))
      return ContentLane::Extraction;
    return entry.size > kLargePlainThreshold ? ContentLane::LargePlain : ContentLane::Plain;
  }

  std::string ContentSearch::notice() const {
    if (!m_invalid.empty())
      return m_invalid;
    if (m_filter.mode != ContentMode::Plain) {
      if (!m_rgaReady)
        return m_backends.rga.empty() ? "Extracted text unavailable (requires ripgrep-all)"
                                      : "Could not read ripgrep-all adapter capabilities";
      if (!m_missing.empty()) {
        std::string message = "Unavailable extraction adapters";
        for (const auto& [name, command] : m_missing)
          message += " · " + name + " (requires " + command + ")";
        return message;
      }
    }
    return {};
  }

  ContentResult ContentSearch::match(std::span<ContentCandidate> candidates, GCancellable* cancel) {
    ContentResult result;
    if (g_cancellable_is_cancelled(cancel))
      return result;
    std::call_once(m_prepare, [&] { prepare(cancel); });
    auto& issues = result.issues;
    if (!m_invalid.empty() || (m_filter.mode == ContentMode::Extracted && !m_rgaReady)) {
      issues.backendError = true;
      return result;
    }

    auto plainArgs = patternArgs(m_filter);
    plainArgs.insert(plainArgs.begin(), m_backends.rg);
    plainArgs.insert(plainArgs.end(), {"--files-with-matches", "--null", "--with-filename", "--"});

    struct Input {
      ContentCandidate* candidate;
      struct stat state;
      bool inaccessible = false;
    };
    std::map<std::string, Input> plain;
    const auto searchPlain = [&] {
      if (plain.empty() || g_cancellable_is_cancelled(cancel))
        return;
      auto args = plainArgs;
      for (const auto& [path, input] : plain)
        args.push_back(path);
      const auto process = run(args, cancel);
      if (g_cancellable_is_cancelled(cancel))
        return;
      issues.plainTimeouts += process.timedOut;
      bool explained = false;
      std::istringstream diagnostics(process.errors);
      for (std::string line; std::getline(diagnostics, line);) {
        const bool accessError = line.starts_with("rg: ")
            && (line.ends_with("Permission denied (os error 13)")
                || line.ends_with("Operation not permitted (os error 1)")
                || line.ends_with("No such file or directory (os error 2)"));
        const auto separator = line.find(": ", 4);
        const auto found =
            accessError && separator != line.npos ? plain.find(line.substr(4, separator - 4)) : plain.end();
        if (found == plain.end()) {
          issues.backendError = true;
        } else if (!std::exchange(found->second.inaccessible, true)) {
          ++issues.inaccessible;
          explained = true;
        }
      }
      if (!process.ok() && !process.timedOut && !explained)
        issues.backendError = true;
      std::string_view output(process.output);
      while (!output.empty()) {
        const auto separator = output.find('\0');
        if (separator == output.npos) {
          issues.backendError |= !process.timedOut;
          break;
        }
        const auto found = plain.find(std::string(output.substr(0, separator)));
        if (found == plain.end()) {
          issues.backendError = true;
        } else {
          const auto& input = found->second;
          if (!input.inaccessible) {
            if (sameRevision(input.candidate->fd, input.state))
              result.entries.push_back(std::move(input.candidate->entry));
            else
              ++issues.inaccessible;
          }
          plain.erase(found);
        }
        output.remove_prefix(separator + 1);
      }
      plain.clear();
    };

    const auto extract = [&](ContentCandidate& candidate, const struct stat& info, const Adapter& adapter) {
      if (!adapter.enabled) {
        ++issues.unsupported;
        return;
      }
      if (static_cast<std::uint64_t>(info.st_size) > kExtractionInputLimit) {
        ++issues.sizeRestricted;
        return;
      }
      if (!m_rgaReady || !adapter.available) {
        ++issues.extractionFailed;
        return;
      }
      const Fd readable(open(fdPath(candidate.fd).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
      if (readable.value < 0) {
        ++issues.inaccessible;
        return;
      }
      AliasDirectory aliases;
      if (!aliases.open(m_cache, candidate.entry.absPath, cancel)) {
        ++issues.extractionFailed;
        return;
      }
      auto extension = candidate.entry.absPath.extension().native();
      std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
      const auto path = aliases.add(candidate, info, extension);
      const bool archive = adapter.name == "tar" || adapter.name == "zip" || adapter.name == "decompress";
      ExtractedText text(aliases.path, adapter.name == "ffmpeg", archive);
      if (path.empty() || text.fd() < 0) {
        ++issues.extractionFailed;
        return;
      }
      std::vector<std::string> args{
          m_preprocessor, "--rga-config-file=/proc/self/fd/0", "--rga-max-archive-recursion=3",
          "--rga-no-prefix-filenames", "--rga-cache-path=" + m_cache.native()
      };
      if (!m_missing.empty()) {
        std::string disabled = "--rga-adapters=-";
        std::string_view previous;
        for (const auto& [name, command] : m_missing) {
          if (name != previous)
            disabled += name + ",";
          previous = name;
        }
        disabled.pop_back();
        args.push_back(std::move(disabled));
      }
      args.push_back(path);
      const auto process = run(args, cancel, true, [&](std::string_view chunk) { return text.append(chunk); });
      if (g_cancellable_is_cancelled(cancel))
        return;
      if (process.timedOut) {
        ++issues.extractionTimeouts;
        return;
      }
      if (process.status != 0 || process.limited || !text.finish()) {
        ++issues.extractionFailed;
        return;
      }
      if (!sameRevision(candidate.fd, info)) {
        ++issues.inaccessible;
        return;
      }
      issues.archiveTruncated += text.truncated();
      issues.unsupported += text.hasUnsupportedMember();

      auto search = plainArgs;
      const auto input = fdPath(text.fd());
      search.push_back(input);
      const auto matches = run(search, cancel);
      if (g_cancellable_is_cancelled(cancel))
        return;
      if (matches.timedOut)
        ++issues.extractionTimeouts;
      else if (!matches.ok() || !matches.errors.empty())
        ++issues.extractionFailed;
      else if (matches.status == 0 && matches.output == input + '\0') {
        if (sameRevision(candidate.fd, info))
          result.entries.push_back(std::move(candidate.entry));
        else
          ++issues.inaccessible;
      } else if (matches.status == 0 || !matches.output.empty())
        ++issues.extractionFailed;
    };

    for (auto& candidate : candidates) {
      if (g_cancellable_is_cancelled(cancel))
        break;
      const auto* adapter = adapterFor(candidate.entry);
      if ((m_filter.mode == ContentMode::Plain && adapter) || (m_filter.mode == ContentMode::Extracted && !adapter))
        continue;
      struct stat info{};
      if (fstat(candidate.fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ++issues.inaccessible;
        continue;
      }
      if (adapter) {
        extract(candidate, info, *adapter);
        continue;
      }
      auto path = fdPath(candidate.fd);
      switch (probeText(path)) {
      case TextProbe::Text:
        plain.try_emplace(std::move(path), Input{&candidate, info});
        if (static_cast<std::uint64_t>(info.st_size) > kLargePlainThreshold)
          searchPlain();
        break;
      case TextProbe::Binary:
        ++issues.unsupported;
        break;
      case TextProbe::Failed:
        ++issues.inaccessible;
        break;
      }
    }
    searchPlain();
    return result;
  }

} // namespace filemanager
