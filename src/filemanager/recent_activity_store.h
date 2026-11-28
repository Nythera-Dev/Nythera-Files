#pragma once

#include "filemanager/file_manager_settings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace filemanager {

  struct RecentFileRecord {
    std::filesystem::path path;
    std::int64_t lastOpenedSec = 0;

    bool operator==(const RecentFileRecord&) const = default;
  };

  struct LocationActivityRecord {
    std::filesystem::path path;
    std::uint64_t visitCount = 0;
    std::int64_t lastVisitedSec = 0;
    double frecencyScore = 0.0;
    bool pinned = false;
    bool excluded = false;

    bool operator==(const LocationActivityRecord&) const = default;
  };

  struct RecentActivityItem {
    std::filesystem::path path;
    std::int64_t timestampSec = 0;
    bool isDirectory = false;
  };

  class RecentActivityStore {
  public:
    explicit RecentActivityStore(std::filesystem::path storagePath = {});

    void setStoragePath(std::filesystem::path storagePath);
    [[nodiscard]] const std::filesystem::path& storagePath() const noexcept { return m_storagePath; }

    void setRetention(RecentRetention retention) noexcept { m_retention = retention; }
    [[nodiscard]] RecentRetention retention() const noexcept { return m_retention; }

    bool load();
    bool save() const;
    bool update(const std::function<bool(RecentActivityStore&)>& mutate);
    void prune(std::int64_t nowSec = currentEpochSeconds());

    void recordFileOpen(const std::filesystem::path& path, std::int64_t nowSec = currentEpochSeconds());
    void recordDirectoryVisit(const std::filesystem::path& path, std::int64_t nowSec = currentEpochSeconds());

    [[nodiscard]] std::vector<LocationActivityRecord>
    frequentLocations(std::size_t limit, std::int64_t nowSec = currentEpochSeconds()) const;
    [[nodiscard]] std::vector<RecentActivityItem> recentItems(std::int64_t nowSec = currentEpochSeconds()) const;

    bool setPinned(const std::filesystem::path& path, bool pinned);
    bool removeLocation(const std::filesystem::path& path);
    bool excludeLocation(const std::filesystem::path& path);
    bool removeRecentFile(const std::filesystem::path& path);
    bool clearHistory();

    [[nodiscard]] const std::vector<RecentFileRecord>& recentFiles() const noexcept { return m_recentFiles; }
    [[nodiscard]] const std::vector<LocationActivityRecord>& locations() const noexcept { return m_locations; }

    [[nodiscard]] static std::int64_t currentEpochSeconds();

  private:
    [[nodiscard]] std::optional<std::int64_t> retentionWindowSeconds() const noexcept;
    [[nodiscard]] LocationActivityRecord* findLocation(const std::filesystem::path& path);
    [[nodiscard]] const LocationActivityRecord* findLocation(const std::filesystem::path& path) const;

    std::filesystem::path m_storagePath;
    RecentRetention m_retention = RecentRetention::Days30;
    std::vector<RecentFileRecord> m_recentFiles;
    std::vector<LocationActivityRecord> m_locations;
  };

} // namespace filemanager
