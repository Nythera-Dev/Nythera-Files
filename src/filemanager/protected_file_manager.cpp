#include "filemanager/protected_file_manager.h"

#include "filemanager/browser_tab.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_operation_engine.h"
#include "render/core/thumbnail_service.h"
#include "ui/controls/virtual_grid_view.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <gio/gio.h>
#include <unistd.h>
#include <utility>

namespace filemanager {

  ProtectedFileManager::ProtectedCopy::~ProtectedCopy() {
    if (!temporary.empty()) {
      unlink(temporary.c_str());
      rmdir(temporary.parent_path().c_str());
    }
  }

  ProtectedFileManager::ProtectedFileManager(TabManager& tabs, Callbacks callbacks)
      : m_tabs(tabs), m_callbacks(std::move(callbacks)) {}

  ProtectedFileManager::~ProtectedFileManager() { shutdown(); }

  void ProtectedFileManager::shutdown() {
    *m_alive = false;
    clearThumbnails();
    m_edits.clear();
  }

  std::shared_ptr<ProtectedFileManager::ProtectedCopy>
  ProtectedFileManager::findEdit(TabId tab, const std::filesystem::path& path) const {
    const auto found =
        std::ranges::find_if(m_edits, [&](const auto& copy) { return copy->tabId == tab && copy->original == path; });
    return found == m_edits.end() ? nullptr : *found;
  }

  std::function<bool()> ProtectedFileManager::requestCopy(
      BrowserTab& tab, const std::filesystem::path& path, ProtectedCopyReady ready, bool thumbnail
  ) {
    g_autoptr(GError) error = nullptr;
    g_autofree char* directory = g_dir_make_tmp("nythera-protected-XXXXXX", &error);
    if (!directory) {
      ready(nullptr, error ? error->message : "Could not create a temporary copy directory.");
      return {};
    }
    auto copy = std::make_shared<ProtectedCopy>();
    copy->tabId = tab.id;
    copy->original = path;
    copy->temporary = std::filesystem::path(directory) / path.filename();
    const int fd = open(copy->temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      ready(nullptr, "Could not create the temporary copy.");
      return {};
    }
    close(fd);
    const auto generation = tab.adminSession.sessionGeneration();
    auto transfer = tab.adminSession.runOperation(
        {.kind = file_operation_engine::Kind::ExportProtectedFile,
         .sources = {path.string()},
         .destination = copy->temporary.string(),
         .contentRevision = thumbnail ? "thumbnail" : ""},
        generation, nullptr,
        [this, alive = std::weak_ptr<bool>(m_alive), copy, generation,
         ready = std::move(ready)](AdminServerFrame result) {
          const auto token = alive.lock();
          if (!token || !*token)
            return;
          const auto* origin = m_tabs.findTab(copy->tabId);
          if (!origin || !origin->hasActiveAdminSession() || origin->adminSession.sessionGeneration() != generation) {
            ready(nullptr, "The originating Administrator session ended.");
            return;
          }
          if (result.status != AdminStatusCode::Ok || result.customDescription.empty()) {
            ready(nullptr, result.detail.empty() ? "Could not read the protected file." : result.detail);
            return;
          }
          copy->revision = std::move(result.customDescription);
          ready(copy, {});
        }
    );
    return std::move(transfer.cancel);
  }

  void ProtectedFileManager::clearThumbnails() {
    ++m_thumbnailRevision;
    m_thumbnailTab = kInvalidTabId;
    m_thumbnailRetry.stop();
    m_thumbnailPending = false;
    if (auto cancel = std::exchange(m_cancelThumbnail, {}))
      (void)cancel();
    m_thumbnails.clear();
  }

  void ProtectedFileManager::clearThumbnailsForTab(TabId tabId) {
    if (tabId == m_thumbnailTab)
      clearThumbnails();
  }

  std::optional<std::string>
  ProtectedFileManager::thumbnailPath(BrowserTab& tab, const FileEntry& entry, std::size_t index) {
    if (!tab.hasActiveAdminSession()
        || entry.isDir
        || entry.isSymlink
        || !DirectoryScanner::isThumbnailPath(entry.absPath)
        || userCanReadPath(entry.absPath))
      return std::nullopt;
    const auto maximum = DirectoryScanner::isSvgPath(entry.absPath) ? ThumbnailService::kMaxSvgSourceBytes
                                                                    : ThumbnailService::kMaxSourceFileBytes;
    VirtualGridView* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (!grid || entry.size == 0 || entry.size > maximum)
      return std::string{};
    if (m_thumbnailTab != tab.id || m_thumbnailListing != tab.session.enumerationGeneration()) {
      clearThumbnails();
      m_thumbnailTab = tab.id;
      m_thumbnailListing = tab.session.enumerationGeneration();
    }
    std::erase_if(m_thumbnails, [grid](const auto& item) { return !grid->inVisiblePool(item.first); });
    if (const auto cached = m_thumbnails.find(index);
        cached != m_thumbnails.end() && cached->second.path == entry.absPath)
      return cached->second.copy ? cached->second.copy->temporary.string() : std::string{};
    if (!m_thumbnailPending && !m_thumbnailRetry.active()) {
      m_thumbnailPending = true;
      m_cancelThumbnail = requestCopy(
          tab, entry.absPath,
          [this, index, path = entry.absPath, revision = m_thumbnailRevision](auto copy, const auto& error) {
            if (revision != m_thumbnailRevision)
              return;
            m_cancelThumbnail = {};
            m_thumbnailPending = false;
            if (error != "Another shared operation is active.")
              m_thumbnails.insert_or_assign(index, ProtectedThumbnail{path, std::move(copy)});
            // Rebind outside the adapter's layout pass and let the next visible item
            // submit after this read's terminal frame.
            m_thumbnailRetry.start(std::chrono::milliseconds(error.empty() ? 0 : 150), [this] {
              if (m_callbacks.onThumbnailReady)
                m_callbacks.onThumbnailReady();
            });
          },
          true
      );
    }
    return std::string{};
  }

  void ProtectedFileManager::openFile(
      TabId tabId, const std::filesystem::path& path, std::function<void(const std::filesystem::path&)> launch
  ) {
    auto* tab = m_tabs.findTab(tabId);
    if (!tab || !tab->hasActiveAdminSession())
      return;
    if (const auto copy = findEdit(tabId, path)) {
      launch(copy->temporary);
      return;
    }
    requestCopy(*tab, path, [this, tabId, launch = std::move(launch)](auto copy, const auto& error) {
      if (!copy) {
        if (auto* origin = m_tabs.findTab(tabId); origin && !origin->active)
          origin->lastStatusMessage = error;
        else if (tabId == m_tabs.activeTabId() && m_callbacks.onStatus)
          m_callbacks.onStatus(error);
        return;
      }
      m_edits.push_back(copy);
      launch(copy->temporary);
    });
  }

  void ProtectedFileManager::saveChanges(BrowserTab& tab, const std::filesystem::path& path) {
    const auto copy = findEdit(tab.id, path);
    if (!copy || !tab.hasActiveAdminSession())
      return;
    [[maybe_unused]] const auto transfer = tab.adminSession.runOperation(
        {.kind = file_operation_engine::Kind::SaveProtectedFile,
         .sources = {copy->temporary.string()},
         .destination = copy->original.string(),
         .contentRevision = copy->revision},
        tab.adminSession.sessionGeneration(), nullptr,
        [this, alive = std::weak_ptr<bool>(m_alive), copy](AdminServerFrame result) {
          const auto token = alive.lock();
          if (!token || !*token)
            return;
          if (result.status == AdminStatusCode::Ok) {
            copy->revision = std::move(result.customDescription);
            if (auto* origin = m_tabs.findTab(copy->tabId);
                origin && origin->session.path() == copy->original.parent_path())
              origin->session.refreshPreservingSelection();
          }
          if (copy->tabId == m_tabs.activeTabId()) {
            if (m_callbacks.onStatus)
              m_callbacks.onStatus(result.detail);
          } else if (auto* origin = m_tabs.findTab(copy->tabId)) {
            origin->lastStatusMessage = result.detail;
          }
        }
    );
  }

} // namespace filemanager
