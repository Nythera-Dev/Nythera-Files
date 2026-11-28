#pragma once

#include "filemanager/dialogs/checksum_dialog.h"
#include "filemanager/dialogs/help_dialog.h"
#include "filemanager/file_clipboard_controller.h"
#include "filemanager/file_launch_controller.h"
#include "filemanager/file_list_header.h"
#include "filemanager/file_manager_settings.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/file_selection_controller.h"
#include "filemanager/menu_ids.h"
#include "filemanager/places_sidebar.h"
#include "filemanager/protected_file_manager.h"
#include "filemanager/tab_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/native_data_transfer.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class Box;
class Button;
class ConfigService;
class FileManagerTestAccess;
class Flex;
class LayerPopupHostRegistry;
class RenderContext;

namespace filemanager {

  class FileMenuController {
  public:
    struct Callbacks {
      std::function<ToplevelSurface*()> mainSurface;
      std::function<std::uint32_t()> lastInputSerial;
      std::function<void(std::uint32_t)> setLastInputSerial;
      std::function<bool()> isModalOpen;
      std::function<bool()> isRenaming;
      std::function<bool()> alwaysShowTabBar;
      std::function<bool()> openTabsInBackground;
      std::function<bool()> canChangeSelection;
      std::function<std::optional<std::filesystem::path>()> selectedPath;
      std::function<const std::unordered_set<std::string>&()> selectedPaths;
      std::function<std::vector<std::string>()> selectedPathsOrdered;
      std::function<std::optional<std::pair<std::filesystem::path, bool>>()> symlinkSelection;
      std::function<std::array<ContextMenuControlEntry, 2>()> checksumMenuEntries;
      std::function<void(std::size_t index)> onSelectSingleForContextMenu;
      std::function<void()> onClearSelectionForEmptyMenu;
      std::function<void(std::string)> onStatus;
      std::function<void()> onRequestLayout;

      std::function<bool(int)> activateArchiveAction;
      std::function<bool(const ContextMenuControlEntry&)> activateSearchScopeEntry;
      std::function<bool(const ContextMenuControlEntry&)> activateSearchFilterEntry;
      std::function<void(SearchMode)> selectSearchMode;
      std::function<void(TabId, std::optional<std::filesystem::path>)> enterAdministratorMode;
      std::function<void(bool verify)> showChecksumDialog;
      std::function<void(const std::filesystem::path&)> launchNewWindow;
      std::function<void()> activateSelected;
      std::function<void(const std::filesystem::path&)> activatePath;
      std::function<void(const std::filesystem::path&, bool withApp)> openArchiveMember;
      std::function<void(const std::filesystem::path&)> pinPlace;
      std::function<void(const std::filesystem::path&)> openProperties;
      std::function<void(const Place&)> openVolumeProperties;
      std::function<void()> requestQuit;
      std::function<void()> openNewTab;
      std::function<void()> reopenClosedTab;
      std::function<void(TabId)> closeTab;
      std::function<void(TabId)> duplicateTab;
      std::function<void(TabId)> closeOtherTabs;
      std::function<void(TabId)> closeTabsToRight;
      std::function<TabId()> contextMenuTabId;
      std::function<void(const std::filesystem::path&, bool activate)> openFolderInNewTab;
      std::function<void()> openQuickLook;
      std::function<void(PreviewMode)> setPreviewMode;
      std::function<void(ThumbnailPolicy)> setThumbnailPolicy;
      std::function<void(bool clear)> inspectThumbnailCache;
      std::function<void(const std::filesystem::path&)> goToLinkTarget;
      std::function<void(const std::filesystem::path&)> navigate;
      std::function<void()> navigateTrash;
      std::function<void(const std::filesystem::path&)> enumerate;
      std::function<void(bool)> setAlwaysShowTabBar;
      std::function<void(bool)> setOpenTabsInBackground;
      std::function<void(int)> handleLocationInputAction;
      std::function<void(BrowserTab&)> recordAdminTabUserActivity;
      std::function<void(SelectionChange)> changeSelection;
      std::function<void()> duplicateSelection;
      std::function<void()> createSelectionSymlink;
      std::function<void()> showSymlinkDialog;
      std::function<void(bool chooseDestination)> restoreWastebinSelection;
      std::function<void()> confirmEmptyWastebin;
      std::function<void(bool cut)> copyToDesktopClipboard;
      std::function<void()> beginRename;
      std::function<void()> confirmPermanentDelete;
      std::function<void(int id)> createNewItem;
      std::function<void(bool grid)> setGridMode;
      std::function<void(float scale)> adjustViewScale;
      std::function<void(FileDialogSortField)> setSortField;
      std::function<void()> toggleSortOrder;
      std::function<void()> toggleSearchHidden;
      std::function<void()> persistFileViewState;
      std::function<void()> syncSortOrHiddenEntries;
    };

    FileMenuController(
        WaylandConnection& wayland, RenderContext& renderContext, AnimationManager& menuAnimations,
        InputDispatcher& dispatcher, FileManagerSettings& settings, FileOperationManager& operations, TabManager& tabs,
        FileListHeaderController& listHeader, PlacesSidebarController& places, ChecksumDialogController& checksumDialog,
        HelpDialogController& helpDialog, ProtectedFileManager& protectedFiles, FileLaunchController& launcher,
        FileClipboardController& clipboard, NativeDataTransfer& transfer, Callbacks callbacks
    );
    ~FileMenuController();

    void loadInitialPreference();
    void initializePopups(ConfigService& configService, LayerPopupHostRegistry& pickerHosts);
    void attachMenuBarUi(Flex& appBar, float toolbarHeight);
    void detachUi();
    void closeAll();
    void closeContextMenu();

    [[nodiscard]] bool isContextMenuOpen() const;
    [[nodiscard]] bool hasOpenPopup() const;

    bool handlePointer(const PointerEvent& event);
    bool handleKeyboard(const KeyboardEvent& event);

    void activateEntry(const ContextMenuControlEntry& entry);

    void syncResponsiveMenuBar(int surfaceWidth);
    void toggleMenuBarVisibility();
    void updateApplicationMenuUnderline();

    bool openApplicationMenu(float x, float y, std::size_t menuIndex = 0, bool highlightOnOpen = false);
    bool openApplicationMenuFromKeyboard(std::size_t menuIndex = 0);
    void openContextMenu(std::size_t index, float x, float y, bool highlightOnOpen = false);
    void openEmptyContextMenu(float x, float y, bool highlightOnOpen = false);
    void showPlacesMenu(std::vector<ContextMenuControlEntry> entries, float x, float y, bool highlightOnOpen = false);
    [[nodiscard]] std::vector<ContextMenuControlEntry> columnVisibilityEntries() const;
    void
    openColumnHeaderMenu(std::vector<ContextMenuControlEntry> entries, float x, float y, bool highlightOnOpen = false);
    void
    openTabContextMenu(std::vector<ContextMenuControlEntry> entries, float x, float y, bool highlightOnOpen = false);
    void openSearchDropdown(std::vector<ContextMenuControlEntry> entries, const Node& trigger, bool highlightOnOpen);
    void setNextPlacesHighlightOnOpen(bool highlight) noexcept { m_nextPlacesHighlightOnOpen = highlight; }
    [[nodiscard]] bool applicationMenuCollapsed() const noexcept { return m_applicationMenuCollapsed; }

    [[nodiscard]] ContextMenuControlEntry newItemMenu() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> fileMenuEntries() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> editMenuEntries() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> viewMenuEntries() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> toolsMenuEntries();
    [[nodiscard]] std::vector<ContextMenuControlEntry> archiveMenuEntries() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> helpMenuEntries() const;
    [[nodiscard]] std::optional<std::vector<ContextMenuControlEntry>> applicationMenuEntries(std::size_t menuIndex);

    [[nodiscard]] const std::optional<std::filesystem::path>& contextMenuTarget() const noexcept {
      return m_contextMenuTarget;
    }
    void setContextMenuTarget(std::optional<std::filesystem::path> target) { m_contextMenuTarget = std::move(target); }

  private:
    void openMainContextMenu(ContextMenuPopupRequest request, bool highlightOnOpen, bool preferSurfaceOutput);
    bool dispatchSearchAndArchiveAction(const ContextMenuControlEntry& entry);
    bool dispatchToolsAndAdminAction(const ContextMenuControlEntry& entry);
    bool dispatchFileAndLaunchAction(const ContextMenuControlEntry& entry);
    bool dispatchTabAndPreviewAction(const ContextMenuControlEntry& entry);
    bool dispatchPlacesAction(const ContextMenuControlEntry& entry);
    bool dispatchSelectionAndClipboardAction(const ContextMenuControlEntry& entry);
    bool dispatchFileMutationAction(const ContextMenuControlEntry& entry);
    bool dispatchViewAndSortAction(const ContextMenuControlEntry& entry);

    friend class ::FileManagerTestAccess;

    void persistApplicationMenuPreference();

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    AnimationManager& m_menuAnimations;
    InputDispatcher& m_dispatcher;
    FileManagerSettings& m_settings;
    FileOperationManager& m_operations;
    TabManager& m_tabs;
    FileListHeaderController& m_listHeader;
    PlacesSidebarController& m_places;
    ChecksumDialogController& m_checksumDialog;
    HelpDialogController& m_helpDialog;
    ProtectedFileManager& m_protectedFiles;
    FileLaunchController& m_launcher;
    FileClipboardController& m_clipboard;
    NativeDataTransfer& m_transfer;
    Callbacks m_callbacks;

    std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;

    Flex* m_appMenuItems = nullptr;
    Button* m_fileMenuButton = nullptr;
    std::array<Button*, 5> m_applicationMenuButtons{};
    Box* m_applicationMenuUnderline = nullptr;
    AnimationManager::Id m_menuUnderlineAnimation = 0;
    float m_menuUnderlineStartX = 0.0F;
    float m_menuUnderlineStartWidth = 0.0F;
    std::optional<std::size_t> m_applicationMenuOpen;
    std::optional<std::size_t> m_applicationMenuHovered;
    Button* m_compactMenuButton = nullptr;
    bool m_showApplicationMenuBar = true;
    bool m_applicationMenuCollapsed = false;
    bool m_nextPlacesHighlightOnOpen = false;

    std::optional<std::filesystem::path> m_contextMenuTarget;
  };

} // namespace filemanager
