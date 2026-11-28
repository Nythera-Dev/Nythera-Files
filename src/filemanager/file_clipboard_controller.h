#pragma once

#include "filemanager/browser_tab.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/path_resolution_cache.h"
#include "filemanager/places_sidebar.h"
#include "filemanager/tab_manager.h"
#include "wayland/native_data_transfer.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class FileManagerTestAccess;
class Renderer;
class VirtualGridView;

namespace filemanager {

  class FileClipboardController {
  public:
    struct Callbacks {
      std::function<ToplevelSurface*()> mainSurface;
      std::function<VirtualGridView*()> grid;
      std::function<bool()> isWindowOpen;
      std::function<bool()> isQuitting;
      std::function<std::vector<std::string>()> selectedPathsOrdered;
      std::function<void(TabId tabId, std::unordered_set<std::string> paths, std::filesystem::path primary)>
          onApplyPasteSelection;
      std::function<void()> onUpdateSelectionSummary;
      std::function<void()> onCutStateChanged;
      std::function<void(std::string)> onStatus;
      std::function<void()> onMaybeStopMainLoop;
    };

    FileClipboardController(
        WaylandConnection& wayland, NativeDataTransfer& transfer, FileOperationManager& operations, TabManager& tabs,
        PlacesSidebarController& places, PathResolutionCache& pathCache, Callbacks callbacks
    );
    ~FileClipboardController();

    FileClipboardController(const FileClipboardController&) = delete;
    FileClipboardController& operator=(const FileClipboardController&) = delete;
    FileClipboardController(FileClipboardController&&) = delete;
    FileClipboardController& operator=(FileClipboardController&&) = delete;

    void shutdown();

    bool onOperationFinished(const FileOperationManager::Operation& operation);
    void onBeforePathChange(BrowserTab& tab);
    void onEnumerateStarted(BrowserTab& tab);
    void registerPasteRequest(std::uint64_t opId, PasteRequest request);

    void pasteClipboard();
    void copyToDesktopClipboard(bool cut);
    void writeDesktopClipboard(std::string payload, const char* mime);
    void setCutPaths(std::vector<std::string> paths);
    bool cancelCutSelection();
    void syncCutStateFromClipboard();

    [[nodiscard]] bool isPathCut(const std::filesystem::path& path) const;
    [[nodiscard]] bool hasActiveCutItems() const noexcept { return !m_activeCutPaths.empty(); }
    [[nodiscard]] std::size_t pendingClipboardReads() const noexcept { return m_pendingClipboardReads; }

    bool recordPasteCompletion(const FileOperationManager::Operation& operation);
    void applyPendingPasteSelection(BrowserTab& tab);
    void applyPendingPasteSelection() { applyPendingPasteSelection(m_tabs.activeTab()); }
    void finishPendingTransferReveal(Renderer& renderer);

  private:
    friend class ::FileManagerTestAccess;

    WaylandConnection& m_wayland;
    NativeDataTransfer& m_transfer;
    FileOperationManager& m_operations;
    TabManager& m_tabs;
    PlacesSidebarController& m_places;
    PathResolutionCache& m_pathCache;
    Callbacks m_callbacks;
    std::function<void(NativeDataTransfer::ReadCallback)> m_readPasteSelectionForTesting;

    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::unordered_map<std::uint64_t, PasteRequest> m_pasteRequests;
    std::unordered_map<std::uint64_t, std::uint64_t> m_cutOperations;
    std::unordered_map<std::uint64_t, std::vector<std::string>> m_cutOperationSources;
    std::optional<std::uint64_t> m_consumedCutSelection;
    std::unordered_set<std::string> m_activeCutPaths;
    std::vector<std::string> m_activeCutOrdered;
    std::uint64_t m_externalCutQueryGeneration = 0;
    std::size_t m_pendingClipboardReads = 0;
  };

} // namespace filemanager
