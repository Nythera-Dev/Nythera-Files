// Adapted from Luciole src/fs/recursive_search.cpp (cb6375a5e046).
// Copyright (c) 2026 noctalia-dev. Distributed under the MIT License in LICENSE.
#include "filemanager/recursive_file_discovery.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
  constexpr const char* kNativeAttributes = "standard::name,standard::type";
  constexpr const char* kProviderAttributes = "standard::name,standard::type,standard::is-hidden,standard::is-symlink";
  constexpr int kBatchSize = 64;
  constexpr int kWorkersPerLane = 2;
  constexpr unsigned int kMaxDepth = 256;
  constexpr std::size_t kMaxPending = 65536;

  bool hasDescendant(const filemanager::SearchPathSet& paths, std::string_view root) {
    if (root == "/")
      return !paths.empty();
    return std::ranges::any_of(paths, [root](const auto& path) {
      return path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
    });
  }

  GFileType entryType(GFileInfo* info) {
    if (g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK))
      return G_FILE_TYPE_SYMBOLIC_LINK;
    return g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_TYPE) ? g_file_info_get_file_type(info)
                                                                           : G_FILE_TYPE_UNKNOWN;
  }

  struct DirectoryIdentity {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint64_t inode;
    bool operator==(const DirectoryIdentity&) const = default;
  };

  struct IdentityHash {
    std::size_t operator()(const DirectoryIdentity& key) const noexcept {
      return std::hash<std::uint64_t>{}(key.inode ^ (std::uint64_t{key.major} << 32U) ^ key.minor);
    }
  };

  std::unordered_set<std::string> readHiddenNames(
      const std::filesystem::path& directory, const filemanager::SearchPlan& plan, GCancellable* cancel,
      int directoryFd = -1
  ) {
    // Apply the native GIO .hidden format once per directory. O_NONBLOCK prevents a special
    // file or a symlink to one from holding up the search while opening the list.
    const auto path = directory / ".hidden";
    constexpr auto flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    int fd = directoryFd < 0 ? -1 : filemanager::openSearchDescendant(directoryFd, ".hidden", flags);
    if (fd < 0 && (directoryFd < 0 || errno == ELOOP || errno == EXDEV)) {
      const filemanager::OpenSearchPath safe(plan, path);
      if (safe.fd() >= 0)
        fd = open(filemanager::OpenSearchPath::fdPath(safe.fd()).c_str(), flags);
    }
    if (fd < 0)
      return {};
    struct stat state{};
    std::string contents;
    if (fstat(fd, &state) == 0 && S_ISREG(state.st_mode)) {
      std::array<char, 4096> buffer;
      while (!g_cancellable_is_cancelled(cancel)) {
        const auto size = read(fd, buffer.data(), buffer.size());
        if (size == -1 && errno == EINTR)
          continue;
        if (size <= 0) {
          if (size < 0)
            contents.clear();
          break;
        }
        contents.append(buffer.data(), static_cast<std::size_t>(size));
      }
    }
    close(fd);

    std::unordered_set<std::string> names;
    g_auto(GStrv) lines = g_strsplit(contents.c_str(), "\n", -1);
    for (auto** line = lines; *line; ++line)
      names.emplace(*line);
    return names;
  }
} // namespace

namespace filemanager {

  int RecursiveFileDiscovery::Source::openFolder(const std::filesystem::path& parent) const {
    if (parent == root)
      return fcntl(rootFd, F_DUPFD_CLOEXEC, 0);
    const auto offset = root == "/" ? 1 : root.native().size() + 1;
    if (!parent.native().starts_with(root.native())
        || parent.native().size() <= offset
        || (root != "/" && parent.native()[offset - 1] != '/'))
      return -1;
    return filemanager::openSearchDescendant(rootFd, parent.c_str() + offset, O_PATH | O_DIRECTORY | O_CLOEXEC);
  }

  struct RecursiveFileDiscovery::Run : std::enable_shared_from_this<Run> {
    GCancellable* cancellable = g_cancellable_new();
    std::shared_ptr<const SearchPlan> plan;
    MakeConsumer consumer;
    Complete complete;
    bool showHidden = false;
    std::deque<SearchRoot> pending;
    SearchPathSet knownRoots;
    std::unordered_set<std::string> activeDevices;
    std::mutex visitedMutex;
    std::unordered_set<DirectoryIdentity, IdentityHash> visited;
    std::array<std::size_t, 2> active{};
    std::size_t skipped = 0;
    std::string error;

    ~Run() { g_object_unref(cancellable); }
    void schedule();
    void addAliases(std::vector<SearchRoot> aliases);
    void finished(const SearchRoot& root, std::string problem, std::size_t folders);
  };

  struct RecursiveFileDiscovery::Scan : std::enable_shared_from_this<Scan> {
    struct Folder {
      std::filesystem::path path;
      unsigned int depth = 0;
    };

    struct WorkResult {
      Publish publish;
      bool finished = false;
      std::vector<SearchRoot> aliases;
    };

    std::shared_ptr<Run> run;
    SearchRoot root;
    GCancellable* cancellable;
    int rootFd = -1;
    int folderFd = -1;
    GFileEnumerator* enumerator = nullptr;
    std::deque<Folder> pending;
    Folder current;
    std::filesystem::path physicalRoot;
    SearchPathSet aliasMountPaths;
    SearchPathSet queuedDirectories;
    std::vector<SearchRoot> aliasMounts;
    std::string childPath;
    std::string physicalChildPath;
    bool publishRoot = false;
    bool pruneChildren = false;
    bool prunePhysicalChildren = false;
    std::unordered_set<std::string> hiddenNames;
    Batch batch;
    Flush flush;
    std::string error;
    std::size_t skipped = 0;
    bool showHidden = false;
    bool native = false;

    Scan(std::shared_ptr<Run> owner, SearchRoot source)
        : run(std::move(owner)), root(std::move(source)), cancellable(run->cancellable), showHidden(run->showHidden) {
      auto consumer = run->consumer();
      batch = std::move(consumer.batch);
      flush = std::move(consumer.flush);
      pending.push_back({root.path});
      if (run->plan->followSymlinks)
        queuedDirectories.insert(root.path.native());
    }

    ~Scan() {
      if (enumerator) {
        // A cancelled task can release its final reference on the UI thread.
        g_file_enumerator_close_async(
            enumerator, G_PRIORITY_DEFAULT_IDLE, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer) {
              g_file_enumerator_close_finish(G_FILE_ENUMERATOR(source), result, nullptr);
            },
            nullptr
        );
        g_object_unref(enumerator);
      }
      if (folderFd >= 0)
        close(folderFd);
      if (rootFd >= 0)
        close(rootFd);
    }

    void reportFailure(const GError* detail) {
      if (g_cancellable_is_cancelled(cancellable))
        return;
      if (current.depth == 0)
        error = detail ? detail->message : "Unable to read the search folder";
      else
        ++skipped;
    }

    bool visibleMount() const {
      if (showHidden || root.visibleFrom.empty())
        return true;
      for (auto path = root.path; path != root.visibleFrom; path = path.parent_path()) {
        if (path.filename().native().starts_with('.')
            || readHiddenNames(path.parent_path(), *run->plan, cancellable).contains(path.filename().native()))
          return false;
      }
      return true;
    }

    void discoverAliasMounts() {
      if (physicalRoot.empty() || physicalRoot == root.path || physicalRoot == "/")
        return;
      const std::string prefix = physicalRoot.native() + '/';
      for (const auto& mount : run->plan->nestedMounts) {
        if (!mount.path.native().starts_with(prefix))
          continue;
        auto path = root.path / mount.path.lexically_relative(physicalRoot);
        if (searchPathExcluded(*run->plan, mount.path) || searchPathExcluded(*run->plan, path))
          continue;
        aliasMountPaths.insert(path.native());
        aliasMounts.push_back({std::move(path), mount.kind == SearchMount::Kind::Network, mount.device, root.path});
      }
    }

    bool openNextFolder() {
      while (!pending.empty() && !g_cancellable_is_cancelled(cancellable) && error.empty()) {
        current = std::move(pending.front());
        pending.pop_front();
        if (folderFd >= 0)
          close(std::exchange(folderFd, -1));
        if (current.depth == 0) {
          if (!visibleMount())
            return false;
          OpenSearchPath resolved(*run->plan, current.path);
          physicalRoot = resolved.resolved();
          rootFd = resolved.release();
          publishRoot = !root.visibleFrom.empty();
          const auto needsPruning = [this](const auto& path) {
            return hasDescendant(run->plan->exclusions, path.native())
                || hasDescendant(run->plan->suppressedMounts, path.native());
          };
          if (rootFd >= 0)
            discoverAliasMounts();
          pruneChildren = !aliasMountPaths.empty() || needsPruning(root.path);
          prunePhysicalChildren = physicalRoot != root.path && needsPruning(physicalRoot);
        }
        const Source source{root.path, rootFd, -1};
        if (rootFd >= 0)
          folderFd = source.openFolder(current.path);
        struct statx identity{};
        if (folderFd < 0
            || statx(folderFd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_INO, &identity) != 0
            || (identity.stx_attributes & STATX_ATTR_AUTOMOUNT) != 0) {
          reportFailure(nullptr);
          continue;
        }
        g_autoptr(GFile) file = g_file_new_for_path(OpenSearchPath::fdPath(folderFd).c_str());
        g_autoptr(GError) detail = nullptr;
        native = g_file_is_native(file);
        enumerator = g_file_enumerate_children(
            file, native ? kNativeAttributes : kProviderAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable,
            &detail
        );
        if (enumerator) {
          if ((identity.stx_mask & STATX_INO) != 0) {
            const DirectoryIdentity key{identity.stx_dev_major, identity.stx_dev_minor, identity.stx_ino};
            bool inserted;
            {
              const std::lock_guard lock(run->visitedMutex);
              inserted = run->visited.insert(key).second;
            }
            if (!inserted) {
              g_file_enumerator_close(enumerator, nullptr, nullptr);
              g_clear_object(&enumerator);
              continue;
            }
          }
          if (pruneChildren) {
            childPath = current.path.native();
            if (childPath != "/")
              childPath += '/';
          }
          if (prunePhysicalChildren) {
            const auto suffix = current.path.lexically_relative(root.path);
            auto physical = suffix == "." ? physicalRoot : physicalRoot / suffix;
            physicalChildPath = physical.native();
            if (physicalChildPath != "/")
              physicalChildPath += '/';
          }
          hiddenNames = native && !showHidden ? readHiddenNames(current.path, *run->plan, cancellable, folderFd)
                                              : std::unordered_set<std::string>{};
          return true;
        }
        reportFailure(detail);
      }
      return false;
    }

    void addCandidate(GFileInfo* info, std::vector<Candidate>& candidates) {
      const char* name = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_STANDARD_NAME);
      if (!name
          || (!showHidden
              && (name[0] == '.'
                  || hiddenNames.contains(name)
                  || g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN))))
        return;
      const auto excluded = [&](std::string& prefix) {
        const auto length = prefix.size();
        prefix += name;
        const bool skip = run->plan->exclusions.contains(prefix)
            || run->plan->suppressedMounts.contains(prefix)
            || aliasMountPaths.contains(prefix);
        prefix.resize(length);
        return skip;
      };
      if ((pruneChildren && excluded(childPath)) || (prunePhysicalChildren && excluded(physicalChildPath)))
        return;
      auto type = entryType(info);
      // Some providers omit the lightweight type. Resolve just those entries and
      // defer the volume-root lost+found rule to GIO when applicable.
      if (type == G_FILE_TYPE_UNKNOWN || (!showHidden && native && std::string_view(name) == "lost+found")) {
        g_autoptr(GFile) child = g_file_enumerator_get_child(enumerator, info);
        g_autoptr(GFileInfo) details =
            g_file_query_info(child, kProviderAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr);
        if (details) {
          if (!showHidden && g_file_info_get_attribute_boolean(details, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN))
            return;
          type = entryType(details);
        }
      }
      candidates.push_back({name, type});
    }

    std::filesystem::path logicalPath(const std::filesystem::path& physical) const {
      const auto suffix = physical.lexically_relative(physicalRoot);
      return suffix == "." ? root.path : root.path / suffix;
    }

    std::filesystem::path resolveDirectoryLink(const Candidate& candidate) const {
      const auto& plan = *run->plan;
      const SearchPathBoundary boundary{
          physicalRoot, rootFd, [&](const std::filesystem::path& path, int parent) {
            const auto logical = logicalPath(path);
            if (g_cancellable_is_cancelled(cancellable)
                || plan.boundaries.contains(path.native())
                || plan.boundaries.contains(logical.native())
                || plan.suppressedMounts.contains(path.native())
                || aliasMountPaths.contains(logical.native())
                || searchPathExcluded(plan, logical))
              return false;
            if (showHidden)
              return true;
            const auto name = path.filename().native();
            if (name.starts_with('.') || readHiddenNames(path.parent_path(), plan, cancellable, parent).contains(name))
              return false;
            if (!native || name == "lost+found") {
              const auto item = OpenSearchPath::fdPath(parent) / name;
              g_autoptr(GFile) file = g_file_new_for_path(item.c_str());
              g_autoptr(GFileInfo) info = g_file_query_info(
                  file, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
              );
              if (info && g_file_info_get_is_hidden(info))
                return false;
            }
            return true;
          }
      };
      const OpenSearchPath target(
          plan, physicalRoot / (current.path / candidate.name).lexically_relative(root.path), &boundary
      );
      struct statx info{};
      if (target.fd() < 0
          || statx(target.fd(), "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_TYPE, &info) != 0
          || !S_ISDIR(info.stx_mode))
        return {};
      // Publish descendants at the resolved path. The link entry itself still has its own name and identity.
      return logicalPath(target.resolved());
    }

    void enqueueFolders(std::span<const Candidate> candidates) {
      if (!run->plan->recursive)
        return;
      for (const auto& candidate : candidates) {
        const bool link = run->plan->followSymlinks && candidate.type == G_FILE_TYPE_SYMBOLIC_LINK;
        if (!link && candidate.type != G_FILE_TYPE_DIRECTORY)
          continue;
        if (current.depth >= kMaxDepth || pending.size() >= kMaxPending) {
          ++skipped;
          continue;
        }
        auto path = link ? resolveDirectoryLink(candidate) : current.path / candidate.name;
        if (path.empty() || run->plan->boundaries.contains(path.native()))
          continue;
        if (run->plan->followSymlinks && !queuedDirectories.insert(path.native()).second)
          continue;
        pending.push_back({std::move(path), current.depth + 1});
      }
    }

    WorkResult readBatch() {
      while (!g_cancellable_is_cancelled(cancellable) && error.empty() && (enumerator || openNextFolder())) {
        if (std::exchange(publishRoot, false)) {
          const Candidate mount{root.path.filename().native(), G_FILE_TYPE_DIRECTORY};
          return {
              batch(root.path.parent_path(), std::span(&mount, 1), {root.path, rootFd, -1}, cancellable), false,
              std::exchange(aliasMounts, {})
          };
        }
        std::vector<Candidate> candidates;
        candidates.reserve(kBatchSize);
        for (int i = 0; i < kBatchSize && !g_cancellable_is_cancelled(cancellable); ++i) {
          g_autoptr(GError) detail = nullptr;
          g_autoptr(GFileInfo) info = g_file_enumerator_next_file(enumerator, cancellable, &detail);
          if (!info) {
            g_file_enumerator_close(enumerator, nullptr, nullptr);
            g_clear_object(&enumerator);
            if (detail)
              reportFailure(detail);
            break;
          }
          addCandidate(info, candidates);
        }
        if (!candidates.empty()) {
          // The GIO batch API previously returned entries in reverse enumeration order.
          std::ranges::reverse(candidates);
          enqueueFolders(candidates);
          return {
              batch(current.path, candidates, {root.path, rootFd, folderFd}, cancellable), false,
              std::exchange(aliasMounts, {})
          };
        }
      }
      return {
          flush && error.empty() && !g_cancellable_is_cancelled(cancellable)
              ? flush({root.path, rootFd, -1}, cancellable)
              : Publish{},
          true, std::exchange(aliasMounts, {})
      };
    }

    static void readTask(gpointer data, gpointer) {
      g_autoptr(GTask) task = G_TASK(data);
      if (g_task_return_error_if_cancelled(task))
        return;
      auto& scan = **static_cast<std::shared_ptr<Scan>*>(g_task_get_task_data(task));
      g_task_return_pointer(task, new WorkResult(scan.readBatch()), [](gpointer result) {
        delete static_cast<WorkResult*>(result);
      });
    }

    static GThreadPool* pool(bool network) {
      // Separate process-wide limits keep abandoned or stalled network reads
      // from occupying local traversal slots after the next query or tab opens.
      static GThreadPool* local = g_thread_pool_new(readTask, nullptr, kWorkersPerLane, false, nullptr);
      static GThreadPool* remote = g_thread_pool_new(readTask, nullptr, kWorkersPerLane, false, nullptr);
      return network ? remote : local;
    }

    void next() {
      if (g_cancellable_is_cancelled(cancellable))
        return;
      g_autoptr(GTask) task = g_task_new(
          nullptr, cancellable,
          [](GObject*, GAsyncResult* result, gpointer) {
            auto* completed = G_TASK(result);
            const auto& scan = *static_cast<std::shared_ptr<Scan>*>(g_task_get_task_data(completed));
            const std::unique_ptr<WorkResult> work(
                static_cast<WorkResult*>(g_task_propagate_pointer(completed, nullptr))
            );
            if (!work)
              return;
            if (work->publish)
              work->publish();
            if (g_cancellable_is_cancelled(scan->cancellable))
              return;
            if (!work->aliases.empty())
              scan->run->addAliases(std::move(work->aliases));
            if (work->finished) {
              scan->pending.clear();
              scan->queuedDirectories.clear();
              scan->run->finished(scan->root, std::move(scan->error), scan->skipped);
            } else {
              scan->next();
            }
          },
          nullptr
      );
      g_task_set_priority(task, G_PRIORITY_DEFAULT_IDLE);
      g_task_set_task_data(task, new std::shared_ptr<Scan>(shared_from_this()), [](gpointer data) {
        delete static_cast<std::shared_ptr<Scan>*>(data);
      });
      g_thread_pool_push(pool(root.network), g_object_ref(task), nullptr);
    }
  };

  void RecursiveFileDiscovery::Run::addAliases(std::vector<SearchRoot> aliases) {
    for (auto& alias : aliases) {
      if (knownRoots.insert(alias.path.native()).second)
        pending.push_back(std::move(alias));
    }
    schedule();
  }

  void RecursiveFileDiscovery::Run::schedule() {
    if (g_cancellable_is_cancelled(cancellable))
      return;
    for (auto it = pending.begin(); it != pending.end();) {
      const auto lane = static_cast<std::size_t>(it->network);
      if (active[lane] == kWorkersPerLane || activeDevices.contains(it->device)) {
        ++it;
        continue;
      }
      auto source = std::move(*it);
      it = pending.erase(it);
      activeDevices.insert(source.device);
      ++active[lane];
      std::make_shared<Scan>(shared_from_this(), std::move(source))->next();
    }
    if (pending.empty() && active[0] == 0 && active[1] == 0) {
      decltype(visited){}.swap(visited);
      auto done = std::move(complete);
      done(std::move(error), skipped);
    }
  }

  void RecursiveFileDiscovery::Run::finished(const SearchRoot& root, std::string problem, std::size_t folders) {
    skipped += folders;
    if (!problem.empty()) {
      if (plan->reportRootError && root.visibleFrom.empty())
        error = std::move(problem);
      else
        ++skipped;
    }
    --active[static_cast<std::size_t>(root.network)];
    activeDevices.erase(root.device);
    schedule();
  }

  RecursiveFileDiscovery::~RecursiveFileDiscovery() { cancel(); }

  void RecursiveFileDiscovery::start(
      const std::filesystem::path& root, bool showHidden, Batch batch, Complete complete, Flush flush
  ) {
    const auto plan =
        std::make_shared<SearchPlan>(resolveSearchScope({}, root, {}, nullptr, {}, discoverSearchMounts()));
    startPlan(
        plan, showHidden, [batch = std::move(batch), flush = std::move(flush)] { return Consumer{batch, flush}; },
        std::move(complete)
    );
  }

  void RecursiveFileDiscovery::startPlan(
      std::shared_ptr<const SearchPlan> plan, bool showHidden, MakeConsumer consumer, Complete complete
  ) {
    cancel();
    auto run = std::make_shared<Run>();
    run->pending.assign(plan->roots.begin(), plan->roots.end());
    for (const auto& root : plan->roots)
      run->knownRoots.insert(root.path.native());
    run->plan = std::move(plan);
    run->showHidden = showHidden;
    run->consumer = std::move(consumer);
    run->complete = std::move(complete);
    m_run = run;
    run->schedule();
  }

  void RecursiveFileDiscovery::cancel() {
    if (auto run = std::exchange(m_run, nullptr))
      g_cancellable_cancel(run->cancellable);
  }

} // namespace filemanager
