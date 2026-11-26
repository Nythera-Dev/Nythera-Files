// Adapted from Luciole src/fs/recursive_search.cpp (cb6375a5e046).
// Copyright (c) 2026 noctalia-dev. Distributed under the MIT License in LICENSE.
#include "filemanager/recursive_file_discovery.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
  constexpr const char* kNativeAttributes = "standard::name,standard::type";
  constexpr const char* kProviderAttributes = "standard::name,standard::type,standard::is-hidden,standard::is-symlink";
  constexpr const char* kDirectoryIdentity = "standard::type,standard::is-symlink,id::file,id::filesystem";
  constexpr int kBatchSize = 64;
  constexpr unsigned int kMaxDepth = 256;
  constexpr std::size_t kMaxPending = 65536;

  GFileType entryType(GFileInfo* info) {
    if (g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK))
      return G_FILE_TYPE_SYMBOLIC_LINK;
    return g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_TYPE) ? g_file_info_get_file_type(info)
                                                                           : G_FILE_TYPE_UNKNOWN;
  }

  std::unordered_set<std::string> readHiddenNames(const std::filesystem::path& directory, GCancellable* cancel) {
    // Apply the native GIO .hidden format once per directory. O_NONBLOCK prevents a special
    // file or a symlink to one from holding up the search while opening the list.
    const auto path = directory / ".hidden";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
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

  struct RecursiveFileDiscovery::Scan : std::enable_shared_from_this<Scan> {
    struct Folder {
      std::filesystem::path path;
      unsigned int depth = 0;
    };

    struct WorkResult {
      Publish publish;
      bool finished = false;
    };

    GCancellable* cancellable = g_cancellable_new();
    GFileEnumerator* enumerator = nullptr;
    std::deque<Folder> pending;
    Folder current;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> hiddenNames;
    Batch batch;
    Complete complete;
    Flush flush;
    std::string error;
    std::size_t skipped = 0;
    bool showHidden = false;
    bool native = false;

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
      g_object_unref(cancellable);
    }

    void reportFailure(const GError* detail) {
      if (g_cancellable_is_cancelled(cancellable))
        return;
      if (current.depth == 0)
        error = detail ? detail->message : "Unable to read the search folder";
      else
        ++skipped;
    }

    bool openNextFolder() {
      while (!pending.empty() && !g_cancellable_is_cancelled(cancellable) && error.empty()) {
        current = std::move(pending.front());
        pending.pop_front();

        g_autoptr(GFile) file = g_file_new_for_path(current.path.c_str());
        g_autoptr(GError) detail = nullptr;
        if (current.depth != 0) {
          // Recheck directory identity and reject a symlink observed in place of the queued directory.
          g_autoptr(GFileInfo) info =
              g_file_query_info(file, kDirectoryIdentity, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &detail);
          if (!info) {
            reportFailure(detail);
            continue;
          }
          const auto type = entryType(info);
          if (type != G_FILE_TYPE_DIRECTORY && type != G_FILE_TYPE_UNKNOWN) {
            ++skipped;
            continue;
          }
          const auto* id = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE);
          const auto* filesystem = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
          if (id && filesystem && !visited.insert(std::string(filesystem) + '\n' + id).second)
            continue;
        }

        native = g_file_is_native(file);
        enumerator = g_file_enumerate_children(
            file, native ? kNativeAttributes : kProviderAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable,
            &detail
        );
        if (enumerator) {
          hiddenNames =
              native && !showHidden ? readHiddenNames(current.path, cancellable) : std::unordered_set<std::string>{};
          return true;
        }
        reportFailure(detail);
      }
      return false;
    }

    void addCandidate(GFileInfo* info, std::vector<Candidate>& candidates) const {
      const char* name = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_STANDARD_NAME);
      if (!name
          || (!showHidden
              && (name[0] == '.'
                  || hiddenNames.contains(name)
                  || g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN))))
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

    void enqueueFolders(std::span<const Candidate> candidates) {
      for (const auto& candidate : candidates) {
        if (candidate.type != G_FILE_TYPE_DIRECTORY)
          continue;
        if (current.depth >= kMaxDepth || pending.size() >= kMaxPending) {
          ++skipped;
          continue;
        }
        pending.push_back({current.path / candidate.name, current.depth + 1});
      }
    }

    WorkResult readBatch() {
      while (!g_cancellable_is_cancelled(cancellable) && error.empty() && (enumerator || openNextFolder())) {
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
          return {batch(current.path, candidates, cancellable), false};
        }
      }
      return {
          flush && error.empty() && !g_cancellable_is_cancelled(cancellable) ? flush(cancellable) : Publish{}, true
      };
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
            if (work->finished) {
              scan->pending.clear();
              scan->visited.clear();
              scan->hiddenNames.clear();
              auto completedScan = std::move(scan->complete);
              completedScan(std::move(scan->error), scan->skipped);
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
      g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
        auto& scan = **static_cast<std::shared_ptr<Scan>*>(data);
        g_task_return_pointer(worker, new WorkResult(scan.readBatch()), [](gpointer result) {
          delete static_cast<WorkResult*>(result);
        });
      });
    }
  };

  RecursiveFileDiscovery::~RecursiveFileDiscovery() { cancel(); }

  void RecursiveFileDiscovery::start(
      const std::filesystem::path& root, bool showHidden, Batch batch, Complete complete, Flush flush
  ) {
    cancel();
    m_scan = std::make_shared<Scan>();
    m_scan->showHidden = showHidden;
    m_scan->batch = std::move(batch);
    m_scan->complete = std::move(complete);
    m_scan->flush = std::move(flush);
    m_scan->pending.push_back({root});
    m_scan->next();
  }

  void RecursiveFileDiscovery::cancel() {
    if (auto scan = std::exchange(m_scan, nullptr))
      g_cancellable_cancel(scan->cancellable);
  }

} // namespace filemanager
