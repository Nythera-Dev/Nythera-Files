#include "filemanager/recent_activity_store.h"

#include "config/atomic_file.h"
#include "core/log.h"
#include "filemanager/runtime_file_lock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace filemanager {

  namespace {

    constexpr Logger kLog("recent-activity");
    constexpr std::size_t kMaxRecentFiles = 200;
    constexpr std::size_t kMaxLocations = 300;
    constexpr double kFrecencyHalfLifeDays = 14.0;
    constexpr double kSecondsPerDay = 86400.0;

    bool isTrackableLocalPath(const std::filesystem::path& path) {
      if (path.empty() || !path.is_absolute()) {
        return false;
      }
      const auto text = path.string();
      return !text.starts_with("trash:") && !text.starts_with("recent:");
    }

    double decayedScore(const LocationActivityRecord& record, std::int64_t nowSec) {
      const double dtDays = std::max(0.0, static_cast<double>(nowSec - record.lastVisitedSec) / kSecondsPerDay);
      return record.frecencyScore * std::exp(-dtDays / kFrecencyHalfLifeDays);
    }

  } // namespace

  RecentActivityStore::RecentActivityStore(std::filesystem::path storagePath) : m_storagePath(std::move(storagePath)) {}

  void RecentActivityStore::setStoragePath(std::filesystem::path storagePath) {
    m_storagePath = std::move(storagePath);
  }

  std::int64_t RecentActivityStore::currentEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  std::optional<std::int64_t> RecentActivityStore::retentionWindowSeconds() const noexcept {
    switch (m_retention) {
    case RecentRetention::Days7:
      return 7LL * 86400LL;
    case RecentRetention::Days30:
      return 30LL * 86400LL;
    case RecentRetention::Days90:
      return 90LL * 86400LL;
    case RecentRetention::Indefinite:
      return std::nullopt;
    }
    return 30LL * 86400LL;
  }

  LocationActivityRecord* RecentActivityStore::findLocation(const std::filesystem::path& path) {
    const auto it = std::ranges::find(m_locations, path, &LocationActivityRecord::path);
    return it == m_locations.end() ? nullptr : &*it;
  }

  const LocationActivityRecord* RecentActivityStore::findLocation(const std::filesystem::path& path) const {
    const auto it = std::ranges::find(m_locations, path, &LocationActivityRecord::path);
    return it == m_locations.end() ? nullptr : &*it;
  }

  bool RecentActivityStore::load() {
    m_recentFiles.clear();
    m_locations.clear();
    if (m_storagePath.empty()) {
      return false;
    }
    std::ifstream in(m_storagePath);
    if (!in) {
      return false;
    }
    try {
      std::stringstream buf;
      buf << in.rdbuf();
      const auto root = nlohmann::json::parse(buf.str());
      if (!root.is_object()) {
        return false;
      }
      if (const auto it = root.find("recent_files"); it != root.end() && it->is_array()) {
        for (const auto& entry : *it) {
          if (!entry.is_object()) {
            continue;
          }
          const auto path = entry.value("path", std::string{});
          const auto ts = entry.value("last_opened", std::int64_t{0});
          if (!path.empty() && std::filesystem::path(path).is_absolute()) {
            m_recentFiles.push_back({std::filesystem::path(path), ts});
          }
        }
      }
      if (const auto it = root.find("locations"); it != root.end() && it->is_array()) {
        for (const auto& entry : *it) {
          if (!entry.is_object()) {
            continue;
          }
          const auto path = entry.value("path", std::string{});
          if (path.empty() || !std::filesystem::path(path).is_absolute()) {
            continue;
          }
          LocationActivityRecord record;
          record.path = std::filesystem::path(path);
          record.visitCount = entry.value("visit_count", std::uint64_t{0});
          record.lastVisitedSec = entry.value("last_visited", std::int64_t{0});
          record.frecencyScore = entry.value("frecency_score", 0.0);
          if (!std::isfinite(record.frecencyScore) || record.frecencyScore < 0.0) {
            record.frecencyScore = 0.0;
          }
          record.pinned = entry.value("pinned", false);
          record.excluded = entry.value("excluded", false);
          m_locations.push_back(std::move(record));
        }
      }
      prune(currentEpochSeconds());
      return true;
    } catch (const std::exception& e) {
      kLog.warn("failed to parse recent activity '{}': {}", m_storagePath.string(), e.what());
      return false;
    }
  }

  bool RecentActivityStore::update(const std::function<bool(RecentActivityStore&)>& mutate) {
    const RuntimeFileLock lock(m_storagePath);
    if (!lock)
      return false;
    if (!load()) {
      std::error_code error;
      if (std::filesystem::exists(m_storagePath, error) || error)
        return false;
    }
    return mutate(*this) && save();
  }

  bool RecentActivityStore::save() const {
    if (m_storagePath.empty()) {
      return false;
    }
    try {
      nlohmann::json filesJson = nlohmann::json::array();
      for (const auto& file : m_recentFiles) {
        filesJson.push_back({
            {"path", file.path.string()},
            {"last_opened", file.lastOpenedSec},
        });
      }
      nlohmann::json locationsJson = nlohmann::json::array();
      for (const auto& loc : m_locations) {
        locationsJson.push_back({
            {"path", loc.path.string()},
            {"visit_count", loc.visitCount},
            {"last_visited", loc.lastVisitedSec},
            {"frecency_score", loc.frecencyScore},
            {"pinned", loc.pinned},
            {"excluded", loc.excluded},
        });
      }
      nlohmann::json root = {
          {"version", 1},
          {"recent_files", std::move(filesJson)},
          {"locations", std::move(locationsJson)},
      };
      std::error_code ec;
      if (m_storagePath.has_parent_path()) {
        std::filesystem::create_directories(m_storagePath.parent_path(), ec);
      }
      return writeTextFileAtomic(m_storagePath, root.dump(2) + "\n");
    } catch (const std::exception& e) {
      kLog.warn("failed to save recent activity '{}': {}", m_storagePath.string(), e.what());
      return false;
    }
  }

  void RecentActivityStore::prune(std::int64_t nowSec) {
    if (const auto window = retentionWindowSeconds()) {
      const std::int64_t cutoff = nowSec - *window;
      std::erase_if(m_recentFiles, [cutoff](const RecentFileRecord& file) { return file.lastOpenedSec < cutoff; });
      for (auto it = m_locations.begin(); it != m_locations.end();) {
        if (it->lastVisitedSec < cutoff) {
          if (it->pinned || it->excluded) {
            it->visitCount = 0;
            it->frecencyScore = 0.0;
            it->lastVisitedSec = 0;
            ++it;
          } else {
            it = m_locations.erase(it);
          }
        } else {
          ++it;
        }
      }
    }
    if (m_recentFiles.size() > kMaxRecentFiles) {
      std::ranges::sort(m_recentFiles, std::ranges::greater{}, &RecentFileRecord::lastOpenedSec);
      m_recentFiles.resize(kMaxRecentFiles);
    }
    if (m_locations.size() > kMaxLocations) {
      std::ranges::stable_sort(m_locations, [&](const auto& a, const auto& b) {
        const bool aKeep = a.pinned || a.excluded;
        const bool bKeep = b.pinned || b.excluded;
        if (aKeep != bKeep) {
          return aKeep > bKeep;
        }
        return decayedScore(a, nowSec) > decayedScore(b, nowSec);
      });
      m_locations.resize(kMaxLocations);
    }
  }

  void RecentActivityStore::recordFileOpen(const std::filesystem::path& path, std::int64_t nowSec) {
    if (!isTrackableLocalPath(path)) {
      return;
    }
    const auto normalized = path.lexically_normal();
    if (auto it = std::ranges::find(m_recentFiles, normalized, &RecentFileRecord::path); it != m_recentFiles.end()) {
      it->lastOpenedSec = std::max(it->lastOpenedSec, nowSec);
    } else {
      m_recentFiles.push_back({normalized, nowSec});
    }
    prune(nowSec);
  }

  void RecentActivityStore::recordDirectoryVisit(const std::filesystem::path& path, std::int64_t nowSec) {
    if (!isTrackableLocalPath(path)) {
      return;
    }
    const auto normalized = path.lexically_normal();
    if (auto* existing = findLocation(normalized)) {
      if (!existing->excluded)
        existing->frecencyScore = decayedScore(*existing, nowSec) + 1.0;
      existing->visitCount += 1;
      existing->lastVisitedSec = std::max(existing->lastVisitedSec, nowSec);
    } else {
      m_locations.push_back(
          LocationActivityRecord{
              .path = normalized,
              .visitCount = 1,
              .lastVisitedSec = nowSec,
              .frecencyScore = 1.0,
              .pinned = false,
              .excluded = false,
          }
      );
    }
    prune(nowSec);
  }

  std::vector<LocationActivityRecord>
  RecentActivityStore::frequentLocations(std::size_t limit, std::int64_t nowSec) const {
    if (limit == 0) {
      return {};
    }
    std::vector<LocationActivityRecord> candidates;
    candidates.reserve(m_locations.size());
    for (const auto& record : m_locations) {
      if (record.excluded) {
        continue;
      }
      if (!record.pinned && record.visitCount == 0) {
        continue;
      }
      candidates.push_back(record);
    }
    std::ranges::stable_sort(candidates, [&](const auto& a, const auto& b) {
      if (a.pinned != b.pinned) {
        return a.pinned > b.pinned;
      }
      const double scoreA = decayedScore(a, nowSec);
      const double scoreB = decayedScore(b, nowSec);
      if (std::abs(scoreA - scoreB) > 1e-6) {
        return scoreA > scoreB;
      }
      if (a.lastVisitedSec != b.lastVisitedSec) {
        return a.lastVisitedSec > b.lastVisitedSec;
      }
      return a.path.string() < b.path.string();
    });
    if (candidates.size() > limit) {
      candidates.resize(limit);
    }
    return candidates;
  }

  std::vector<RecentActivityItem> RecentActivityStore::recentItems(std::int64_t nowSec) const {
    const auto window = retentionWindowSeconds();
    const std::int64_t cutoff = window ? (nowSec - *window) : std::numeric_limits<std::int64_t>::min();
    std::vector<RecentActivityItem> items;
    items.reserve(m_recentFiles.size() + m_locations.size());
    for (const auto& file : m_recentFiles) {
      if (file.lastOpenedSec >= cutoff) {
        items.push_back({file.path, file.lastOpenedSec, false});
      }
    }
    for (const auto& loc : m_locations) {
      if (loc.visitCount > 0 && loc.lastVisitedSec >= cutoff) {
        items.push_back({loc.path, loc.lastVisitedSec, true});
      }
    }
    std::ranges::stable_sort(items, [](const auto& a, const auto& b) {
      if (a.timestampSec != b.timestampSec) {
        return a.timestampSec > b.timestampSec;
      }
      return a.path.string() < b.path.string();
    });
    return items;
  }

  bool RecentActivityStore::setPinned(const std::filesystem::path& path, bool pinned) {
    const auto normalized = path.lexically_normal();
    if (auto* existing = findLocation(normalized)) {
      if (existing->excluded && pinned) {
        existing->excluded = false;
      }
      existing->pinned = pinned;
      if (!pinned && existing->visitCount == 0 && !existing->excluded) {
        std::erase_if(m_locations, [&](const auto& r) { return r.path == normalized; });
      }
      return true;
    }
    if (!pinned || !isTrackableLocalPath(normalized)) {
      return false;
    }
    m_locations.push_back({.path = normalized, .pinned = true});
    return true;
  }

  bool RecentActivityStore::removeLocation(const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    const auto it = std::ranges::find(m_locations, normalized, &LocationActivityRecord::path);
    if (it == m_locations.end()) {
      return false;
    }
    if (it->excluded) {
      it->visitCount = 0;
      it->frecencyScore = 0.0;
      it->lastVisitedSec = 0;
      it->pinned = false;
    } else {
      m_locations.erase(it);
    }
    return true;
  }

  bool RecentActivityStore::excludeLocation(const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    if (!isTrackableLocalPath(normalized)) {
      return false;
    }
    if (auto* existing = findLocation(normalized)) {
      existing->excluded = true;
      existing->pinned = false;
      existing->frecencyScore = 0.0;
      return true;
    }
    m_locations.push_back({.path = normalized, .excluded = true});
    return true;
  }

  bool RecentActivityStore::removeRecentFile(const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    return std::erase_if(m_recentFiles, [&](const auto& f) { return f.path == normalized; }) > 0;
  }

  bool RecentActivityStore::clearHistory() {
    m_recentFiles.clear();
    for (auto it = m_locations.begin(); it != m_locations.end();) {
      if (it->pinned || it->excluded) {
        it->visitCount = 0;
        it->frecencyScore = 0.0;
        it->lastVisitedSec = 0;
        ++it;
      } else {
        it = m_locations.erase(it);
      }
    }
    return true;
  }

} // namespace filemanager
