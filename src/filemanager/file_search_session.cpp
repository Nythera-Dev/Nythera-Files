#include "filemanager/file_search_session.h"

#include "filemanager/file_entry_info.h"
#include "filemanager/file_search_match.h"
#include "filemanager/file_sort.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <iterator>
#include <unistd.h>
#include <utility>

namespace {
  constexpr const char* kResultAttributes =
      "standard::name,standard::type,standard::is-symlink,standard::size,standard::fast-content-type,"
      "time::modified,time::created,time::access,unix::device,unix::inode";

  using Candidate = filemanager::RecursiveFileDiscovery::Candidate;

  using Source = filemanager::RecursiveFileDiscovery::Source;

  class ResultProjection {
  public:
    ResultProjection(
        filemanager::SearchFilters filters, std::shared_ptr<filemanager::ContentSearch> content,
        filemanager::ContentLane lane
    )
        : m_filters(std::move(filters)), m_content(std::move(content)), m_lane(lane) {}

    const filemanager::SearchFilters& filters() const { return m_filters; }
    bool readsContent() const { return bool(m_content); }

    bool needsCandidate(const std::filesystem::path& path, GCancellable* cancel) {
      const auto candidate = m_content->lane(FileEntry{.absPath = path}, cancel);
      return (candidate == filemanager::ContentLane::Extraction) == (m_lane == filemanager::ContentLane::Extraction);
    }

    void add(FileEntry entry, filemanager::ContentCandidate pinned, GCancellable* cancel) {
      if (!m_content) {
        m_result.entries.push_back(std::move(entry));
        return;
      }
      if (m_content->lane(entry, cancel) != m_lane)
        return;
      pinned.entry = std::move(entry);
      if (m_lane == filemanager::ContentLane::Extraction) {
        constexpr std::size_t queueCapacity = 24;
        collectExtraction(false);
        if (m_extraction.size() == queueCapacity) {
          merge(m_extraction.front().get());
          m_extraction.pop_front();
        }
        if (!g_cancellable_is_cancelled(cancel))
          m_extraction.push_back(filemanager::ContentSearch::queueExtraction(m_content, std::move(pinned), cancel));
        return;
      }
      if (m_pending.empty())
        m_queued = std::chrono::steady_clock::now();
      m_pending.push_back(std::move(pinned));
      constexpr std::size_t batchSize = 64;
      if (m_pending.size() == batchSize)
        scan(cancel);
    }

    void skip(const std::filesystem::path& path, GCancellable* cancel) {
      if (m_content->lane(FileEntry{.absPath = path}, cancel) == m_lane)
        ++m_result.issues.inaccessible;
    }

    auto take(GCancellable* cancel, bool final) {
      if (m_lane == filemanager::ContentLane::Extraction)
        collectExtraction(final);
      if (m_content
          && ((final && (m_first || !m_pending.empty()))
              || (!m_pending.empty()
                  && (m_first
                      || m_lane == filemanager::ContentLane::LargePlain
                      || std::chrono::steady_clock::now() - m_queued >= std::chrono::milliseconds(35)))))
        scan(cancel);
      return std::pair(std::exchange(m_result, {}), std::exchange(m_notice, {}));
    }

  private:
    void scan(GCancellable* cancel) {
      auto result = m_content->match(m_pending, cancel);
      m_pending.clear();
      if (std::exchange(m_first, false))
        m_notice = m_content->notice();
      merge(std::move(result));
    }

    void collectExtraction(bool wait) {
      std::erase_if(m_extraction, [this, wait](auto& task) {
        if (!wait && task.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
          return false;
        merge(task.get());
        return true;
      });
    }

    void merge(filemanager::ContentResult result) {
      m_result.issues.merge(result.issues);
      m_result.entries.insert(
          m_result.entries.end(), std::make_move_iterator(result.entries.begin()),
          std::make_move_iterator(result.entries.end())
      );
    }

    filemanager::SearchFilters m_filters;
    std::shared_ptr<filemanager::ContentSearch> m_content;
    filemanager::ContentLane m_lane;
    std::vector<filemanager::ContentCandidate> m_pending;
    std::deque<std::future<filemanager::ContentResult>> m_extraction;
    filemanager::ContentResult m_result;
    std::string m_notice;
    bool m_first = true;
    std::chrono::steady_clock::time_point m_queued;
  };

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
        ResultProjection& results, const std::filesystem::path& parent, const Candidate& candidate, GCancellable* cancel
    ) {
      using filemanager::OpenSearchPath;
      const auto path = parent / candidate.name;
      filemanager::ContentCandidate pinned({}, -1);
      if (results.readsContent()) {
        if ((candidate.type != G_FILE_TYPE_REGULAR
             && (candidate.type != G_FILE_TYPE_SYMBOLIC_LINK || !m_source.openLink))
            || !results.needsCandidate(path, cancel))
          return;
        pinned.fd = m_source.pinRegular(path, candidate.type);
        if (pinned.fd < 0) {
          if (candidate.type == G_FILE_TYPE_REGULAR)
            results.skip(path, cancel);
          return;
        }
      }
      g_autoptr(GFileInfo) info = nullptr;
      if (pinned.fd >= 0) {
        info = query(OpenSearchPath::fdPath(pinned.fd), G_FILE_QUERY_INFO_NONE, cancel);
        if (!info) {
          results.skip(path, cancel);
          return;
        }
        g_file_info_set_name(info, candidate.name.c_str());
        g_file_info_set_is_symlink(info, candidate.type == G_FILE_TYPE_SYMBOLIC_LINK);
      } else if (candidate.type == G_FILE_TYPE_SYMBOLIC_LINK) {
        const OpenSearchPath target(m_plan, path);
        if (target.fd() >= 0)
          info = query(OpenSearchPath::fdPath(target.fd()), G_FILE_QUERY_INFO_NONE, cancel);
        if (info) {
          g_file_info_set_name(info, candidate.name.c_str());
          g_file_info_set_is_symlink(info, true);
        }
      }
      if (!info && path == m_source.root) {
        info = query(OpenSearchPath::fdPath(m_source.rootFd), G_FILE_QUERY_INFO_NONE, cancel);
        if (info) {
          g_file_info_set_name(info, candidate.name.c_str());
          g_file_info_set_is_symlink(info, false);
        }
      } else if (!info) {
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
      if (!info) {
        return;
      }
      const auto& filters = results.filters();
      if (filters.size
          && (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR
              || !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)
              || !filters.size->matches(g_file_info_get_size(info))))
        return;
      if (filters.modified
          && (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)
              || !filters.modified->matches(
                  static_cast<std::int64_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
              )))
        return;
      auto entry = filemanager::makeFileEntry(info, path, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
      if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK)
          && candidate.type == G_FILE_TYPE_SYMBOLIC_LINK) {
        entry.isSymlink = true;
        entry.hasIdentity = false;
      }
      if (entry.type.empty() && entry.isDir)
        entry.type = "inode/directory";
      else if (entry.type.empty() || (pinned.fd >= 0 && g_content_type_is_unknown(entry.type.c_str()))) {
        g_autofree char* guessed = g_content_type_guess(entry.name.c_str(), nullptr, 0, nullptr);
        if (guessed)
          entry.type = guessed;
      }
      results.add(std::move(entry), std::move(pinned), cancel);
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

    bool match(
        const filemanager::FzfSearch& options, const std::string& query, GCancellable* cancel,
        const filemanager::SearchPlan& plan, const Source& source, ResultProjection& results
    ) {
      if (items.empty())
        return true;
      std::vector<std::string_view> names;
      names.reserve(items.size());
      for (const auto& item : items)
        names.push_back(item.candidate.name);
      const auto indices = filemanager::filterWithFzf(options, query, names, cancel);
      if (!indices)
        return false;
      ResultReader reader(plan, source);
      for (const auto index : *indices) {
        if (g_cancellable_is_cancelled(cancel))
          break;
        const auto& item = items[index];
        reader.append(results, item.parent, item.candidate, cancel);
      }
      items.clear();
      first = false;
      return true;
    }
  };
} // namespace

namespace filemanager {

  FileSearchSession::~FileSearchSession() { cancel(); }

  void FileSearchSession::edit(
      const std::filesystem::path& root, std::string query, bool showHidden, Order order, Changed changed,
      std::function<bool()> canReorder, FzfSearch backend, BuildPlan buildPlan, SearchFilters filters
  ) {
    cancel();
    if (query.empty() && !filters.active())
      return;
    m_filters = std::move(filters);

    m_buildPlan = buildPlan ? std::move(buildPlan)
                            : [root] { return resolveSearchScope({}, root, {}, nullptr, {}, discoverSearchMounts()); };
    m_query = std::move(query);
    m_showHidden = showHidden;
    m_changed = std::move(changed);
    m_canReorder = std::move(canReorder);
    m_order = m_displayedOrder = order;
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

  void FileSearchSession::stopDiscovery() {
    cancelTimeout();
    for (auto& lane : m_discovery)
      lane.cancel();
    m_pendingLanes = 0;
    m_completionPending = false;
  }

  void FileSearchSession::stop() {
    if (m_status != Status::Searching)
      return;
    stopDiscovery();
    m_status = Status::Cancelled;
    (void)applyOrder();
    m_changed(true);
  }

  void FileSearchSession::cancel() {
    stopDiscovery();
    cancelSort();
    m_entries.clear();
    m_pendingEntries.clear();
    m_query.clear();
    m_error.clear();
    m_contentNotice.clear();
    m_contentIssues = {};
    m_filters = {};
    m_changed = {};
    m_canReorder = {};
    m_skipped = 0;
    m_buildPlan = {};
    m_fzf = {};
    m_fzfFallback = false;
    m_status = Status::Idle;
  }

  RecursiveFileDiscovery::Publish
  FileSearchSession::publish(FileSearchSession* session, ContentResult results, std::string notice) {
    if (results.entries.empty() && !results.issues.partial() && results.issues.unsupported == 0 && notice.empty())
      return {};
    return [session, results = std::move(results), notice = std::move(notice)]() mutable {
      session->m_pendingEntries.insert(
          session->m_pendingEntries.end(), std::make_move_iterator(results.entries.begin()),
          std::make_move_iterator(results.entries.end())
      );
      session->m_contentIssues.merge(results.issues);
      const bool diagnostics = results.issues.partial() || results.issues.unsupported != 0 || !notice.empty();
      if (!notice.empty())
        session->m_contentNotice = std::move(notice);
      session->notifyResults(session->applyOrder() || diagnostics);
    };
  }

  void FileSearchSession::completed(std::string error, std::size_t skipped) {
    m_error = std::move(error);
    m_skipped = skipped;
    m_completionPending = true;
    resumeOrdering();
  }

  void FileSearchSession::start() {
    for (auto& lane : m_discovery)
      lane.cancel();
    m_error.clear();
    m_skipped = 0;
    const auto plan = std::make_shared<SearchPlan>(m_buildPlan());
    auto content = m_filters.content ? std::make_shared<ContentSearch>(*m_filters.content) : nullptr;
    using Worker = RecursiveFileDiscovery::Lane;
    const bool plain = !m_filters.content || m_filters.content->mode != ContentMode::Extracted;
    const bool extraction = m_filters.content && m_filters.content->mode != ContentMode::Plain;
    m_pendingLanes = static_cast<std::size_t>(plain) + static_cast<std::size_t>(plain && content) + extraction;
    if (plain)
      startLane(plan, content, ContentLane::Plain, Worker::Discovery);
    if (plain && content)
      startLane(plan, content, ContentLane::LargePlain, Worker::LargeContent);
    if (extraction)
      startLane(plan, content, ContentLane::Extraction, Worker::Extraction);
  }

  void FileSearchSession::startLane(
      const std::shared_ptr<SearchPlan>& plan, const std::shared_ptr<ContentSearch>& content, ContentLane lane,
      RecursiveFileDiscovery::Lane worker
  ) {
    // Each lane generates its own bounded batches from the same scope. A slow
    // extractor cannot stall discovery or retain an unbounded queue of pinned
    // descriptors, and a burst of extraction candidates does not drop files.
    auto complete = [this](std::string error, std::size_t skipped) {
      if (m_error.empty())
        m_error = std::move(error);
      m_skipped = std::max(m_skipped, skipped);
      if (--m_pendingLanes == 0)
        completed(std::exchange(m_error, {}), m_skipped);
    };
    m_discovery[static_cast<std::size_t>(worker)].startPlan(
        plan, m_showHidden,
        [this, plan, content, lane, filters = m_filters, options = m_fzf, query = m_query, fallback = m_fzfFallback] {
          auto projection = std::make_shared<ResultProjection>(filters, content, lane);
          const auto publishBatch = [this, projection](GCancellable* cancel, bool final) {
            auto [results, notice] = projection->take(cancel, final);
            return publish(this, std::move(results), std::move(notice));
          };
          if (query.empty() || options.executable.empty() || fallback) {
            return RecursiveFileDiscovery::Consumer{
                [plan, projection, publishBatch, folded = foldSearchName(query)](
                    const std::filesystem::path& parent, std::span<const Candidate> candidates, const Source& source,
                    GCancellable* cancel
                ) {
                  ResultReader reader(*plan, source);
                  for (const auto& candidate : candidates) {
                    if (g_cancellable_is_cancelled(cancel))
                      break;
                    if (folded.empty() || foldSearchName(candidate.name).contains(folded))
                      reader.append(*projection, parent, candidate, cancel);
                  }
                  return publishBatch(cancel, false);
                },
                [content, publishBatch](const Source&, GCancellable* cancel) {
                  return content ? publishBatch(cancel, true) : RecursiveFileDiscovery::Publish{};
                }
            };
          }

          auto batch = std::make_shared<FzfBatch>();
          const auto flush = [this, plan, projection, publishBatch, batch, options, query](
                                 const Source& source, GCancellable* cancel, bool final
                             ) -> RecursiveFileDiscovery::Publish {
            auto batchSource = source;
            batchSource.folderFd = -1;
            if (batch->match(options, query, cancel, *plan, batchSource, *projection))
              return publishBatch(cancel, final);
            return [this] {
              // Discard the partial fzf projection and restart with native matching.
              cancelSort();
              m_entries.clear();
              m_pendingEntries.clear();
              m_displayedOrder = m_order;
              m_contentIssues = {};
              m_contentNotice.clear();
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
                return batch->ready() ? flush(source, cancel, false) : RecursiveFileDiscovery::Publish{};
              },
              [flush](const Source& source, GCancellable* cancel) { return flush(source, cancel, true); }
          };
        },
        std::move(complete), worker
    );
  }

  void FileSearchSession::setOrder(Order order) {
    if (order == m_order)
      return;
    cancelSort();
    m_order = order;
    resumeOrdering();
  }

  void FileSearchSession::resumeOrdering() { notifyResults(applyOrder()); }

  bool FileSearchSession::applyOrder() {
    const bool needsSort = m_displayedOrder != m_order && m_entries.size() > 1;
    if (m_sort || (!needsSort && m_pendingEntries.empty()) || (m_canReorder && !m_canReorder()))
      return false;
    if (needsSort) {
      sortResults();
      return false;
    }
    m_displayedOrder = m_order;
    mergePendingEntries();
    return true;
  }

  void FileSearchSession::notifyResults(bool entriesChanged) {
    if (m_completionPending && !m_sort) {
      m_completionPending = false;
      m_status = m_error.empty() ? Status::Finished : Status::Failed;
      m_changed(true);
    } else if (entriesChanged) {
      m_changed(false);
    }
  }

  void FileSearchSession::mergePendingEntries() {
    if (m_pendingEntries.empty())
      return;
    const auto less = [order = m_displayedOrder](const FileEntry& lhs, const FileEntry& rhs) {
      return fileEntryLess(lhs, rhs, order.field, order.direction, order.foldersFirst, order.hiddenLast);
    };
    std::ranges::stable_sort(m_pendingEntries, less);
    const auto oldSize = m_entries.size();
    m_entries.insert(
        m_entries.end(), std::make_move_iterator(m_pendingEntries.begin()),
        std::make_move_iterator(m_pendingEntries.end())
    );
    m_pendingEntries.clear();
    std::inplace_merge(
        m_entries.begin(), m_entries.begin() + static_cast<std::ptrdiff_t>(oldSize), m_entries.end(), less
    );
  }

  void FileSearchSession::cancelSort() {
    if (m_sort) {
      g_cancellable_cancel(m_sort);
      g_clear_object(&m_sort);
    }
  }

  void FileSearchSession::sortResults() {
    // Keep the old view usable during a column sort. New batches wait for the
    // new order and are merged into the sorted snapshot when it is ready.
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
          if (g_task_get_cancellable(completed) != self.m_sort)
            return;
          g_clear_object(&self.m_sort);
          if (self.m_canReorder && !self.m_canReorder()) {
            self.notifyResults(false);
            return;
          }

          auto& request = *static_cast<SortRequest*>(g_task_get_task_data(completed));
          self.m_entries.swap(request.entries);
          self.m_displayedOrder = request.order;
          self.mergePendingEntries();
          self.notifyResults(true);
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
