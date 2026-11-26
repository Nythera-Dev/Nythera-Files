#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/fzf_search.h"
#include "filemanager/recursive_file_discovery.h"

#include <vector>

namespace filemanager {

  class FileSearchSession {
  public:
    enum class Status { Idle, Searching, Finished, Failed };
    using BuildPlan = std::function<SearchPlan()>;
    using Changed = std::function<void(bool complete)>;

    struct Order {
      FileDialogSortField field;
      FileDialogSortOrder direction;
      bool foldersFirst;
      bool hiddenLast;
    };

    ~FileSearchSession();

    void edit(
        const std::filesystem::path& root, std::string query, bool showHidden, Order order, Changed changed,
        std::function<bool()> canReorder, FzfSearch backend = {}, BuildPlan buildPlan = {}
    );
    void submit();
    void setOrder(Order order);
    void cancel();

    [[nodiscard]] bool usingFallback() const noexcept { return m_fzfFallback; }
    [[nodiscard]] bool active() const noexcept { return m_status != Status::Idle; }
    [[nodiscard]] Status status() const noexcept { return m_status; }
    [[nodiscard]] const std::string& query() const noexcept { return m_query; }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }
    [[nodiscard]] std::size_t skippedFolders() const noexcept { return m_skipped; }
    [[nodiscard]] std::vector<FileEntry>& entries() noexcept { return m_entries; }
    [[nodiscard]] const std::vector<FileEntry>& entries() const noexcept { return m_entries; }

  private:
    void start();
    [[nodiscard]] static RecursiveFileDiscovery::Publish
    publish(FileSearchSession* session, std::vector<FileEntry> results);
    void completed(std::string error, std::size_t skipped);
    void cancelTimeout();
    void cancelSort();
    void sortResults();

    RecursiveFileDiscovery m_discovery;
    GSource* m_timeout = nullptr;
    GCancellable* m_sort = nullptr;
    BuildPlan m_buildPlan;
    std::string m_query;
    std::string m_error;
    std::vector<FileEntry> m_entries;
    Changed m_changed;
    std::function<bool()> m_canReorder;
    Order m_order{};
    FzfSearch m_fzf;
    bool m_fzfFallback = false;
    bool m_discoveryFinished = false;
    std::size_t m_skipped = 0;
    Status m_status = Status::Idle;
    bool m_showHidden = false;
  };

} // namespace filemanager
