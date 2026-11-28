#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

class FileManagerTestAccess;

namespace filemanager {

  struct PlacePathResolution {
    bool exists = false;
    bool isDirectory = false;
    std::filesystem::path canonicalPath;
  };

  class PathResolutionCache {
  public:
    PathResolutionCache() = default;
    ~PathResolutionCache();

    PathResolutionCache(const PathResolutionCache&) = delete;
    PathResolutionCache& operator=(const PathResolutionCache&) = delete;
    PathResolutionCache(PathResolutionCache&&) = delete;
    PathResolutionCache& operator=(PathResolutionCache&&) = delete;

    void cancel();

    void
    resolveAsync(const std::filesystem::path& path, std::function<void(const PlacePathResolution&)> onResolved = {});
    void cacheResolved(const std::filesystem::path& path, const PlacePathResolution& resolved);

    [[nodiscard]] std::optional<PlacePathResolution> findCached(const std::filesystem::path& path) const;
    [[nodiscard]] bool samePersonalPlace(const std::filesystem::path& first, const std::filesystem::path& second) const;
    [[nodiscard]] std::optional<bool>
    sameDirectoryPathCached(const std::filesystem::path& first, const std::filesystem::path& second) const;

    void compareDirectoriesAsync(
        const std::filesystem::path& first, const std::filesystem::path& second, std::function<void(bool)> onResolved
    );

    void
    setResolveHook(std::function<bool(const std::filesystem::path&, std::function<void(PlacePathResolution)>)> hook) {
      m_resolveHook = std::move(hook);
    }

  private:
    friend class ::FileManagerTestAccess;

    std::unordered_map<std::string, PlacePathResolution> m_cache;
    std::unordered_set<std::string> m_pending;
    GCancellable* m_cancellable = nullptr;
    std::uint64_t m_generation = 0;
    std::function<bool(const std::filesystem::path&, std::function<void(PlacePathResolution)>)> m_resolveHook;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
