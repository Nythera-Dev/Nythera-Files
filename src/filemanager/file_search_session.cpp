#include "filemanager/file_search_session.h"

#include "filemanager/file_entry_info.h"
#include "filemanager/file_search_match.h"
#include "filemanager/file_sort.h"

#include <chrono>
#include <iterator>
#include <unistd.h>
#include <utility>

namespace {
  constexpr const char* kResultAttributes =
      "standard::name,standard::type,standard::is-symlink,standard::size,standard::fast-content-type,"
      "time::modified,time::created,time::access,unix::device,unix::inode";

  using Candidate = filemanager::RecursiveFileDiscovery::Candidate;

  using Source = filemanager::RecursiveFileDiscovery::Source;

  class ResultReader {
  public:
    ResultReader(const filemanager::SearchPlan& plan, const Source& source) : m_plan(plan), m_source(source) {}
    ~ResultReader() {
      if (m_folder >= 0)
        close(m_folder);
    }
    ResultReader(const ResultReader&) = delete;
    ResultReader& operator=(const ResultReader&) = delete;

    void append(
        std::vector<FileEntry>& results, const std::filesystem::path& parent, const Candidate& candidate,
        GCancellable* cancel
    ) {
      using filemanager::OpenSearchPath;
      const auto path = parent / candidate.name;
      g_autoptr(GFileInfo) info = nullptr;
      if (candidate.type == G_FILE_TYPE_SYMBOLIC_LINK) {
        const OpenSearchPath target(m_plan, path);
        if (target.fd() >= 0)
          info = query(OpenSearchPath::fdPath(target.fd()), G_FILE_QUERY_INFO_NONE, cancel);
      }
      if (info) {
        g_file_info_set_name(info, candidate.name.c_str());
        g_file_info_set_is_symlink(info, true);
      } else if (path == m_source.root) {
        info = query(OpenSearchPath::fdPath(m_source.rootFd), G_FILE_QUERY_INFO_NONE, cancel);
        if (info) {
          g_file_info_set_name(info, candidate.name.c_str());
          g_file_info_set_is_symlink(info, false);
        }
      } else {
        if (m_source.folderFd >= 0) {
          if (m_pinnedParent.empty())
            m_pinnedParent = OpenSearchPath::fdPath(m_source.folderFd);
        } else if (m_parent != parent) {
          if (m_folder >= 0)
            close(m_folder);
          m_parent = parent;
          m_folder = m_source.openFolder(parent);
          m_pinnedParent = m_folder >= 0 ? OpenSearchPath::fdPath(m_folder) : std::filesystem::path{};
        }
        if (!m_pinnedParent.empty())
          info = query(m_pinnedParent / candidate.name, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel);
      }
      if (!info)
        return;
      auto entry = filemanager::makeFileEntry(info, path, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
      if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK)
          && candidate.type == G_FILE_TYPE_SYMBOLIC_LINK) {
        entry.isSymlink = true;
        entry.hasIdentity = false;
      }
      if (entry.type.empty() && entry.isDir)
        entry.type = "inode/directory";
      else if (entry.type.empty()) {
        g_autofree char* guessed = g_content_type_guess(entry.name.c_str(), nullptr, 0, nullptr);
        if (guessed)
          entry.type = guessed;
      }
      results.push_back(std::move(entry));
    }

  private:
    static GFileInfo* query(const std::filesystem::path& path, GFileQueryInfoFlags flags, GCancellable* cancel) {
      g_autoptr(GFile) file = g_file_new_for_path(path.c_str());
      return g_file_query_info(file, kResultAttributes, flags, cancel, nullptr);
    }

    const filemanager::SearchPlan& m_plan;
    const Source& m_source;
    int m_folder = -1;
    std::filesystem::path m_parent;
    std::filesystem::path m_pinnedParent;
  };

  struct FzfBatch {
    struct Item {
      std::filesystem::path parent;
      Candidate candidate;
    };

    bool first = true;
    std::chrono::steady_clock::time_point queuedAt;
    std::vector<Item> items;

    void push(const std::filesystem::path& parent, std::span<const Candidate> candidates) {
      if (items.empty())
        queuedAt = std::chrono::steady_clock::now();
      for (const auto& candidate : candidates)
        items.push_back({parent, candidate});
    }

    bool ready() const {
      // Publish the first directory batch immediately. Amortise later process
      // starts while letting slow traversals publish as further batches arrive.
      return first
          || items.size() >= 1024
          || std::chrono::steady_clock::now() - queuedAt >= std::chrono::milliseconds(35);
    }

    std::optional<std::vector<FileEntry>> match(
        const filemanager::FzfSearch& options, const std::string& query, GCancellable* cancel,
        const filemanager::SearchPlan& plan, const Source& source
    ) {
      if (items.empty())
        return std::vector<FileEntry>{};
      std::vector<std::string_view> names;
      names.reserve(items.size());
      for (const auto& item : items)
        names.push_back(item.candidate.name);
      const auto indices = filemanager::filterWithFzf(options, query, names, cancel);
      if (!indices)
        return std::nullopt;
      std::vector<FileEntry> results;
      ResultReader reader(plan, source);
      for (const auto index : *indices) {
        if (g_cancellable_is_cancelled(cancel))
          break;
        const auto& item = items[index];
        reader.append(results, item.parent, item.candidate, cancel);
      }
      items.clear();
      first = false;
      return results;
    }
  };
} // namespace

namespace filemanager {

  FileSearchSession::~FileSearchSession() { cancel(); }

  void FileSearchSession::edit(
      const std::filesystem::path& root, std::string query, bool showHidden, Order order, Changed changed,
      std::function<bool()> canReorder, FzfSearch backend, BuildPlan buildPlan
  ) {
    cancel();
    if (query.empty())
      return;

    m_buildPlan = buildPlan ? std::move(buildPlan)
                            : [root] { return resolveSearchScope({}, root, {}, nullptr, {}, discoverSearchMounts()); };
    m_query = std::move(query);
    m_showHidden = showHidden;
    m_changed = std::move(changed);
    m_canReorder = std::move(canReorder);
    m_order = order;
    m_fzf = std::move(backend);
    m_status = Status::Searching;
    // Debounce live edits, but invalidate the previous scan and results immediately.
    m_timeout = g_timeout_source_new(175);
    g_source_set_callback(
        m_timeout,
        [](gpointer data) -> gboolean {
          static_cast<FileSearchSession*>(data)->submit();
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(m_timeout, g_main_context_get_thread_default());
  }

  void FileSearchSession::submit() {
    if (!m_timeout)
      return;
    cancelTimeout();
    start();
  }

  void FileSearchSession::cancelTimeout() {
    if (auto* timeout = std::exchange(m_timeout, nullptr)) {
      g_source_destroy(timeout);
      g_source_unref(timeout);
    }
  }

  void FileSearchSession::cancel() {
    cancelTimeout();
    m_discovery.cancel();
    cancelSort();
    m_entries.clear();
    m_query.clear();
    m_error.clear();
    m_changed = {};
    m_canReorder = {};
    m_discoveryFinished = false;
    m_skipped = 0;
    m_buildPlan = {};
    m_fzf = {};
    m_fzfFallback = false;
    m_status = Status::Idle;
  }

  RecursiveFileDiscovery::Publish
  FileSearchSession::publish(FileSearchSession* session, std::vector<FileEntry> results) {
    if (results.empty())
      return {};
    return [session, results = std::move(results)]() mutable {
      session->m_entries.insert(
          session->m_entries.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end())
      );
      session->m_changed(false);
    };
  }

  void FileSearchSession::completed(std::string error, std::size_t skipped) {
    m_error = std::move(error);
    m_skipped = skipped;
    m_discoveryFinished = true;
    if (!m_error.empty()) {
      m_status = Status::Failed;
      m_changed(true);
    } else {
      sortResults();
    }
  }

  void FileSearchSession::start() {
    auto plan = std::make_shared<SearchPlan>(m_buildPlan());
    auto complete = [this](std::string error, std::size_t skipped) { completed(std::move(error), skipped); };
    if (m_fzf.executable.empty() || m_fzfFallback) {
      m_discovery.startPlan(
          plan, m_showHidden,
          [this, plan, query = foldSearchName(m_query)] {
            return RecursiveFileDiscovery::Consumer{
                [this, plan, query](
                    const std::filesystem::path& parent, std::span<const Candidate> candidates, const Source& source,
                    GCancellable* cancel
                ) {
                  std::vector<FileEntry> results;
                  ResultReader reader(*plan, source);
                  for (const auto& candidate : candidates) {
                    if (g_cancellable_is_cancelled(cancel))
                      break;
                    if (foldSearchName(candidate.name).contains(query))
                      reader.append(results, parent, candidate, cancel);
                  }
                  return publish(this, std::move(results));
                },
                {}
            };
          },
          std::move(complete)
      );
      return;
    }

    m_discovery.startPlan(
        plan, m_showHidden,
        [this, plan, options = m_fzf, query = m_query] {
          auto batch = std::make_shared<FzfBatch>();
          auto flush = [this, plan, batch, options,
                        query](const Source& source, GCancellable* cancel) -> RecursiveFileDiscovery::Publish {
            auto results = batch->match(options, query, cancel, *plan, source);
            if (results)
              return publish(this, std::move(*results));
            return [this] {
              // Discard the partial fzf projection and restart with native matching.
              m_entries.clear();
              m_fzfFallback = true;
              start();
              m_changed(false);
            };
          };
          return RecursiveFileDiscovery::Consumer{
              [batch, flush](
                  const std::filesystem::path& parent, std::span<const Candidate> candidates, const Source& source,
                  GCancellable* cancel
              ) {
                batch->push(parent, candidates);
                return batch->ready() ? flush({source.root, source.rootFd, -1}, cancel)
                                      : RecursiveFileDiscovery::Publish{};
              },
              flush
          };
        },
        std::move(complete)
    );
  }

  void FileSearchSession::setOrder(Order order) {
    m_order = order;
    if (m_discoveryFinished && m_status != Status::Failed)
      sortResults();
  }

  void FileSearchSession::cancelSort() {
    if (m_sort) {
      g_cancellable_cancel(m_sort);
      g_clear_object(&m_sort);
    }
  }

  void FileSearchSession::sortResults() {
    cancelSort();
    if (m_entries.size() < 2) {
      m_status = Status::Finished;
      m_changed(true);
      return;
    }

    // Keep the progressive listing usable while sorting a snapshot off the UI thread.
    struct SortRequest {
      std::vector<FileEntry> entries;
      Order order;
    };
    m_sort = g_cancellable_new();
    g_autoptr(GTask) task = g_task_new(
        nullptr, m_sort,
        [](GObject*, GAsyncResult* result, gpointer data) {
          auto* completed = G_TASK(result);
          if (!g_task_propagate_boolean(completed, nullptr))
            return;

          auto& self = *static_cast<FileSearchSession*>(data);
          g_clear_object(&self.m_sort);
          auto& request = *static_cast<SortRequest*>(g_task_get_task_data(completed));
          if (!self.m_canReorder || self.m_canReorder())
            self.m_entries.swap(request.entries);
          self.m_status = Status::Finished;
          self.m_changed(true);
        },
        this
    );
    g_task_set_priority(task, G_PRIORITY_DEFAULT_IDLE);
    g_task_set_task_data(task, new SortRequest{m_entries, m_order}, [](gpointer data) {
      delete static_cast<SortRequest*>(data);
    });
    g_task_run_in_thread(task, [](GTask* work, gpointer, gpointer data, GCancellable*) {
      if (g_task_return_error_if_cancelled(work))
        return;
      auto& request = *static_cast<SortRequest*>(data);
      const auto order = request.order;
      std::ranges::stable_sort(request.entries, [order](const FileEntry& lhs, const FileEntry& rhs) {
        return fileEntryLess(lhs, rhs, order.field, order.direction, order.foldersFirst, order.hiddenLast);
      });
      g_task_return_boolean(work, true);
    });
  }

} // namespace filemanager
