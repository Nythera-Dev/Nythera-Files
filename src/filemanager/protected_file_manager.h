#pragma once

#include "core/files/directory_scanner.h"
#include "core/timer_manager.h"
#include "filemanager/tab_manager.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class VirtualGridView;

namespace filemanager {

  class BrowserTab;

  class ProtectedFileManager {
  public:
    struct ProtectedCopy {
      TabId tabId = kInvalidTabId;
      std::filesystem::path original;
      std::filesystem::path temporary;
      std::string revision;
      ~ProtectedCopy();
    };
    using ProtectedCopyReady = std::function<void(std::shared_ptr<ProtectedCopy>, const std::string&)>;

    struct Callbacks {
      std::function<VirtualGridView*()> grid;
      std::function<void(std::string)> onStatus;
      std::function<void()> onThumbnailReady;
    };

    ProtectedFileManager(TabManager& tabs, Callbacks callbacks);
    ~ProtectedFileManager();

    ProtectedFileManager(const ProtectedFileManager&) = delete;
    ProtectedFileManager& operator=(const ProtectedFileManager&) = delete;
    ProtectedFileManager(ProtectedFileManager&&) = delete;
    ProtectedFileManager& operator=(ProtectedFileManager&&) = delete;

    void shutdown();

    [[nodiscard]] std::shared_ptr<ProtectedCopy> findEdit(TabId tab, const std::filesystem::path& path) const;
    [[nodiscard]] bool hasEdit(TabId tab, const std::filesystem::path& path) const {
      return findEdit(tab, path) != nullptr;
    }

    std::function<bool()>
    requestCopy(BrowserTab& tab, const std::filesystem::path& path, ProtectedCopyReady ready, bool thumbnail = false);
    void
    openFile(TabId tabId, const std::filesystem::path& path, std::function<void(const std::filesystem::path&)> launch);
    void saveChanges(BrowserTab& tab, const std::filesystem::path& path);

    void clearThumbnails();
    void clearThumbnailsForTab(TabId tabId);
    [[nodiscard]] std::optional<std::string> thumbnailPath(BrowserTab& tab, const FileEntry& entry, std::size_t index);

  private:
    struct ProtectedThumbnail {
      std::filesystem::path path;
      std::shared_ptr<ProtectedCopy> copy;
    };

    TabManager& m_tabs;
    Callbacks m_callbacks;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::vector<std::shared_ptr<ProtectedCopy>> m_edits;
    std::unordered_map<std::size_t, ProtectedThumbnail> m_thumbnails;
    TabId m_thumbnailTab = kInvalidTabId;
    std::uint64_t m_thumbnailListing = 0;
    std::uint64_t m_thumbnailRevision = 0;
    std::function<bool()> m_cancelThumbnail;
    bool m_thumbnailPending = false;
    Timer m_thumbnailRetry;
  };

} // namespace filemanager
