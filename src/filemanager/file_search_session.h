#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/fzf_search.h"
#include "filemanager/recursive_file_discovery.h"
#include "filemanager/search_content.h"

#include <array>
#include <vector>

namespace filemanager {

  class FileSearchSession {
  public:
    enum class Status { Idle, Searching, Finished, Failed, Cancelled };
    using BuildPlan = std::function<SearchPlan()>;
    using Changed = std::function<void(bool complete)>;

    struct Order {
      FileDialogSortField field;
      FileDialogSortOrder direction;
      bool foldersFirst;
      bool hiddenLast;
      bool operator==(const Order&) const = default;
    };

    ~FileSearchSession();

    void edit(
        const std::filesystem::path& root, std::string query, bool showHidden, Order order, Changed changed,
        std::function<bool()> canReorder, FzfSearch backend = {}, BuildPlan buildPlan = {}, SearchFilters filters = {}
    );
    void submit();
    void setOrder(Order order);
    void cancel();
    void stop();
    void resumeOrdering();

    [[nodiscard]] bool usingFallback() const noexcept { return m_fzfFallback; }
    [[nodiscard]] bool active() const noexcept { return m_status != Status::Idle; }
    [[nodiscard]] Status status() const noexcept { return m_status; }
    [[nodiscard]] const std::string& query() const noexcept { return m_query; }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }
    [[nodiscard]] const std::string& contentNotice() const noexcept { return m_contentNotice; }
    [[nodiscard]] const ContentIssues& contentIssues() const noexcept { return m_contentIssues; }
    [[nodiscard]] std::size_t skippedFolders() const noexcept { return m_skipped; }
    [[nodiscard]] std::vector<FileEntry>& entries() noexcept { return m_entries; }
    [[nodiscard]] const std::vector<FileEntry>& entries() const noexcept { return m_entries; }

  private:
    void start();
    void startLane(
        const std::shared_ptr<SearchPlan>& plan, const std::shared_ptr<ContentSearch>& content, ContentLane lane,
        RecursiveFileDiscovery::Lane worker
    );
    [[nodiscard]] static RecursiveFileDiscovery::Publish
    publish(FileSearchSession* session, ContentResult results, std::string notice = {});
    void completed(std::string error, std::size_t skipped);
    void stopDiscovery();
    void cancelTimeout();
    void cancelSort();
    void sortResults();
    void mergePendingEntries();
    [[nodiscard]] bool applyOrder();
    void notifyResults(bool entriesChanged);

    std::array<RecursiveFileDiscovery, 3> m_discovery;
    GSource* m_timeout = nullptr;
    GCancellable* m_sort = nullptr;
    BuildPlan m_buildPlan;
    std::string m_query;
    std::string m_error;
    SearchFilters m_filters;
    std::string m_contentNotice;
    ContentIssues m_contentIssues;
    std::size_t m_pendingLanes = 0;
    std::vector<FileEntry> m_entries;
    std::vector<FileEntry> m_pendingEntries;
    Changed m_changed;
    std::function<bool()> m_canReorder;
    Order m_order{};
    Order m_displayedOrder{};
    FzfSearch m_fzf;
    bool m_fzfFallback = false;
    bool m_completionPending = false;
    std::size_t m_skipped = 0;
    Status m_status = Status::Idle;
    bool m_showHidden = false;
  };

} // namespace filemanager
