#include "filemanager/file_manager.h"

#include "app/timer_poll_source.h"
#include "core/deferred_call.h"
#include "core/files/display_text.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/text_clipboard.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/byte_size_format.h"
#include "filemanager/file_breadcrumbs.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_creation.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_list_columns.h"
#include "filemanager/file_sort.h"
#include "filemanager/file_transfer_payload.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/folder_size.h"
#include "filemanager/path_autocomplete.h"
#include "filemanager/permanent_delete_prompt.h"
#include "filemanager/property_operations.h"
#include "filemanager/startup_path.h"
#include "i18n/i18n_service.h"
#include "render/animation/motion_service.h"
#include "render/animation/surface_haze.h"
#include "render/core/renderer.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "system/icon_resolver.h"
#include "theme/builtin_palettes.h"
#include "theme/fixed_palette.h"
#include "theme/fixed_palette_json.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/controls/spinner.h"
#include "ui/dialogs/dialog_background_artwork.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"
#include "ui/dialogs/file_icon.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "ui/style.h"
#include "ui/tooltip/tooltip_manager.h"
#include "util/string_utils.h"
#include "wayland/key_repeat_poll_source.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <future>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <tuple>
#include <utility>
#include <wayland-client-protocol.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr float kFilesSelectionOverlayOpacity = 0.42F;
  constexpr float kFilesAppBarOpacity = 0.72F;
  constexpr float kFilesWindowOpacity = 0.88F;
  constexpr float kFilesSidebarOpacity = 0.62F;
  constexpr float kFilesLocationBarOpacity = 0.78F;
  constexpr float kFilesGridOpacity = 0.38F;
  constexpr float kFilesStatusStripOpacity = 0.82F;
  constexpr float kResizeDividerHoverOpacity = 0.62F;

  using namespace filemanager::menu_id;

  using filemanager::columnIndex;
  using filemanager::FileColumn;

  FileManager* g_activeInputHooksOwner = nullptr;

  constexpr std::array kManagedKeybindActions{
      KeybindAction::Validate, KeybindAction::Cancel, KeybindAction::Up,      KeybindAction::Down,
      KeybindAction::Left,     KeybindAction::Right,  KeybindAction::TabNext, KeybindAction::TabPrevious,
      KeybindAction::Copy,     KeybindAction::Save,   KeybindAction::Delete,
  };

  bool isNodeDescendantOf(const Node* node, const Node* ancestor) noexcept {
    if (ancestor == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  void collectFocusableDescendants(Node* node, std::vector<InputArea*>& out) {
    if (node == nullptr || !node->visible()) {
      return;
    }
    if (auto* area = dynamic_cast<InputArea*>(node);
        area != nullptr && area->enabled() && area->focusable() && area->tabStop()) {
      out.push_back(area);
    }
    for (const auto& child : node->children()) {
      collectFocusableDescendants(child.get(), out);
    }
  }

} // namespace

class FileManager::NativeTextClipboard final : public TextClipboard {
public:
  NativeTextClipboard(NativeDataTransfer& transfer, std::function<void(std::string)> writeText)
      : m_transfer(transfer), m_writeText(std::move(writeText)) {}

  [[nodiscard]] std::optional<std::string> clipboardText() override { return std::nullopt; }

  void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override {
    m_transfer.readTextSelection(std::move(callback));
  }

  void setClipboardText(std::string text) override {
    if (m_writeText) {
      m_writeText(std::move(text));
    }
  }

private:
  NativeDataTransfer& m_transfer;
  std::function<void(std::string)> m_writeText;
};

class FileManager::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(FileManager& owner) : m_owner(owner) {}
  std::size_t itemCount() const override { return m_owner.activeSession().entries().size(); }
  std::optional<bool> selectedState(std::size_t index) const override {
    const auto& tab = m_owner.activeTab();
    if (index >= tab.session.entries().size())
      return false;
    return tab.isPathSelected(tab.session.entries()[index].absPath.string());
  }
  std::unique_ptr<Node> createTile() override {
    if (m_owner.m_listHeader.gridMode()) {
      auto tile = std::make_unique<FileEntryTile>(
          1.0F, &m_owner.m_pickerThumbnails, filemanager::fileViewMetrics(m_owner.m_listHeader.viewScale()).gridIcon
      );
      tile->setSelectionRoles(ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer);
      return tile;
    }
    auto row = std::make_unique<FileEntryRow>(
        1.0F, filemanager::fileViewMetrics(m_owner.m_listHeader.viewScale()).listIcon, &m_owner.m_pickerThumbnails
    );
    row->setSelectionRoles(ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer);
    return row;
  }
  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    auto& tab = m_owner.activeTab();
    auto& session = m_owner.activeSession();
    if (index >= session.entries().size() || m_renderer == nullptr)
      return;
    const auto& entry = session.entries()[index];
    const bool searchResult = session.search().active();
    const auto location = searchResult ? filemanager::searchLocationText(entry.absPath) : std::string{};
    // Search provides viewport thumbnails separately from the shared artwork lookup.
    const bool allowThumbnails = !searchResult && tab.capabilities().canGenerateThumbnails;
    const auto useListingArtwork = [&entry, searchResult, archive = session.browsingArchive()](FileIcon* icon) {
      if (!searchResult && !archive)
        return;
      const bool broken = entry.isSymlink && entry.type == "inode/symlink";
      std::vector<std::string> names;
      if (!broken && !entry.isDir && !entry.type.empty()) {
        g_autoptr(GIcon) themed = g_content_type_get_icon(entry.type.c_str());
        if (themed && G_IS_THEMED_ICON(themed)) {
          for (const auto* name = g_themed_icon_get_names(G_THEMED_ICON(themed)); name && *name; ++name)
            names.emplace_back(*name);
        }
      }
      names.emplace_back(entry.isDir ? "folder" : "text-x-generic");
      icon->setThemedIconNames(std::move(names));
      icon->setSymlinkState(entry.isSymlink, broken);
    };
    const bool manageThumbnail =
        tab.hasActiveAdminSession() && m_owner.m_settings.thumbnailPolicy() != filemanager::ThumbnailPolicy::Never;
    const auto protectedThumbnail =
        manageThumbnail ? m_owner.m_protectedFiles.thumbnailPath(tab, entry, index) : std::nullopt;
    const bool cut =
        !session.browsingTrash() && tab.capabilities().canMutate && m_owner.m_clipboard.isPathCut(entry.absPath);
    const bool gridFocused =
        m_owner.m_grid != nullptr && m_owner.m_grid->focusArea() != nullptr && m_owner.m_grid->focusArea()->focused();
    const bool itemFocused =
        m_owner.m_selection.focusVisible() && gridFocused && m_owner.m_grid->selectedIndex() == index;
    if (m_owner.m_listHeader.gridMode()) {
      auto& tileNode = static_cast<FileEntryTile&>(tile);
      tileNode.setAllowThumbnailsAndMetadata(allowThumbnails);
      tileNode.setManageThumbnail(manageThumbnail);
      tileNode.bind(
          *m_renderer, entry, index, tile.width(), tile.height(), selected,
          hovered || m_owner.m_transfers.dropIndex() == index, false, cut, protectedThumbnail, itemFocused
      );
      if (!location.empty())
        tileNode.setSearchLocation(location);
      useListingArtwork(tileNode.fileIcon());
    } else {
      auto& row = static_cast<FileEntryRow&>(tile);
      row.setAllowThumbnailsAndMetadata(allowThumbnails);
      row.setManageThumbnail(manageThumbnail);
      row.setColumns(m_owner.m_listHeader.visibleColumns(), m_owner.m_listHeader.listColumns());
      row.bind(
          *m_renderer, entry, index, tile.width(), selected, hovered || m_owner.m_transfers.dropIndex() == index, false,
          session.folderItemCount(entry), cut, protectedThumbnail, itemFocused
      );
      if (!location.empty()
          && m_owner.m_listHeader.columnLayout().visible[columnIndex(filemanager::kSearchLocationSlot)])
        row.setSearchLocation(location);
      row.setColumnViewport(
          *m_renderer, std::max(tile.width(), m_owner.m_listHeader.contentWidth()), m_owner.m_listHeader.scrollOffset()
      );
      useListingArtwork(row.fileIcon());
    }
  }
  void onActivate(std::size_t index) override {
    if (index < m_owner.activeSession().entries().size())
      m_owner.activatePointerItem(index);
  }
  void onMiddleActivate(std::size_t index) override { m_owner.activatePointerMiddleItem(index); }
  std::string itemTooltip(std::size_t index) const override {
    const auto& session = m_owner.activeSession();
    if (index >= session.entries().size())
      return {};
    const auto& entry = session.entries()[index];
    if (!m_owner.m_listHeader.gridMode() && m_owner.m_hoveredFileColumn && m_renderer) {
      const auto column = *m_owner.m_hoveredFileColumn;
      const auto value = filemanager::fileColumnText(session, entry, column);
      const auto width = m_owner.m_listHeader.listColumns()[column] - m_owner.m_listHeader.columnTextInset(column);
      if (!value.empty()
          && filemanager::measureListColumnText(*m_renderer, column, session.search().active(), value) > width)
        return value;
    }
    auto found = session.trashOriginalPaths().find(entry.absPath.string());
    const auto description = guessFileTypeDescription(entry.absPath, entry.isDir, entry.type);
    auto tooltip = filemanager::sanitizeFilenameForDisplay(entry.name) + "\n" + description;
    if (session.search().active())
      tooltip += "\n" + filemanager::searchLocationText(entry.absPath);
    return found == session.trashOriginalPaths().end()
        ? tooltip
        : tooltip + "\nOriginal location: " + filemanager::sanitizeFilenameForDisplay(found->second);
  }
  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    m_owner.m_menus.openContextMenu(index, anchorX, anchorY);
  }
  void onSecondaryEmptyActivate(float anchorX, float anchorY) override {
    m_owner.m_menus.openEmptyContextMenu(anchorX, anchorY);
  }
  InputArea* beginInlineEdit(
      Node& tile, const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
      std::function<void()> cancel
  ) override {
    const auto& entries = m_owner.activeSession().entries();
    const auto found = std::ranges::find(entries, std::filesystem::path(path), &FileEntry::absPath);
    const bool isDir = found != entries.end() && found->isDir;
    if (m_owner.m_listHeader.gridMode())
      return static_cast<FileEntryTile&>(tile).beginInlineEdit(path, name, std::move(submit), std::move(cancel), isDir);
    return static_cast<FileEntryRow&>(tile).beginInlineEdit(path, name, std::move(submit), std::move(cancel), isDir);
  }
  void endInlineEdit(Node& tile, const std::string& path) override {
    if (m_owner.m_listHeader.gridMode())
      static_cast<FileEntryTile&>(tile).endInlineEdit(path);
    else
      static_cast<FileEntryRow&>(tile).endInlineEdit(path);
  }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }

private:
  FileManager& m_owner;
  Renderer* m_renderer = nullptr;
};

namespace {
  constexpr float kToolbarHeight = 32.0F;
  constexpr float kWindowPadding = 8.0F;
  constexpr float kWindowResizeInset = 6.0F;
  constexpr float kWindowResizeCorner = kWindowPadding;
  struct WindowResizeHandle {
    std::uint32_t edge;
    std::uint32_t cursor;
  };
  constexpr std::array kWindowResizeHandles{
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_TOP, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
      WindowResizeHandle{XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
  };
  constexpr float kPaneGap = 2.0F;
  constexpr float kSidebarDividerWidth = 4.0F;
  constexpr float kSidebarMinWidth = 150.0F;
  constexpr float kSidebarMaxWidth = 360.0F;
  constexpr float kStatusMinHeight = 30.0F;
  constexpr float kStatusMaxHeight = 76.0F;

  struct CompactSize {
    double value = 0.0;
    std::string unit;
    std::size_t unitIndex = 0;
  };

  CompactSize compactSize(std::uint64_t bytes) {
    const auto scaled = filemanager::compactBinaryByteSize(bytes);
    return {scaled.value, std::string(scaled.unit), scaled.unitIndex};
  }

  void useSubtleSelection(Button& button, bool muted = false) {
    auto palette = Button::defaultPalette(button.variant());
    palette.selected = Button::ButtonStateColors{
        .bg = colorSpecFromRole(ColorRole::PrimaryContainer, kFilesSelectionOverlayOpacity),
        .border = clearColorSpec(),
        .label = colorSpecFromRole(ColorRole::OnPrimaryContainer),
    };
    if (muted) {
      for (auto* state : {&palette.normal, &palette.hover, &palette.pressed})
        state->label = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    }
    button.setCustomPalette(std::move(palette));
  }

  std::unique_ptr<Button> makeButton(std::string_view text, std::function<void()> action, Button** out = nullptr) {
    auto button = std::make_unique<Button>();
    button->setText(text);
    button->setVariant(ButtonVariant::Ghost);
    button->setControlHeight(kToolbarHeight);
    button->setPadding(5.0F);
    button->setOnClick(std::move(action));
    if (out != nullptr)
      *out = button.get();
    return button;
  }

  filemanager::TabViewState loadInitialTabViewState(filemanager::FileManagerSettings& settings, bool search = false) {
    const auto view = settings.loadFileView(search);
    return filemanager::TabViewState{
        .gridMode = view.gridMode,
        .showHidden = view.showHidden,
        .foldersFirst = view.foldersFirst,
        .hiddenLast = view.hiddenLast,
        .sortField = view.sortField,
        .sortOrder = view.sortOrder,
        .viewScale = settings.loadViewScale(filemanager::kFileViewDefaultScale, search),
    };
  }

} // namespace

filemanager::TabManager::Callbacks FileManager::makeTabManagerCallbacks() {
  return filemanager::TabManager::Callbacks{
      .onBeforeTabSwitch =
          [this](filemanager::BrowserTab& outgoing, filemanager::BrowserTab&) {
            m_searchThumbnails.clear();
            m_protectedFiles.clearThumbnails();
            if (m_refreshAfterRename) {
              outgoing.needsRefresh = true;
              m_refreshAfterRename = false;
            }
            if (m_renamePath)
              cancelRename();
            m_pendingInlineRenamePath.reset();
            m_inlineRenameTarget.reset();
            setLocationEditMode(false);
            cancelFileGesture();
            m_listHeader.cancelGesture();
            m_menus.closeContextMenu();
            m_selection.resetForTabSwitch();
            if (m_grid != nullptr)
              m_grid->resetTransientStateForDataSwap();
          },
      .onActiveTabChanged =
          [this](filemanager::BrowserTab& active, const filemanager::TabViewState&) {
            recordLocationVisit(active);
            applyActiveView();
            m_statusMessage = active.lastStatusMessage;
            m_statusMessageTimer.stop();
            if (!m_statusMessage.empty())
              m_statusMessageUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            else
              m_statusMessageUntil = {};
            const bool searchView = active.session.search().active();
            const bool needsRefresh = active.needsRefresh;
            if (needsRefresh) {
              if (!searchView)
                active.pendingScrollOffsetAfterRefresh = active.displayedScrollState().offset;
              active.needsRefresh = false;
              active.session.refreshPreservingSelection(false);
            }
            if (!needsRefresh || searchView) {
              if (m_grid != nullptr) {
                m_grid->notifyDataChanged();
                restoreSelection(false);
              }
              if (active.pendingPasteSelection && !active.session.loading())
                m_clipboard.applyPendingPasteSelection(active);
              if (active.pendingRevealOnActivate) {
                active.pendingRevealOnActivate = false;
                if (m_grid != nullptr
                    && active.selectedPath()
                    && (!active.pendingDropScrollGeneration
                        || *active.pendingDropScrollGeneration == active.displayedScrollState().userScrollGeneration)) {
                  const auto entry =
                      std::ranges::find(active.session.entries(), *active.selectedPath(), &FileEntry::absPath);
                  if (entry != active.session.entries().end())
                    m_grid->scrollToIndex(static_cast<std::size_t>(entry - active.session.entries().begin()));
                }
              }
            }
            rebuildBreadcrumbs();
            if (m_locationInput != nullptr)
              m_locationInput->setValue(active.session.path().string());
            updateNavigationControls();
            updateStatusBar();
            updateWindowTitle();
            updateAdminHeaderUi();
            m_tabBar.markDirty();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onTabsChanged =
          [this] {
            updateAdminHeaderUi();
            m_tabBar.markDirty();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onTabMetadataChanged =
          [this](filemanager::TabId tabId) {
            m_tabBar.markDirty();
            if (tabId == m_tabs.activeTabId()) {
              updateWindowTitle();
              updateAdminHeaderUi();
            }
            if (m_surface)
              m_surface->requestLayout();
          },
      .onSearchActivityChanged = [this](filemanager::TabId id) { m_tabBar.syncSearchActivity(id); },
      .onRequestCloseLastTab = [this] { requestQuit(m_surface.get()); },
      .isSizeColumnVisible = [this] { return m_listHeader.columnLayout().visible[columnIndex(FileColumn::Size)]; },
      .isRenaming =
          [this](filemanager::TabId tabId) { return tabId == m_tabs.activeTabId() && m_renamePath.has_value(); },
      .onDeferRefreshDuringRename =
          [this](filemanager::TabId tabId) {
            if (tabId == m_tabs.activeTabId())
              m_refreshAfterRename = true;
            else if (auto* tab = m_tabs.findTab(tabId))
              tab->needsRefresh = true;
          },
      .onCancelFileGesture = [this] { cancelFileGesture(); },
      .onReorderSearchResults =
          [this] {
            if (m_transfers.isDragPending() || m_transfers.isDraggingFiles() || (m_grid && m_grid->rubberBandActive()))
              cancelFileGesture();
          },
      .onActiveBeforeNavigate =
          [this](filemanager::BrowserTab& tab, const std::filesystem::path&, bool pathChanged) {
            if (tab.session.search().active())
              resetSearchSelection();
            if (pathChanged || tab.session.searchOpen()) {
              if (m_renamePath)
                cancelRename();
              m_pendingInlineRenamePath.reset();
              m_selection.clearTypeahead();
            }
            m_selection.clearLastPointerIndex();
          },
      .onActiveBeforePathChange =
          [this](filemanager::BrowserTab&, const std::filesystem::path&) {
            if (m_grid != nullptr) {
              m_grid->setSelectedIndex(std::nullopt, false);
              m_grid->notifyDataChanged();
            }
          },
      .onActiveAfterNavigatePathSet =
          [this](filemanager::BrowserTab& active, const std::filesystem::path& path, bool) {
            if (m_locationInput != nullptr)
              m_locationInput->setValue(path.string());
            rebuildBreadcrumbs();
            updateNavigationControls();
            updateWindowTitle();
            m_tabBar.markDirty();
            recordLocationVisit(active);
            if (m_settings.addressBarMode() == filemanager::AddressBarMode::FullPath && !m_editingLocation) {
              if (m_locationInput != nullptr) {
                m_locationInput->setVisible(true);
                m_locationInput->setParticipatesInLayout(true);
              }
              if (m_breadcrumbBar != nullptr) {
                m_breadcrumbBar->setVisible(false);
                m_breadcrumbBar->setParticipatesInLayout(false);
              }
            }
          },
      .onActiveEnumerateStarted =
          [this](filemanager::BrowserTab&, const std::filesystem::path& path, bool) {
            setStatus("Loading…");
            rebuildBreadcrumbs();
            if (m_locationInput != nullptr)
              m_locationInput->setValue(path.string());
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveEntriesLoaded =
          [this](filemanager::BrowserTab& active, bool chooseFirst) {
            m_searchThumbnails.clear();
            syncSearchUi();
            if (m_grid != nullptr) {
              m_grid->notifyDataChanged();
              if (active.pendingScrollOffsetAfterRefresh.has_value()) {
                const float savedOffset = *active.pendingScrollOffsetAfterRefresh;
                active.pendingScrollOffsetAfterRefresh.reset();
                m_grid->scrollView().requestScrollToOffset(savedOffset);
              }
            }
            restoreSelection(chooseFirst);
            updateStatusBar();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveEntriesChanged =
          [this](filemanager::BrowserTab& tab) {
            if (!tab.session.search().active() || tab.session.entries().empty())
              m_searchThumbnails.clear();
            syncSearchUi();
            if (m_grid != nullptr) {
              m_grid->notifyDataChanged();
              restoreSelection();
            }
            updateStatusBar();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveEntryChangedAt =
          [this](filemanager::BrowserTab&, std::size_t index) {
            if (m_grid != nullptr) {
              m_grid->notifyItemChanged(index);
              if (m_surface)
                m_surface->requestLayout();
            }
          },
      .onActiveMonitorQueryCompleted =
          [this](filemanager::BrowserTab& active) {
            if (active.pendingPasteRevealPath && m_surface)
              m_surface->requestLayout();
          },
      .onActiveStatus =
          [this](filemanager::BrowserTab&, std::string text) {
            setStatus(std::move(text));
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveStatusInfoChanged =
          [this](filemanager::BrowserTab&) {
            updateStatusBar();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onApplyPendingPasteSelection =
          [this](filemanager::BrowserTab& tab) { m_clipboard.applyPendingPasteSelection(tab); },
      .onAnyBeforePathChange =
          [this](filemanager::BrowserTab& tab, const std::filesystem::path&) {
            (void)tab.adminSession.cancelAuthentication();
            m_clipboard.onBeforePathChange(tab);
            m_transfers.onBeforePathChange(tab);
          },
      .onAnyEnumerateStarted =
          [this](filemanager::BrowserTab& tab, const std::filesystem::path&, bool) {
            m_clipboard.onEnumerateStarted(tab);
            m_tabBar.markDirty();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onTabAdminSessionChanged =
          [this](filemanager::BrowserTab& tab, const filemanager::AdminSessionStatus& status) {
            if (status.state == filemanager::AdminSessionState::Failed && tab.administratorBrowsing) {
              // If the session was already active and failed (inactivity expiry, unexpected termination,
              // or protocol failure), recover the tab back to unprivileged browsing.
              finishAdministratorMode(tab.id, status.message.empty() ? "Administrator Mode ended." : status.message);
              return;
            }
            m_tabBar.markDirty();
            if (tab.id == m_tabs.activeTabId()) {
              updateWindowTitle();
              updateAdminHeaderUi();
            }
            if (m_surface)
              m_surface->requestLayout();
          },
      .onInvalidateMountCapacities =
          [this](const std::filesystem::path& path) { m_places.invalidateMountCapacitiesForPath(path); },
      .onResolvePathAsync = [this](const std::filesystem::path& path) { m_pathCache.resolveAsync(path); },
      .archivePassword = m_modalDialog.makeArchivePasswordResolver(),
      .recentItems =
          [this] {
            if (!m_settings.recentActivityEnabled())
              return std::vector<filemanager::RecentActivityItem>{};
            (void)m_recentActivity.load();
            return m_recentActivity.recentItems();
          },
  };
}

FileManager::FileManager(std::string initialPath)
    : FileManager(
          StartupOptions{
              .explicitPath = initialPath.empty() ? std::nullopt : std::optional<std::filesystem::path>(initialPath),
              .forceNewWindow = true,
          }
      ) {}

FileManager::FileManager(StartupOptions options)
    : m_startupOptions(std::move(options)), m_operations([this](const FileOperationManager::Operation& operation) {
        if (operation.adminOrigin) {
          const auto& origin = *operation.adminOrigin;
          auto* tab = m_tabs.findTab(origin.tabId);
          const bool sameSession = m_windowOpen
              && tab
              && tab->hasActiveAdminSession()
              && tab->adminSession.sessionGeneration() == origin.sessionGeneration;
          const bool sameContext = sameSession
              && tab->session.navigationGeneration() == origin.navigation
              && tab->session.path() == origin.directory;
          if (sameContext && tab->active)
            setStatus(operation.status);
          else if (sameSession && operation.finished)
            tab->lastStatusMessage = operation.status;

          if (operation.finished) {
            using Kind = FileOperationManager::Kind;
            if (m_inlineRenameTarget && m_inlineRenameTarget->operationId == operation.id) {
              m_inlineRenameTarget.reset();
              if (sameContext && tab->active && operation.succeeded && m_renamePath == operation.source) {
                m_refreshAfterRename = false;
                cancelRename();
              }
            }
            if (sameContext && operation.succeeded) {
              if (operation.kind == Kind::Rename) {
                tab->renameSelectedPath(operation.source, operation.destination);
              } else if (operation.kind == Kind::CreateFolder || operation.kind == Kind::CreateFile) {
                tab->selectSingle(operation.destination);
                if (tab->active)
                  m_pendingInlineRenamePath = operation.destination;
              }
            }
            m_transfers.onOperationFinished(operation);
            m_clipboard.onOperationFinished(operation);
            if (operation.mutated || operation.outcomeUncertain) {
              for (const auto& other : m_tabs.tabs()) {
                if (!m_windowOpen || other->session.browsingTrash() || (other.get() == tab && !sameSession))
                  continue;
                const auto& directory = other->session.path();
                if (directory != origin.directory
                    && (operation.source.empty() || directory != std::filesystem::path(operation.source).parent_path())
                    && std::ranges::none_of(operation.movedSources, [&](const auto& source) {
                         return directory == std::filesystem::path(source).parent_path();
                       }))
                  continue;
                if (sameContext && other.get() == tab)
                  tab->operationResultAfterRefresh = operation.status;
                if (other->active) {
                  other->session.setNextEnumerateUserInitiated(false);
                  other->session.refreshPreservingSelection();
                } else {
                  other->needsRefresh = true;
                }
              }
              m_places.refreshCapacities();
            }
            if (!m_operations.active() && ((!m_windowOpen && m_clipboard.pendingClipboardReads() == 0) || m_quitting))
              m_running = false;
          }
          syncProgressWindow();
          return;
        }
        if (operation.finished) {
          setStatus(operation.status);
          if (operation.succeeded && operation.kind == FileOperationManager::Kind::Rename) {
            for (const auto& tabPtr : m_tabs.tabs())
              tabPtr->renameSelectedPath(operation.source, operation.destination);
          }
          if (m_inlineRenameTarget && m_inlineRenameTarget->operationId == operation.id) {
            const auto renameTarget = *m_inlineRenameTarget;
            m_inlineRenameTarget.reset();
            if (operation.succeeded
                && renameTarget.tabId == m_tabs.activeTabId()
                && m_renamePath == std::filesystem::path(operation.source)) {
              // The completion below refreshes once, after selection follows the new path.
              activeSession().clearPendingMonitorChanges();
              m_refreshAfterRename = false;
              cancelRename();
            }
          }
          if (const auto createIt = m_createRequests.find(operation.id); createIt != m_createRequests.end()) {
            const auto createReq = createIt->second;
            m_createRequests.erase(createIt);
            if (operation.succeeded
                && (operation.kind == FileOperationManager::Kind::CreateFile
                    || operation.kind == FileOperationManager::Kind::CreateFolder)) {
              if (auto* originTab = m_tabs.findTab(createReq.tabId)) {
                if (originTab->session.navigationGeneration() == createReq.navigation
                    && std::filesystem::path(operation.destination).parent_path() == originTab->session.path()) {
                  originTab->selectSingle(operation.destination, false);
                  if (originTab->id == m_tabs.activeTabId())
                    m_pendingInlineRenamePath = operation.destination;
                }
              }
            }
          }
          m_transfers.onOperationFinished(operation);
          const bool pastedEntries = m_clipboard.onOperationFinished(operation);
          const bool wastebinOperation = operation.kind == FileOperationManager::Kind::Restore
              || operation.kind == FileOperationManager::Kind::EmptyWastebin;
          const bool anyTrashChange = wastebinOperation
              || operation.kind == FileOperationManager::Kind::Trash
              || operation.kind == FileOperationManager::Kind::TrashToHome;
          if (!operation.succeeded && !operation.cancelled && operation.trashFallback.has_value() && m_windowOpen) {
            m_trashFallbackDialog.show(
                *operation.trashFallback,
                [this](std::vector<std::string> sources) {
                  if (!sources.empty() && m_windowOpen)
                    m_operations.trashToHomeWastebin(std::move(sources));
                },
                [this](std::vector<std::string> sources) {
                  if (sources.empty() || !m_windowOpen)
                    return;
                  const auto prompt = filemanager::permanentDeletePrompt(sources, sources.size());
                  showDialog(
                      "Permanently delete?", prompt,
                      {{"Cancel", 0, filemanager::DialogChoiceStyle::Cancel},
                       {"Delete", 1, filemanager::DialogChoiceStyle::Destructive}},
                      [this, sources = std::move(sources)](int response) mutable {
                        if (response == 1 && m_windowOpen)
                          m_operations.removePermanently(std::move(sources));
                      },
                      nullptr, filemanager::DialogStyle::Warning
                  );
                }
            );
          }
          const bool dirtyChange = operation.succeeded || operation.mutated || wastebinOperation || pastedEntries;
          if (dirtyChange && m_windowOpen) {
            if (wastebinOperation || operation.undoUnavailable || !operation.succeeded)
              activeTab().operationResultAfterRefresh = operation.status;
            enumerate(activeSession().path());
          }
          if (dirtyChange) {
            const auto activeId = m_tabs.activeTabId();
            const bool historyReplay = operation.kind == FileOperationManager::Kind::Undo
                || operation.kind == FileOperationManager::Kind::Redo;
            const auto matchesDir = [this](const std::filesystem::path& tabPath, const std::filesystem::path& dir) {
              if (dir.empty())
                return false;
              if (filemanager::normalizeTabPath(tabPath) == filemanager::normalizeTabPath(dir))
                return true;
              const auto same = m_pathCache.sameDirectoryPathCached(tabPath, dir);
              return same.value_or(true);
            };
            const auto srcParent = operation.source.empty() ? std::filesystem::path{}
                                                            : std::filesystem::path(operation.source).parent_path();
            const std::filesystem::path dstPath(operation.destination);
            const auto dstParent = dstPath.empty() ? std::filesystem::path{} : dstPath.parent_path();
            for (const auto& tabPtr : m_tabs.tabs()) {
              if (tabPtr->id == activeId)
                continue;
              if (tabPtr->session.browsingTrash()) {
                if (anyTrashChange || historyReplay)
                  tabPtr->needsRefresh = true;
                continue;
              }
              if (tabPtr->session.search().active()
                  || historyReplay
                  || matchesDir(tabPtr->session.path(), srcParent)
                  || matchesDir(tabPtr->session.path(), dstPath)
                  || matchesDir(tabPtr->session.path(), dstParent)) {
                tabPtr->needsRefresh = true;
                continue;
              }
              for (const auto& completed : operation.completedEntries) {
                if (matchesDir(tabPtr->session.path(), std::filesystem::path(completed.path).parent_path())) {
                  tabPtr->needsRefresh = true;
                  break;
                }
              }
            }
          }
          if (operation.succeeded || operation.mutated)
            m_places.refreshCapacities();
          if (!m_operations.active() && !m_windowOpen && m_clipboard.pendingClipboardReads() == 0)
            m_running = false;
          if (!m_operations.active() && m_quitting)
            m_running = false;
        } else {
          setStatus(operation.status);
        }
        syncProgressWindow();
      }),
      m_adapter(std::make_unique<Adapter>(*this)),
      m_tabs(initialWindowLocation(), loadInitialTabViewState(m_settings), makeTabManagerCallbacks()),
      m_menus(
          m_wayland, m_renderContext, m_menuAnimations, m_dispatcher, m_settings, m_operations, m_tabs, m_listHeader,
          m_places, m_checksumDialog, m_helpDialog, m_protectedFiles, m_launcher, m_clipboard, m_transfer,
          {
              .mainSurface = [this] { return m_surface.get(); },
              .lastInputSerial = [this] { return m_lastInputSerial; },
              .setLastInputSerial = [this](std::uint32_t serial) { m_lastInputSerial = serial; },
              .isModalOpen = [this] { return m_modalDialog.isOpen(); },
              .isRenaming = [this] { return m_renamePath.has_value(); },
              .alwaysShowTabBar = [this] { return m_alwaysShowTabBar; },
              .openTabsInBackground = [this] { return m_openTabsInBackground; },
              .canChangeSelection = [this] { return canChangeSelection(); },
              .selectedPath = [this] { return activeTab().selectedPath(); },
              .selectedPaths = [this]() -> const std::unordered_set<std::string>& {
                return activeTab().selectedPaths();
              },
              .selectedPathsOrdered = [this] { return selectedPaths(); },
              .symlinkSelection = [this] { return symlinkSelection(); },
              .checksumMenuEntries = [this] { return checksumMenuEntries(); },
              .onSelectSingleForContextMenu =
                  [this](std::size_t index) { m_selection.selectSingleForContextMenu(index); },
              .onClearSelectionForEmptyMenu = [this] { m_selection.clearSelectionForEmptyMenu(); },
              .onStatus = [this](std::string text) { setStatus(std::move(text)); },
              .onRequestLayout =
                  [this] {
                    if (m_surface)
                      m_surface->requestLayout();
                  },
              .activateArchiveAction = [this](int id) { return activateArchiveAction(id); },
              .activateSearchScopeEntry = [this](
                                              const ContextMenuControlEntry& entry
                                          ) { return m_searchBarController.activateScopeEntry(entry); },
              .activateSearchFilterEntry = [this](
                                               const ContextMenuControlEntry& entry
                                           ) { return m_searchBarController.activateFilterEntry(entry); },
              .selectSearchMode = [this](filemanager::SearchMode mode) { selectSearchMode(mode); },
              .enterAdministratorMode = [this](
                                            filemanager::TabId tabId, std::optional<std::filesystem::path> target
                                        ) { (void)enterAdministratorMode(tabId, std::move(target)); },
              .showChecksumDialog = [this](bool verify) { showChecksumDialog(verify); },
              .launchNewWindow = [this](const std::filesystem::path& path) { launchNewWindow(path); },
              .activateSelected = [this] { activateSelected(); },
              .activatePath = [this](const std::filesystem::path& path) { activatePath(path); },
              .openArchiveMember =
                  [this](const std::filesystem::path& member, bool withApp) { openArchiveMember(member, withApp); },
              .pinPlace = [this](const std::filesystem::path& path) { pinPlace(path); },
              .openProperties = [this](const std::filesystem::path& path) { openProperties(path); },
              .openVolumeProperties =
                  [this](const filemanager::Place& place) {
                    if (!m_propertiesWindow)
                      m_propertiesWindow = std::make_unique<PropertiesWindow>(
                          m_wayland, m_renderContext, m_settings, m_operations, [this](std::string text) {
                            writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8");
                          }
                      );
                    if (!m_propertiesWindow->openVolume(place, m_surface.get()))
                      setStatus("Could not open Properties window");
                  },
              .requestQuit = [this] { requestQuit(m_surface.get()); },
              .openNewTab = [this] { (void)openNewTab(); },
              .reopenClosedTab = [this] { (void)reopenClosedTab(); },
              .closeTab = [this](filemanager::TabId tabId) { (void)closeTab(tabId); },
              .duplicateTab = [this](filemanager::TabId tabId) { (void)duplicateTab(tabId); },
              .closeOtherTabs = [this](filemanager::TabId tabId) { (void)closeOtherTabs(tabId); },
              .closeTabsToRight = [this](filemanager::TabId tabId) { (void)closeTabsToRight(tabId); },
              .contextMenuTabId = [this] { return m_tabBar.contextMenuTabId(); },
              .openFolderInNewTab = [this](
                                        const std::filesystem::path& path, bool activate
                                    ) { (void)openFolderInNewTab(path, activate); },
              .openQuickLook = [this] { (void)openQuickLook(); },
              .openPreferences = [this] { openPreferences(); },
              .setPreviewMode = [this](filemanager::PreviewMode mode) { setPreviewMode(mode); },
              .setThumbnailPolicy = [this](filemanager::ThumbnailPolicy policy) { setThumbnailPolicy(policy); },
              .inspectThumbnailCache = [this](bool clear) { inspectThumbnailCache(clear); },
              .goToLinkTarget = [this](const std::filesystem::path& link) { goToLinkTarget(link); },
              .navigate = [this](const std::filesystem::path& path) { navigate(path); },
              .navigateTrash = [this] { navigateTrash(); },
              .enumerate = [this](const std::filesystem::path& path) { enumerate(path); },
              .setAlwaysShowTabBar = [this](bool show) { setAlwaysShowTabBar(show); },
              .setOpenTabsInBackground = [this](bool bg) { setOpenTabsInBackground(bg); },
              .handleLocationInputAction = [this](int id) { handleLocationInputAction(id); },
              .recordAdminTabUserActivity = [this](filemanager::BrowserTab& tab) { recordAdminTabUserActivity(tab); },
              .changeSelection = [this](SelectionChange change) { changeSelection(change); },
              .duplicateSelection = [this] { duplicateSelection(); },
              .createSelectionSymlink = [this] { createSelectionSymlink(); },
              .showSymlinkDialog = [this] { showSymlinkDialog(); },
              .restoreWastebinSelection = [this](bool chooseDest) { restoreWastebinSelection(chooseDest); },
              .confirmEmptyWastebin = [this] { confirmEmptyWastebin(); },
              .copyToDesktopClipboard = [this](bool cut) { copyToDesktopClipboard(cut); },
              .beginRename = [this] { beginRename(); },
              .confirmPermanentDelete = [this] { confirmPermanentDelete(); },
              .createNewItem = [this](int id) { createNewItem(id); },
              .setGridMode = [this](bool grid) { setGridMode(grid); },
              .adjustViewScale = [this](float scale) { adjustViewScale(scale); },
              .setSortField = [this](FileDialogSortField field) { setSortField(field); },
              .toggleSortOrder = [this] { toggleSortOrder(); },
              .toggleSearchHidden = [this] { toggleSearchOption(&filemanager::SearchOptions::showHidden); },
              .persistFileViewState = [this] { persistFileViewState(); },
              .syncSortOrHiddenEntries =
                  [this] {
                    if (m_grid) {
                      m_grid->notifyDataChanged();
                      restoreSelection();
                    }
                  },
          }
      ),
      m_tabBar(
          m_tabs, m_menuAnimations, m_dispatcher, m_wayland,
          {
              .isAlwaysShowTabBar = [this] { return m_alwaysShowTabBar; },
              .homePath = [] { return std::filesystem::path(g_get_home_dir()); },
              .onNewTab = [this] { openNewTab(); },
              .onActivateTab = [this](filemanager::TabId tabId) { activateTab(tabId); },
              .onCloseTab = [this](filemanager::TabId tabId) { closeTab(tabId); },
              .onReorderTab =
                  [this](filemanager::TabId tabId, std::size_t targetIndex) { m_tabs.reorderTab(tabId, targetIndex); },
              .onOpenTabContextMenu = [this](
                                          std::vector<ContextMenuControlEntry> entries, float x, float y
                                      ) { m_menus.openTabContextMenu(std::move(entries), x, y); },
              .onRequestLayout =
                  [this] {
                    if (m_surface)
                      m_surface->requestLayout();
                  },
              .onRequestRedraw =
                  [this] {
                    if (m_surface)
                      m_surface->requestRedraw();
                  },
              .canHandlePointer = [this] { return !hasActiveGesture(true, true, false); },
              .pointerSerial = [this] { return m_pointerSerial; },
          }
      ),
      m_transfers(
          m_wayland, m_renderContext, m_dispatcher, m_transfer, m_operations, m_tabs, m_places, m_pathCache,
          m_clipboard,
          {
              .mainSurface = [this] { return m_surface.get(); },
              .sceneRoot = [this] { return m_root.get(); },
              .grid = [this] { return m_grid; },
              .isWindowOpen = [this] { return m_windowOpen; },
              .isQuitting = [this] { return m_quitting; },
              .isRenaming = [this] { return m_renamePath.has_value(); },
              .keyboardModifiers = [this] { return m_keyboardModifiers; },
              .pointerSerial = [this] { return m_pointerSerial; },
              .selectedPathsOrdered = [this] { return selectedOperationPaths(); },
              .selectedPath = [this] { return activeTab().selectedPath(); },
              .onClickUnmovedDragItem = [this](
                                            std::size_t index, std::uint32_t clickTime
                                        ) { m_selection.handleUnmovedDragClick(index, clickTime); },
              .onCancelPlaceReorder = [this] { m_places.cancelGesture(); },
              .onClearLastPointerIndex = [this] { m_selection.clearLastPointerIndex(); },
              .onSynthesizeMotion = [this](const PointerEvent& motion) { handlePointer(motion); },
              .onUpdateNavigationControls = [this] { updateNavigationControls(); },
              .onStatus = [this](std::string text) { setStatus(std::move(text)); },
              .fileDropTabAt = [this](float x, float y) { return m_tabBar.fileDropTabAt(x, y); },
          }
      ) {
  if (const auto stateDir = m_settings.stateDir(); !stateDir.empty())
    m_recentActivity.setStoragePath(stateDir / "recent_activity.json");
  m_recentActivity.setRetention(m_settings.recentRetention());
  (void)m_recentActivity.load();
  m_menus.loadInitialPreference();
  m_alwaysShowTabBar = m_settings.alwaysShowTabBar();
  m_openTabsInBackground = m_settings.openTabsInBackground();
  m_listHeader.loadInitialState();
  m_listHeader.applyTabViewState(activeTab().viewState);
  updateWindowTitle();
  m_settings.setSearchReloadCallback([this](const filemanager::SearchSettings& previous) {
    refreshSearchSettings(previous);
  });
  m_operations.setConflictResolver(m_modalDialog.makeConflictResolver());
  m_operations.setArchivePasswordResolver(m_modalDialog.makeArchivePasswordResolver());
  m_operations.setAdminConflictResolver(
      m_modalDialog.makeConflictResolver(filemanager::ConflictPathAccess::ExecutorOnly)
  );
  installInputHooks();
}

bool FileManager::hasActiveGesture(bool includeStatusResize, bool includeListHeader, bool includeTabBar) const {
  return m_transfers.isDragPending()
      || m_transfers.isDraggingFiles()
      || m_transfer.dragging()
      || m_places.gestureActive()
      || (includeListHeader && m_listHeader.gestureActive())
      || (includeTabBar && m_tabBar.gestureActive())
      || m_resizingSidebar
      || m_previewResize.has_value()
      || (includeStatusResize && m_resizingStatus)
      || (m_grid && m_grid->rubberBandActive());
}

bool FileManager::canChangeSelection() const {
  return m_grid && !activeSession().loading() && !m_renamePath && !hasActiveGesture();
}

void FileManager::duplicateSelection() {
  auto& tab = activeTab();
  if (!tab.capabilities().canCreate
      || tab.session.browsingTrash()
      || tab.session.loading()
      || !currentDirectoryWritable())
    return;
  auto paths = selectedOperationPaths();
  if (paths.empty())
    return;
  if (const auto id = m_operations.duplicate(
          std::move(paths), tab.session.path().string(),
          filemanager::adminOperationContext(tab.administratorBrowsing ? &tab : nullptr)
      ))
    m_clipboard.registerPasteRequest(id, PasteRequest{tab.id, tab.session.path(), tab.session.navigationGeneration()});
}

void FileManager::createSelectionSymlink() {
  auto& tab = activeTab();
  if (!tab.capabilities().canCreate || tab.session.browsingTrash() || !currentDirectoryWritable())
    return;
  const auto selection = symlinkSelection();
  if (!selection) {
    setStatus("Select one regular file or folder to create a symlink");
    return;
  }
  const auto& [path, directory] = *selection;
  auto name = path.filename().string();
  // Treat compound filename extensions as a unit and retain a leading dot.
  const auto extension = directory ? std::string::npos : name.find('.', 1);
  name.insert(extension == std::string::npos ? name.size() : extension, "-link");
  if (const auto id = m_operations.createSymlink(
          path.string(), (tab.session.path() / name).string(), true,
          filemanager::adminOperationContext(tab.administratorBrowsing ? &tab : nullptr)
      ))
    m_clipboard.registerPasteRequest(id, PasteRequest{tab.id, tab.session.path(), tab.session.navigationGeneration()});
}

FileManager::SymlinkDialogHandlers FileManager::makeSymlinkDialogHandlers() {
  const auto& tab = activeTab();
  const PasteRequest request{tab.id, tab.session.path(), tab.session.navigationGeneration()};
  const bool admin = tab.administratorBrowsing;
  const auto generation = tab.adminSession.sessionGeneration();
  const auto dialogAlive = std::make_shared<bool>(true);
  const auto pickerOwned = std::make_shared<bool>(false);
  return {
      .onSubmit = [this, request, admin, generation](const std::string& name, const std::string& target)
          -> std::optional<filemanager::ModalDialogHost::SymlinkError> {
        using Field = filemanager::ModalDialogHost::SymlinkField;
        using Err = filemanager::ModalDialogHost::SymlinkError;
        if (m_places.hasActiveDeviceOperations() || m_operations.replaying())
          return Err{"Wait for the current operation to finish.", Field::None};
        auto* currentTab = m_tabs.findTab(request.tabId);
        if (currentTab == nullptr
            || request.tabId != m_tabs.activeTabId()
            || request.navigation != currentTab->session.navigationGeneration()
            || currentTab->administratorBrowsing != admin
            || (admin && currentTab->adminSession.sessionGeneration() != generation)
            || currentTab->session.browsingTrash()
            || !currentTab->capabilities().canCreate)
          return Err{"The displayed folder changed. Reopen the dialog in the destination folder.", Field::None};
        if (name.empty())
          return Err{"Enter a symlink name.", Field::Name};
        if (const auto reason = filemanager::invalidFileNameReason(name); !reason.empty())
          return Err{std::string(reason), Field::Name};
        if (target.empty() || target.find('\0') != std::string::npos)
          return Err{"Enter a target path.", Field::Target};
        const auto destination = request.destination / name;
        if (!admin) {
          std::error_code error;
          const auto status = std::filesystem::symlink_status(destination, error);
          if (std::filesystem::exists(status))
            return Err{"An item with that name already exists.", Field::Name};
          if (error && error != std::errc::no_such_file_or_directory)
            return Err{error.message(), Field::Name};
        }
        if (request.destination == currentTab->session.path() && !currentTab->session.currentDirectoryWritable())
          return Err{"The destination folder does not support file creation.", Field::None};
        const auto id = m_operations.createSymlink(
            filemanager::expandHomePath(target, g_get_home_dir()), destination.string(), false,
            filemanager::adminOperationContext(admin ? currentTab : nullptr)
        );
        if (id == 0)
          return Err{"The symlink operation could not be started.", Field::None};
        m_clipboard.registerPasteRequest(id, request);
        return std::nullopt;
      },
      .onBrowseTarget =
          [this, alive = std::weak_ptr<bool>(m_alive), dialogAlive, pickerOwned,
           baseDirectory = request.destination](const std::string& currentTarget) {
            if (!*dialogAlive || m_modalDialog.closeQueued())
              return;
            FileDialogOptions options{
                .mode = FileDialogMode::SelectFileOrFolder,
                .startDirectory = filemanager::resolveSymlinkTargetPickerStartDirectory(
                    currentTarget, baseDirectory, g_get_home_dir()
                ),
                .title = "Choose Symlink Target",
            };
            *pickerOwned = true;
            if (!FileDialog::open(
                    std::move(options),
                    [this, alive, dialogAlive, pickerOwned](std::optional<std::filesystem::path> path) {
                      *pickerOwned = false;
                      const auto locked = alive.lock();
                      if (!locked || !*locked || !*dialogAlive || m_modalDialog.closeQueued() || !path)
                        return;
                      m_modalDialog.setSymlinkTargetPath(path->string());
                    }
                )) {
              *pickerOwned = false;
              m_modalDialog.showSymlinkTargetError("Could not open the target picker.");
            }
          },
      .onDismiss =
          [this, dialogAlive, pickerOwned] {
            *dialogAlive = false;
            if (*pickerOwned) {
              *pickerOwned = false;
              if (m_destinationPicker && m_destinationPicker->isOpen())
                m_destinationPicker->closeFileDialogWithoutResult();
              FileDialog::cancelIfPending();
            }
          },
  };
}

void FileManager::showSymlinkDialog() {
  const auto& tab = activeTab();
  if (!tab.capabilities().canCreate
      || m_modalDialog.isOpen()
      || tab.session.browsingTrash()
      || !currentDirectoryWritable())
    return;
  auto handlers = makeSymlinkDialogHandlers();
  m_modalDialog.showSymlinkDialog(
      std::move(handlers.onSubmit), std::move(handlers.onBrowseTarget), std::move(handlers.onDismiss)
  );
}

void FileManager::beginRename() {
  if (!activeTab().capabilities().canRename) {
    if (activeSession().browsingTrash())
      setStatus("This action is unavailable in Wastebin");
    else if (activeTab().hasActiveAdminSession() && activeSession().loading())
      setStatus("Wait for the folder listing to finish");
    else if (!currentDirectoryWritable())
      setStatus("This directory is read-only");
    else
      setStatus("This action is currently unavailable");
    return;
  }
  const auto paths = selectedPaths();
  if (!paths.empty())
    beginRenamePath(paths.front());
}

void FileManager::beginRenamePath(const std::filesystem::path& path) {
  auto& tab = activeTab();
  auto& session = tab.session;
  const auto tabId = tab.id;
  if (session.browsingTrash()) {
    setStatus("This action is unavailable in Wastebin");
    return;
  }
  if (!tab.capabilities().canRename) {
    if (tab.hasActiveAdminSession() && session.loading()) {
      m_pendingInlineRenamePath = path;
      return;
    }
    setStatus(!currentDirectoryWritable() ? "This directory is read-only" : "This action is currently unavailable");
    return;
  }
  if (!m_grid || !m_surface || m_inlineRenameTarget.has_value())
    return;
  const bool isAdminTab = tab.hasActiveAdminSession();
  if (isAdminTab && m_operations.administratorOperationActive(tabId)) {
    setStatus("Wait for the current Administrator operation to finish");
    return;
  }
  if (!session.search().active() && path.parent_path() != session.path()) {
    m_pendingInlineRenamePath.reset();
    return;
  }
  const auto found = std::ranges::find(session.entries(), path, &FileEntry::absPath);
  if (found == session.entries().end()) {
    if (session.loading())
      m_pendingInlineRenamePath = path;
    else
      setStatus("The item is no longer available");
    return;
  }
  const auto adminSessionGen = tab.adminSession.sessionGeneration();
  const auto navGen = session.navigationGeneration();
  const bool entryHasIdentity = found->hasIdentity;
  const std::uint64_t entryDevice = found->device;
  const std::uint64_t entryInode = found->inode;
  const auto index = static_cast<std::size_t>(found - session.entries().begin());
  m_wayland.stopKeyRepeat();
  m_selection.clearTypeahead();
  m_grid->setSelectedIndex(index, false);
  m_grid->scrollToIndex(index);
  m_listHeader.revealColumn(FileColumn::Name);
  m_renamePath = path;
  const auto stablePath = path.string();
  InputArea* editor = m_grid->beginInlineEdit(
      index, stablePath, path.filename().string(),
      [this, path, tabId, isAdminTab, adminSessionGen, navGen, entryHasIdentity, entryDevice,
       entryInode](const std::string& newName) {
        if (const auto reason = filemanager::invalidFileNameReason(newName); !reason.empty()) {
          setStatus(std::string(reason));
          return;
        }
        if (m_inlineRenameTarget.has_value())
          return;
        const auto destination = path.parent_path() / newName;
        if (destination == path) {
          cancelRename();
          return;
        }
        if (isAdminTab) {
          auto* originTab = m_tabs.findTab(tabId);
          if (!originTab
              || tabId != m_tabs.activeTabId()
              || !originTab->hasActiveAdminSession()
              || originTab->adminSession.sessionGeneration() != adminSessionGen
              || !originTab->capabilities().canRename
              || originTab->session.navigationGeneration() != navGen
              || originTab->session.path() != path.parent_path()) {
            cancelRename();
            setStatus("Administrator session is no longer active");
            return;
          }
          if (!entryHasIdentity || entryInode == 0) {
            setStatus("Could not verify the selected item identity; refresh and try again");
            return;
          }
          if (const auto id = m_operations.renameChecked(
                  path.string(), destination.string(), entryDevice, entryInode, {},
                  filemanager::adminOperationContext(originTab)
              ))
            m_inlineRenameTarget = InlineRenameTarget{id, tabId, path};
          else
            setStatus("Wait for the current Administrator operation to finish");
          return;
        }
        if (activeTab().administratorBrowsing) {
          cancelRename();
          return;
        }
        struct stat state{};
        if (lstat(path.c_str(), &state) != 0) {
          setStatus("Could not inspect the item: " + std::string(g_strerror(errno)));
          return;
        }
        // Keep the editor and the proposed name until success. Checked rename
        // rejects collisions without offering to overwrite another item.
        if (const auto id =
                m_operations.renameChecked(path.string(), destination.string(), state.st_dev, state.st_ino, {})) {
          m_inlineRenameTarget = InlineRenameTarget{id, tabId, path};
        }
      },
      [this] { cancelRename(); }
  );
  if (editor != nullptr) {
    m_inlineEditArea = editor;
    m_dispatcher.setFocus(editor);
    if (m_surface)
      m_surface->requestLayout();
  } else {
    m_pendingInlineRenamePath = path;
    if (m_surface)
      m_surface->requestLayout();
  }
}

void FileManager::cancelRename() {
  m_wayland.stopKeyRepeat();
  m_selection.clearTypeahead();
  if (m_renamePath && m_grid)
    m_grid->endInlineEdit(m_renamePath->string());
  m_renamePath.reset();
  m_inlineEditArea = nullptr;
  if (m_grid)
    m_dispatcher.setFocus(m_grid->focusArea());
  if (m_surface)
    m_surface->requestLayout();
  auto& session = activeSession();
  if (!session.loading() && session.hasPendingMonitorChanges())
    session.applyPendingMonitorChanges();
  if (m_refreshAfterRename) {
    m_refreshAfterRename = false;
    enumerate(session.path());
  } else {
    session.resumeSearchOrdering();
  }
}

void FileManager::installInputHooks() {
  if (!m_textClipboard) {
    m_textClipboard = std::make_unique<NativeTextClipboard>(
        m_transfer, [this, alive = std::weak_ptr<bool>(m_alive)](std::string text) {
          const auto locked = alive.lock();
          if (locked && *locked)
            writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8");
        }
    );
  }
  g_activeInputHooksOwner = this;
  for (const auto action : kManagedKeybindActions) {
    KeybindMatcher::setMatcher(
        action, [this, alive = std::weak_ptr<bool>(m_alive), action](std::uint32_t sym, std::uint32_t modifiers) {
          const auto locked = alive.lock();
          if (!locked || !*locked)
            return false;
          if (action == KeybindAction::TabPrevious) {
            const std::uint32_t mods = modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super);
            if ((sym == XKB_KEY_ISO_Left_Tab && (mods == 0U || mods == KeyMod::Shift))
                || (sym == XKB_KEY_Tab && mods == KeyMod::Shift))
              return true;
          } else if (action == KeybindAction::TabNext) {
            const std::uint32_t mods = modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super);
            if (sym == XKB_KEY_Tab && mods == 0U)
              return true;
          }
          return m_configService.matchesKeybind(action, sym, modifiers);
        }
    );
  }
  Input::setValidateKeyMatcher([this,
                                alive = std::weak_ptr<bool>(m_alive)](std::uint32_t sym, std::uint32_t modifiers) {
    const auto locked = alive.lock();
    if (locked && *locked && m_configService.matchesKeybind(KeybindAction::Validate, sym, modifiers))
      return true;
    const std::uint32_t mods = modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super);
    return mods == 0U && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter);
  });
  Input::setTextClipboard(m_textClipboard.get());
  Input::setReadlineShortcutsEnabled(m_settings.readlineShortcuts());
}

void FileManager::uninstallInputHooks() {
  if (g_activeInputHooksOwner != this)
    return;
  g_activeInputHooksOwner = nullptr;
  for (const auto action : kManagedKeybindActions)
    KeybindMatcher::setMatcher(action, {});
  Input::setValidateKeyMatcher({});
  Input::setTextClipboard(nullptr);
}

FileManager::~FileManager() {
  *m_alive = false;
  uninstallInputHooks();
  m_operations.shutdown();
  m_checksumDialog.dismiss();
  m_helpDialog.dismiss();
  m_menuAnimations.cancelAll();
  m_tabBar.detachUi();
  m_menus.closeContextMenu();
  m_transfers.shutdown();
  m_clipboard.shutdown();
  m_customSearches.forceCloseImmediate();
  m_modalDialog.forceCloseImmediate();
  m_launcher.closeApplicationPicker();
  if (m_destinationPicker) {
    m_destinationPicker->closeFileDialogWithoutResult();
    FileDialog::cancelIfPending();
    FileDialog::setPresenter(nullptr);
    m_destinationPicker.reset();
  }
  m_places.shutdown();
  m_protectedFiles.shutdown();
  m_pathCache.cancel();
  m_tabs.shutdownAll();
  m_dispatcher.setTextInputContext(nullptr, nullptr);
  m_dispatcher.setSceneRoot(nullptr);
  if (m_surface)
    m_surface->setSceneRoot(nullptr);
  m_selection.shutdown();
  releaseSearchThumbnails();
  m_selectionPreview.detach();
  TooltipManager::instance().shutdown();
  m_renderContext.cleanup();
  m_sharedContext.cleanup();
}

int FileManager::run() {
  installInputHooks();
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");
  i18n::Service::instance().init();
  Style::setRtl(i18n::Service::instance().rtl());
  m_configService.addReloadCallback(
      [this] {
        Input::setReadlineShortcutsEnabled(m_settings.readlineShortcuts());
        applyThemeAndAppearance();
        IconResolver::invalidateThemeContent();
        if (m_grid)
          m_grid->notifyDataChanged();
        m_places.markDirty(false);
        m_tabBar.updateTheme();
        updateAdminHeaderUi();
        m_modalDialog.updateTheme();
        m_customSearches.updateTheme();
        if (m_settingsWindow)
          m_settingsWindow->updateTheme();
        if (m_surface)
          m_surface->requestLayout();
        if (m_modalDialog.isOpen())
          m_modalDialog.requestLayout();
        m_customSearches.requestLayoutIfOpen();
      },
      "file-manager-theme"
  );
  m_settings.setReloadCallback([this](const toml::table& previous) { applyFilesPreferences(previous); });
  applyThumbnailLimits();
  m_themeService.setResolvedCallback([this](const nythera::theme::GeneratedPalette& generated, std::string_view) {
    applyFilesPalette(generated);
  });
  applyThemeAndAppearance();
  m_wayland.setNativeDataTransfer(&m_transfer);
  m_wayland.setTextInputService(&m_textInputService);
  m_wayland.setKeyboardFocusCallback([this](wl_surface* /*surface*/, bool entered) {
    if (!entered) {
      m_keyboardModifiers = 0;
      m_wayland.stopKeyRepeat();
      m_selection.clearTypeahead();
    } else {
      m_keyboardModifiers = m_wayland.keyboardModifiers();
    }
  });
  m_transfers.setupDragHandlers();
  m_wayland.connect();
  m_transfer.setDragIconResources(m_wayland.compositor(), m_wayland.shm());
  if (!m_wayland.hasXdgShell() || m_wayland.compositor() == nullptr || m_wayland.shm() == nullptr)
    throw std::runtime_error("Wayland compositor, shm, and xdg-shell are required");
  m_sharedContext.initialize(m_wayland.display());
  m_renderContext.initialize(m_sharedContext);
  m_renderContext.setTextFontFamily(m_settings.effectiveFontFamily());
  m_renderContext.setTextBaseDirection(i18n::Service::instance().rtl());
  TooltipManager::instance().initialize(m_wayland, &m_configService, &m_renderContext);
  TooltipManager::instance().setFreshHoverPolicy([this] {
    return m_menus.hasOpenPopup()
        || m_transfers.isDraggingFiles()
        || m_places.gestureActive()
        || m_transfers.isDragPending()
        || m_tabBar.gestureActive()
        || m_resizingSidebar
        || (m_modalDialog.isOpen()
            && (!activeModalDialog().hoverTooltipsEnabled() || activeModalDialog().pointerCaptured()))
        || (m_destinationPicker && m_destinationPicker->isOpen())
        || (!m_modalDialog.isOpen() && m_dispatcher.pointerCaptured())
        || (m_grid && m_grid->rubberBandActive());
  });
  m_dispatcher.setHoverChangeCallback([this](InputArea*, InputArea* next) {
    if (m_surface && !activeModalDialog().hoverTooltipsEnabled())
      TooltipManager::instance().onHoverChange(
          next, m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
      );
  });
  m_dispatcher.setFocusChangeCallback([this](InputArea* previous, InputArea* next) {
    InputArea* gridArea = m_grid ? m_grid->focusArea() : nullptr;
    if (next != nullptr && next != gridArea) {
      if (auto* scroll = findEnclosingScrollView(next))
        scrollNodeIntoScrollView(*scroll, nullptr, *next, Style::spaceSm);
    }
    if (m_grid != nullptr && (previous == gridArea || next == gridArea)) {
      if (next == gridArea && m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard)
        m_selection.setFocusVisible(true);
      else if (next != gridArea)
        m_selection.setFocusVisible(false);
      m_grid->notifyDataChanged();
      if (m_surface)
        m_surface->requestRedraw();
    }
  });
  m_surface = std::make_unique<ToplevelSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);
  m_surface->setAnimationManager(&m_menuAnimations);
  m_surface->setCloseRequestCallback([this] { return handleWindowCloseRequest(); });
  m_surface->setHazeProfile("windows");
  m_surface->setClosedCallback([this] {
    saveWindowSessionState();
    m_instanceRetryTimer.stop();
    m_instanceCoordinator.stop();
    m_customSearches.forceCloseImmediate();
    if (m_modalDialog.isOpen())
      m_modalDialog.forceCloseImmediate();
    m_windowOpen = false;
    if (!m_operations.active())
      m_tabs.shutdownAll();
    if (m_destinationPicker) {
      m_destinationPicker->closeFileDialogWithoutResult();
      FileDialog::cancelIfPending();
    }
    if (m_propertiesWindow)
      m_propertiesWindow->close();
    if (m_settingsWindow)
      m_settingsWindow->close();
    cancelFileGesture();
    TooltipManager::instance().forceDestroy();
    m_places.cancelCapacityQueries();
    m_menus.closeAll();
    struct MainCloseRequest {
      FileManager* self;
      std::weak_ptr<bool> alive;
    };
    g_idle_add(
        [](gpointer context) -> gboolean {
          std::unique_ptr<MainCloseRequest> request(static_cast<MainCloseRequest*>(context));
          const auto locked = request->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          auto* self = request->self;
          self->m_dispatcher.setSceneRoot(nullptr);
          self->releaseSearchThumbnails();
          self->m_selectionPreview.detach();
          self->m_previewPane = self->m_previewDivider = nullptr;
          self->m_previewResize.reset();
          if (self->m_surface) {
            self->m_surface->setSceneRoot(nullptr);
            self->m_surface.reset();
          }
          self->m_listHeader.detachUi();
          self->m_places.detachUi();
          self->m_menus.detachUi();
          self->m_tabBar.detachUi();
          self->m_transfers.detachUi();
          self->m_root.reset();
          self->syncProgressWindow();
          self->m_appBar = nullptr;
          self->m_windowMoveArea = nullptr;
          self->m_windowResizeAreas.fill(nullptr);
          self->m_appBrandContainer = nullptr;
          self->m_adminHeaderContainer = nullptr;
          self->m_adminHeaderSeparator = nullptr;
          self->m_adminHeaderShield = nullptr;
          self->m_adminHeaderLabel = nullptr;
          self->m_adminFinishButton = nullptr;
          self->m_grid = nullptr;
          self->m_locationInput = nullptr;
          self->m_searchBarController.detachUi();
          self->m_statusLabel = nullptr;
          self->m_statusDetailLabel = nullptr;
          if (!self->m_operations.active() && self->m_clipboard.pendingClipboardReads() == 0)
            self->m_running = false;
          return G_SOURCE_REMOVE;
        },
        new MainCloseRequest{this, m_alive}
    );
  });
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  m_windowTitle =
      filemanager::windowTitleForPath(activeSession().path(), g_get_home_dir(), activeTab().hasActiveAdminSession());
  const auto geometry = (m_settings.rememberWindowGeometry() ? m_settings.loadMainWindowGeometry() : std::nullopt)
                            .value_or(filemanager::WindowGeometryState{});
  ToplevelSurfaceConfig config{
      .width = geometry.width,
      .height = geometry.height,
      .minWidth = 560,
      .minHeight = 380,
      .maxWidth = 0,
      .maxHeight = 0,
      .title = m_windowTitle,
      .appId = "dev.nythera.FileManager",
      .maximized = geometry.maximized,
  };
  wl_output* output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output;
  if (!m_surface->initialize(output, config))
    throw std::runtime_error("could not create xdg_toplevel");
  m_dispatcher.setTextInputContext(m_surface->wlSurface(), &m_textInputService);

  m_thumbnailPendingSub = m_pickerThumbnails.subscribePendingUpload([this] {
    if (m_surface)
      m_surface->requestUpdate();
    if (m_quickLookOpen)
      m_modalDialog.requestLayout();
  });

  m_pickerHosts.registerHost(
      [this](wl_surface* surface) -> std::optional<LayerPopupParentContext> {
        auto* parent = m_surface.get();
        if (m_settingsWindow && m_settingsWindow->wlSurface() == surface)
          parent = m_settingsWindow->surface();
        else if (activeModalDialog().ownsSurface(surface))
          parent = activeModalDialog().surface();
        if (!parent || surface != parent->wlSurface())
          return std::nullopt;
        return LayerPopupParentContext{
            .surface = surface,
            .xdgSurface = parent->xdgSurface(),
            .xdgToplevel = parent->xdgToplevel(),
            .output = m_wayland.outputForSurface(surface),
            .width = parent->width(),
            .height = parent->height()
        };
      },
      {}, {},
      [this]() -> std::optional<LayerPopupParentContext> {
        if (activeModalDialog().isOpen())
          return m_pickerHosts.contextForSurface(activeModalDialog().wlSurface());
        return m_surface ? m_pickerHosts.contextForSurface(m_surface->wlSurface()) : std::nullopt;
      }
  );
  m_destinationPicker = std::make_unique<FileDialogPopup>();
  m_destinationPicker->initialize(m_wayland, m_configService, m_renderContext, m_pickerHosts, m_pickerThumbnails);
  FileDialog::setPresenter(m_destinationPicker.get());
  m_menus.initializePopups(m_configService, m_pickerHosts);

  m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
    if (m_launcher.handleAppPickerPointer(event))
      return;
    if (m_destinationPicker && m_destinationPicker->onPointerEvent(event))
      return;
    if (m_propertiesWindow && m_propertiesWindow->isOpen() && m_propertiesWindow->ownsSurface(event.surface))
      m_propertiesWindow->handlePointer(event);
    else if (m_settingsWindow && m_settingsWindow->isOpen() && m_settingsWindow->ownsSurface(event.surface))
      (void)m_settingsWindow->handlePointer(event);
    else
      handlePointer(event);
  });
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland.setCursorShape(serial, shape);
  });
  m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
    if (event.pressed && !event.repeat)
      m_lastInputSerial = m_wayland.lastInputSerial();
    if (m_launcher.isApplicationPickerOpen() && m_launcher.handleKeyboard(event))
      return;
    else if (m_destinationPicker && m_destinationPicker->handlesKeyboardForSurface(m_wayland.lastKeyboardSurface()))
      m_destinationPicker->onKeyboardEvent(event);
    else if (m_menus.isContextMenuOpen() && m_menus.handleKeyboard(event))
      return;
    else if (activeModalDialog().ownsSurface(m_wayland.lastKeyboardSurface()))
      activeModalDialog().handleKeyboard(event);
    else if (
        m_propertiesWindow
        && m_propertiesWindow->isOpen()
        && m_propertiesWindow->ownsSurface(m_wayland.lastKeyboardSurface())
    )
      m_propertiesWindow->handleKeyboard(event);
    else if (
        m_settingsWindow && m_settingsWindow->isOpen() && m_settingsWindow->ownsSurface(m_wayland.lastKeyboardSurface())
    )
      m_settingsWindow->handleKeyboard(event);
    else if (m_modalDialog.isOpen()) {
      // While a nested editor is open, the management surface stays visible but modal.
      if (!m_customSearches.isChildOpen())
        m_modalDialog.handleKeyboard(event);
    } else if (m_progressWindow.ownsSurface(m_wayland.lastKeyboardSurface()))
      m_progressWindow.handleKeyboard(event);
    else
      handleKeyboard(event);
  });
  m_wayland.setKeyboardModifiersCallback([this](std::uint32_t modifiers) {
    m_keyboardModifiers = modifiers;
    m_transfer.updateDragTarget();
    if (m_transfers.isDraggingFiles() && m_surface)
      m_surface->requestUpdate();
  });
  restoreWindowTabs();
  navigate(activeSession().path(), false);
  if (!m_recentActivity.storagePath().empty()) {
    (void)m_settingsFileWatcher.watch(
        m_recentActivity.storagePath(),
        [this] {
          if (m_windowOpen && m_settings.recentActivityEnabled() && activeSession().browsingRecent())
            enumerate(activeSession().path());
        },
        FileWatcher::WatchTrigger::WriteCompleted
    );
  }
  startInstanceCoordinator();
  if (!m_instanceCoordinator.listening() && !filemanager::InstanceCoordinator::defaultSocketPath().empty()) {
    m_instanceRetryTimer.startRepeating(std::chrono::seconds(1), [this] {
      startInstanceCoordinator();
      if (m_instanceCoordinator.listening())
        m_instanceRetryTimer.stop();
    });
  }
  m_settings.setProblemCallback(filemanager::notifyInvalidConfiguration);
  m_settings.setManualReloadCallback(filemanager::notifyConfigurationReloaded);
  TimerPollSource timers;
  KeyRepeatPollSource keyRepeat(m_wayland);
  MainLoop loop(
      m_wayland,
      [this, &timers, &keyRepeat] {
        return std::vector<PollSource*>{&m_gioPollSource,
                                        &m_configPollSource,
                                        &m_settings,
                                        &m_pickerThumbnails,
                                        &timers,
                                        &keyRepeat,
                                        &m_transfer,
                                        &m_instanceCoordinator,
                                        &m_settingsFileWatchSource};
      },
      [this] { return !m_running && !m_transfer.ownsSelection() && !m_transfer.busy(); },
      [this] {
        if (m_destinationPicker) {
          m_destinationPicker->closeFileDialogWithoutResult();
          FileDialog::cancelIfPending();
        }
        m_launcher.closeApplicationPicker();
        m_tabBar.detachUi();
        cancelFileGesture();
        TooltipManager::instance().forceDestroy();
        m_dispatcher.setTextInputContext(nullptr, nullptr);
        m_dispatcher.setSceneRoot(nullptr);
        m_customSearches.forceCloseImmediate();
        m_modalDialog.forceCloseImmediate();
        m_progressWindow.close();
        if (m_surface) {
          m_surface->setSceneRoot(nullptr);
          m_surface.reset();
        }
      },
      [this] { m_running = false; }
  );
  loop.run();
  return 0;
}

void FileManager::restoreWastebinSelection(bool chooseDestination) {
  const auto& session = activeSession();
  if (!session.browsingTrash() || !m_windowOpen)
    return;
  if (m_places.hasActiveDeviceOperations()) {
    setStatus("Wait for the device operation to finish");
    return;
  }
  if (std::ranges::any_of(m_operations.operations(), [](const auto& operation) {
        return operation.kind == FileOperationManager::Kind::EmptyWastebin && !operation.finished;
      })) {
    setStatus("Wait for Wastebin emptying to finish");
    return;
  }
  std::vector<std::string> uris;
  for (const auto& path : selectedPaths()) {
    const auto found = session.trashUris().find(path);
    if (found == session.trashUris().end()) {
      setStatus("A selected Wastebin item is no longer available; refresh the view");
      return;
    }
    uris.push_back(found->second);
  }
  if (uris.empty())
    return;
  if (!chooseDestination) {
    m_operations.restore(std::move(uris));
    return;
  }
  FileDialogOptions options{
      .mode = FileDialogMode::SelectFolder, .startDirectory = g_get_home_dir(), .title = "Restore to…"
  };
  const std::weak_ptr<bool> alive = m_alive;
  if (!FileDialog::open(
          std::move(options), [this, alive, tabId = activeTab().id, uris = std::move(uris)](auto destination) mutable {
            const auto locked = alive.lock();
            if (!locked || !*locked || !m_windowOpen || !destination)
              return;
            if (const auto* tab = m_tabs.findTab(tabId); tab && !tab->hasActiveAdminSession())
              m_operations.restore(std::move(uris), destination->string());
          }
      ))
    setStatus("Could not open the destination picker");
}

void FileManager::confirmEmptyWastebin() {
  if (!m_windowOpen)
    return;
  if (m_operations.active() || m_places.hasActiveDeviceOperations()) {
    setStatus("Wait for the current operation to finish before emptying Wastebin");
    return;
  }
  showDialog(
      "Empty Wastebin?",
      "Permanently delete all items in Wastebin, including Wastebins on mounted filesystems?\n\n"
      "This cannot be undone. These items cannot be restored from Wastebin afterwards.",
      {{"Cancel", 0, filemanager::DialogChoiceStyle::Cancel},
       {"Empty Wastebin", 1, filemanager::DialogChoiceStyle::Destructive}},
      [this, id = activeTab().id](int response) { emptyWastebinAfterConfirmation(id, response); }, nullptr,
      filemanager::DialogStyle::Warning
  );
}

void FileManager::emptyWastebinAfterConfirmation(filemanager::TabId originatingTab, int response) {
  if (response != 1 || !m_windowOpen || !m_tabs.findTab(originatingTab))
    return;
  if (m_operations.active() || m_places.hasActiveDeviceOperations())
    setStatus("Wait for the current operation to finish before emptying Wastebin");
  else
    m_operations.emptyWastebin(true);
}

void FileManager::pinPlace(const std::filesystem::path& path) {
  auto& tab = activeTab();
  if (!tab.administratorBrowsing) {
    m_places.savePlace(path);
    return;
  }
  const auto generation = tab.adminSession.sessionGeneration();
  (void)tab.adminSession.statInfoAsync(
      path, true,
      [this, alive = std::weak_ptr<bool>(m_alive), path, id = tab.id,
       generation](bool ok, const auto& record, auto, const std::string&) {
        const auto token = alive.lock();
        if (!token || !*token)
          return;
        const auto* origin = m_tabs.findTab(id);
        if (!m_windowOpen
            || !origin
            || !origin->hasActiveAdminSession()
            || origin->adminSession.sessionGeneration() != generation)
          return;
        if (ok && record.exists && record.isDir)
          m_places.savePlace(path, true);
        else if (origin->active)
          setStatus("This folder is unavailable");
      }
  );
}

void FileManager::handleLocationInputAction(int id) {
  if (m_locationInput == nullptr)
    return;
  switch (id) {
  case kLocationCut:
    m_locationInput->cutSelectedText();
    break;
  case kLocationCopy:
    (void)m_locationInput->copySelectedText();
    break;
  case kLocationPaste:
    pasteLocationFromClipboard();
    break;
  case kLocationSelectAll:
    m_locationInput->selectAll();
    break;
  default:
    break;
  }
}

void FileManager::confirmPermanentDelete() {
  const auto& tab = activeTab();
  const auto& session = tab.session;
  if (!tab.capabilities().canMutate) {
    setStatus("This action is unavailable in Administrator Mode");
    return;
  }
  auto targets = selectedOperationPaths();
  if (targets.empty())
    return;
  const auto prompt = filemanager::permanentDeletePrompt(targets, targets.size());
  showDialog(
      "Permanently delete?", prompt,
      {{"Cancel", 0, filemanager::DialogChoiceStyle::Cancel},
       {"Delete", 1, filemanager::DialogChoiceStyle::Destructive}},
      [this, tabId = tab.id, admin = tab.administratorBrowsing, generation = tab.adminSession.sessionGeneration(),
       navigation = session.navigationGeneration(), targets = std::move(targets)](int response) mutable {
        auto* origin = m_tabs.findTab(tabId);
        if (response != 1 || !origin || origin->administratorBrowsing != admin)
          return;
        if (!admin) {
          if (origin->capabilities().canMutate)
            m_operations.removePermanently(std::move(targets));
          return;
        }
        if (!origin->hasActiveAdminSession()
            || origin->adminSession.sessionGeneration() != generation
            || origin->session.navigationGeneration() != navigation)
          return;
        if (!origin->session.loading()) {
          if (!origin->capabilities().canMutate)
            setStatus("The folder is no longer writable.");
          else if (!m_operations.removePermanently(std::move(targets), filemanager::adminOperationContext(origin)))
            setStatus("Wait for the current Administrator operation to finish");
          return;
        }
        // A refresh temporarily clears the directory writability cache. Ask the
        // authenticated helper for current access before using the confirmed paths.
        const auto directory = origin->session.path();
        const auto alive = std::weak_ptr<bool>(m_alive);
        if (!origin->adminSession.statInfoAsync(
                directory, true,
                [this, alive, tabId, generation, navigation, directory, targets = std::move(targets)](
                    bool ok, const filemanager::AdminStatRecord& record, filemanager::AdminStatusCode,
                    const std::string& reason
                ) mutable {
                  const auto token = alive.lock();
                  if (!token || !*token)
                    return;
                  auto context = filemanager::adminOperationContext(m_tabs.findTab(tabId));
                  if (!context
                      || !context->active
                      || context->origin.sessionGeneration != generation
                      || context->origin.navigation != navigation
                      || context->origin.directory != directory)
                    return;
                  if (!ok || !record.exists || !record.isDir || !record.writable) {
                    setStatus(reason.empty() ? "The folder is no longer writable." : reason);
                    return;
                  }
                  context->canMutate = true;
                  if (!m_operations.removePermanently(std::move(targets), context))
                    setStatus("Wait for the current Administrator operation to finish");
                }
            ))
          setStatus("Could not verify access for the Administrator operation");
      },
      nullptr, filemanager::DialogStyle::Warning
  );
}

void FileManager::createNewItem(int id) {
  auto& tab = activeTab();
  auto& session = tab.session;
  const auto caps = tab.capabilities();
  if (id >= kCreateDocument && id <= kCreatePresentation) {
    if (!caps.canCreate) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    if (session.browsingTrash() || !currentDirectoryWritable()) {
      setStatus("This directory does not support file creation");
      return;
    }
    const char* extension = id == kCreateDocument ? ".odt" : id == kCreateSpreadsheet ? ".ods" : ".odp";
    const char* asset = id == kCreateDocument ? "templates/filemanager/office/blank.odt"
        : id == kCreateSpreadsheet            ? "templates/filemanager/office/blank.ods"
                                              : "templates/filemanager/office/blank.odp";
    const std::string defaultName = id == kCreateDocument ? "New Document"
        : id == kCreateSpreadsheet                        ? "New Spreadsheet"
                                                          : "New Presentation";
    if (tab.administratorBrowsing) {
      if (!m_operations.createFileFromTemplate(
              paths::assetPath(asset).string(), (session.path() / (defaultName + extension)).string(),
              filemanager::adminOperationContext(&tab)
          ))
        setStatus("Wait for the current Administrator operation to finish");
      return;
    }
    std::error_code destinationError;
    auto destination =
        filemanager::creationDestinationWithExtension(session.path(), defaultName, extension, destinationError);
    if (!destination) {
      setStatus(destinationError ? destinationError.message() : "Could not choose a filename");
      return;
    }
    if (const auto opId = m_operations.createFileFromTemplate(paths::assetPath(asset).string(), destination->string());
        opId != 0) {
      m_createRequests[opId] = CreateRequest{tab.id, session.navigationGeneration(), *destination};
    }
    return;
  }
  if (tab.administratorBrowsing) {
    if (!caps.canCreateBasic) {
      setStatus(
          session.loading() ? "Wait for the folder listing to finish" : "This directory does not support file creation"
      );
      return;
    }
    const auto directory = session.path();
    const auto opId = id == kCreateFolder
        ? m_operations.createFolder((directory / "New Folder").string(), filemanager::adminOperationContext(&tab))
        : m_operations.createFile(
              (directory / (id == kCreateTextFile ? "New File.txt" : "New File")).string(),
              filemanager::adminOperationContext(&tab)
          );
    if (!opId)
      setStatus("Wait for the current Administrator operation to finish");
    return;
  }
  if (!caps.canCreate) {
    setStatus("This action is unavailable in Administrator Mode");
    return;
  }
  if (session.browsingTrash() || !currentDirectoryWritable()) {
    setStatus("This directory does not support file creation");
    return;
  }
  const std::string defaultName = id == kCreateFolder ? "New Folder" : "New File";
  std::error_code error;
  auto destination = filemanager::creationDestination(session.path(), defaultName, id == kCreateTextFile, error);
  if (!destination) {
    setStatus(error ? error.message() : "Could not choose a filename");
    return;
  }
  const auto opId = id == kCreateFolder ? m_operations.createFolder(destination->string())
                                        : m_operations.createFile(destination->string());
  if (opId != 0)
    m_createRequests[opId] = CreateRequest{tab.id, session.navigationGeneration(), *destination};
}

void FileManager::openPreferences() {
  if (!m_settingsWindow) {
    m_settingsWindow = std::make_unique<filemanager::SettingsWindow>(
        m_wayland, m_renderContext, m_configService, m_settings,
        filemanager::SettingsWindow::Callbacks{
            .onPreferencesChanged = [this](const toml::table& previous) { applyFilesPreferences(previous); },
            .onSearchSettingsChanged =
                [this](const filemanager::SearchSettings& previous) { refreshSearchSettings(previous); },
            .onManageCustomSearches = [this] { m_customSearches.manageCustomSearches(); },
            .onEditCustomSearch = [this](
                                      std::optional<filemanager::CustomSearch> profile
                                  ) { m_customSearches.editCustomSearch(std::move(profile)); },
            .onInspectThumbnailCache = [this](auto onResult) { inspectThumbnailCache(false, std::move(onResult)); },
            .onClearThumbnailCache = [this] { inspectThumbnailCache(true); },
            .onExportTheme =
                [this](const auto& path, std::string& error) {
                  return nythera::theme::writePaletteFile(path, m_filesDarkPalette, m_filesLightPalette, &error);
                },
            .onPickColor = [this](
                               std::string title, Color color, std::function<void(Color)> apply
                           ) { showPreferenceColorPicker(std::move(title), color, std::move(apply)); },
            .onClearRecentHistory =
                [this] {
                  if (!m_recentActivity.update([](auto& store) { return store.clearHistory(); }))
                    return false;
                  m_places.markDirty();
                  if (activeSession().browsingRecent())
                    enumerate(activeSession().path());
                  return true;
                },
        }
    );
  }
  if (!m_settingsWindow->open(m_surface.get()))
    setStatus("Could not open Preferences window");
}

void FileManager::openProperties(const std::filesystem::path& path) {
  if (activeSession().browsingArchive()) {
    showArchiveMemberProperties(path);
    return;
  }
  if (!activeTab().capabilities().canOpenProperties) {
    setStatus("Properties is unavailable in Administrator Mode");
    return;
  }
  if (!m_propertiesWindow)
    m_propertiesWindow = std::make_unique<PropertiesWindow>(
        m_wayland, m_renderContext, m_settings, m_operations,
        [this](std::string text) { writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8"); }
    );
  PropertiesWindow::AdministratorTab administrator;
  if (activeTab().administratorBrowsing)
    administrator = [this, id = activeTab().id] { return m_tabs.findTab(id); };
  const auto selected = activeTab().selectedPathsOrdered();
  std::vector<std::filesystem::path> selection;
  selection.reserve(selected.size());
  for (const auto& item : selected)
    selection.emplace_back(item);
  const bool opened = selection.size() > 1
      ? m_propertiesWindow->openSelection(selection, m_surface.get(), std::move(administrator))
      : m_propertiesWindow->open(path, m_surface.get(), std::move(administrator));
  if (!opened)
    setStatus("Could not open Properties window");
}

void FileManager::applyFilesPreferences(const toml::table& previous) {
  const auto& current = m_settings.preferences();
  const auto changed = [&](std::string_view key) { return previous["files"][key] != current["files"][key]; };
  if (changed("fzf_search") || changed("search_default_mode")) {
    for (const auto& tab : m_tabs.tabs()) {
      if (tab->session.searchOpen())
        configureSearchBackend(tab->session);
    }
    syncSearchUi();
  }
  if (changed("readline_shortcuts"))
    Input::setReadlineShortcutsEnabled(m_settings.readlineShortcuts());
  if (previous["theme"] != current["theme"]
      || changed("font_family")
      || changed("icon_theme")
      || changed("animations_enabled")
      || changed("animation_speed")
      || changed("haze_enabled")
      || changed("branding_mode")) {
    applyThemeAndAppearance();
    m_places.markDirty();
    m_tabBar.updateTheme();
    updateAdminHeaderUi();
    m_modalDialog.updateTheme();
    if (m_settingsWindow)
      m_settingsWindow->updateTheme();
    if (m_grid)
      m_grid->notifyDataChanged();
    if (m_surface)
      m_surface->requestLayout();
  }
  if (changed("address_bar_mode") && !m_editingLocation && m_locationInput)
    setLocationEditMode(false, true);
  if (changed("recent_retention"))
    m_recentActivity.setRetention(m_settings.recentRetention());
  if (activeSession().browsingRecent() && (changed("recent_activity_enabled") || changed("recent_retention")))
    enumerate(activeSession().path());
  if (changed("show_places_personal")
      || changed("show_places_devices")
      || changed("show_places_network")
      || changed("show_places_capacity")
      || changed("recent_activity_enabled")
      || changed("show_recent_sidebar")
      || changed("show_frequent_locations")
      || changed("recent_retention")) {
    m_places.markDirty(true);
    if (m_surface)
      m_surface->requestLayout();
  }
  if (changed("admin_inactivity_timeout_seconds")) {
    const int timeoutSeconds = m_settings.adminInactivityTimeoutSeconds();
    for (const auto& tab : m_tabs.tabs()) {
      if (!tab->adminSession.hasCustomTimeoutForTesting())
        tab->adminSession.setInactivityTimeoutMs(static_cast<std::uint32_t>(timeoutSeconds) * 1000U);
    }
  }
  if (m_settingsWindow)
    m_settingsWindow->onPreferencesReloaded();
  if (changed("show_menu_bar")) {
    m_menus.loadInitialPreference();
    m_menus.syncResponsiveMenuBar(m_lastHeaderWidth);
  }
  if (changed("thumbnail_max_file_mib") || changed("thumbnail_cache_mib"))
    applyThumbnailLimits();
  if (changed("preview_mode") || changed("thumbnail_policy") || changed("thumbnail_max_file_mib")) {
    const auto mode = m_settings.previewMode();
    if (mode != filemanager::PreviewMode::Hidden)
      m_lastVisiblePreviewMode = mode;
    if (changed("thumbnail_policy") || changed("thumbnail_max_file_mib")) {
      m_searchThumbnails.clear();
      if (m_grid)
        m_grid->notifyDataChanged();
    }
    if (m_surface)
      m_surface->requestLayout();
    if (m_quickLookOpen)
      m_modalDialog.requestLayout();
  }
  if (changed("open_tabs_in_background"))
    m_openTabsInBackground = m_settings.openTabsInBackground();
  if (changed("always_show_tab_bar")) {
    m_alwaysShowTabBar = m_settings.alwaysShowTabBar();
    m_tabBar.markDirty();
    if (m_surface)
      m_surface->requestLayout();
  }
  if (changed("saved_places") || changed("place_order") || changed("hidden_mounts"))
    m_places.reloadPreferences();
  const bool searchView = activeSession().search().active();
  if (searchView ? changed("search_column_widths") || changed("search_column_layout")
                 : changed("list_column_widths") || changed("list_column_layout"))
    m_listHeader.reloadColumnPreferences();

  const bool grid = changed("grid_view");
  const bool scale = changed("view_scale");
  const bool hidden = changed("show_hidden");
  const bool foldersFirst = changed("folders_first");
  const bool hiddenLast = changed("hidden_last");
  const bool sortField = changed("sort_field");
  const bool sortOrder = changed("sort_descending");
  const bool searchGrid = changed("search_grid_view");
  const bool searchScale = changed("search_view_scale");
  const bool searchSortField = changed("search_sort_field");
  const bool searchSortOrder = changed("search_sort_descending");
  if (!(grid
        || scale
        || hidden
        || foldersFirst
        || hiddenLast
        || sortField
        || sortOrder
        || searchGrid
        || searchScale
        || searchSortField
        || searchSortOrder))
    return;
  const auto view = loadInitialTabViewState(m_settings);
  const auto search = loadInitialTabViewState(m_settings, true);
  for (const auto& tab : m_tabs.tabs()) {
    auto& searchState = tab->searchViewState;
    if (searchGrid)
      searchState.gridMode = search.gridMode;
    if (searchScale)
      searchState.viewScale = search.viewScale;
    if (searchSortField)
      searchState.sortField = search.sortField;
    if (searchSortOrder)
      searchState.sortOrder = search.sortOrder;
    if ((searchSortField || searchSortOrder) && tab.get() != &activeTab() && tab->session.search().active())
      tab->session.sortEntries();
    auto& state = tab->viewState;
    if (grid) {
      state.gridMode = view.gridMode;
      if (view.gridMode && !tab->session.search().active())
        tab->session.clearFolderCounts();
    }
    if (scale)
      state.viewScale = view.viewScale;
    if (hidden) {
      searchState.showHidden = state.showHidden = view.showHidden;
      tab->session.setShowHidden(view.showHidden);
    }
    if (foldersFirst) {
      searchState.foldersFirst = state.foldersFirst = view.foldersFirst;
      tab->session.setFoldersFirst(view.foldersFirst);
    }
    if (hiddenLast) {
      searchState.hiddenLast = state.hiddenLast = view.hiddenLast;
      tab->session.setHiddenLast(view.hiddenLast);
    }
    if (sortField)
      state.sortField = view.sortField;
    if (sortOrder)
      state.sortOrder = view.sortOrder;
    if (tab.get() != &activeTab() && (hidden || foldersFirst || hiddenLast || sortField || sortOrder))
      tab->needsRefresh = true;
  }
  applyActiveView();
  if (hidden) {
    activeSession().refreshPreservingSelection();
  } else if (
      foldersFirst || hiddenLast || sortField || sortOrder || (searchView && (searchSortField || searchSortOrder))
  ) {
    activeSession().sortEntries(foldersFirst || hiddenLast || sortField || sortOrder);
    if (m_grid) {
      m_grid->notifyDataChanged();
      restoreSelection(false);
    }
  }
  updateNavigationControls();
}

void FileManager::persistFileViewState() {
  auto& tab = activeTab();
  auto& state = tab.displayedViewState();
  const filemanager::FileViewSettings previous{
      state.gridMode, state.showHidden, state.foldersFirst, state.hiddenLast, state.sortField, state.sortOrder,
  };
  const filemanager::FileViewSettings next{
      m_listHeader.gridMode(),  tab.session.showHidden(), tab.session.foldersFirst(),
      tab.session.hiddenLast(), m_listHeader.sortField(), m_listHeader.sortOrder(),
  };
  state.gridMode = next.gridMode;
  state.showHidden = next.showHidden;
  state.foldersFirst = next.foldersFirst;
  state.hiddenLast = next.hiddenLast;
  state.sortField = next.sortField;
  state.sortOrder = next.sortOrder;
  state.viewScale = m_listHeader.viewScale();
  tab.viewState.showHidden = next.showHidden;
  tab.viewState.foldersFirst = next.foldersFirst;
  tab.viewState.hiddenLast = next.hiddenLast;
  if (!m_settings.saveFileView(next, previous, tab.session.search().active()))
    setStatus("Unable to save file view preferences");
}

void FileManager::launchNewWindow(const std::filesystem::path& path) {
  std::error_code pathError;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", pathError);
  if (pathError) {
    setStatus("Could not locate the Files executable");
    return;
  }
  const std::string executableText = executable.string();
  const std::string pathText = path.string();
  gchar* argv[] = {
      const_cast<gchar*>(executableText.c_str()), const_cast<gchar*>("--new-window"),
      const_cast<gchar*>(pathText.c_str()), nullptr
  };
  GError* error = nullptr;
  if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
    setStatus(error ? error->message : "Could not open a new window");
    g_clear_error(&error);
  }
}

void FileManager::activateSelected() {
  if (m_grid == nullptr)
    return;
  const auto& entries = activeSession().entries();
  const auto selected = m_grid->selectedIndex();
  if (!selected || *selected >= entries.size())
    return;
  activatePath(entries[*selected].absPath);
}

void FileManager::goToLinkTarget(const std::filesystem::path& link) {
  auto& tab = activeTab();
  if (tab.session.browsingTrash())
    return;
  const auto& entries = tab.session.entries();
  const auto entry = std::ranges::find(entries, link, &FileEntry::absPath);
  if (entry == entries.end() || !entry->isSymlink)
    return;
  const auto target = filemanager::resolveLinkTarget(link);
  if (!target) {
    setStatus("The link target is unavailable");
    return;
  }
  if (target->directory) {
    navigate(target->path);
    return;
  }

  // Navigation clears the prior selection before its asynchronous listing.
  navigate(target->path.parent_path(), true, target->path.filename().native());
  tab.selectSingle(target->path);
  tab.linkTargetToReveal = target->path;
}

void FileManager::activatePath(const std::filesystem::path& path) {
  auto& tab = activeTab();
  const auto& session = tab.session;
  const auto found = std::ranges::find(session.entries(), path, &FileEntry::absPath);
  if (found == session.entries().end())
    return;
  const auto entry = *found;
  if (session.browsingArchive()) {
    if (entry.isDir)
      navigateLocation({session.path(), entry.absPath.native()});
    else
      openArchiveMember(entry.absPath, false);
    return;
  }
#ifdef NYTHERA_FILES_ARCHIVES
  if (!entry.isDir
      && !entry.isSymlink
      && !session.browsingTrash()
      && !tab.administratorBrowsing
      && !tab.hasActiveAdminSession()) {
    const auto format = filemanager::archive_backend::formatFromName(entry.name);
    const auto capabilities = filemanager::archive_backend::capabilities();
    if (format && std::ranges::any_of(capabilities, [&](const auto& item) {
          return item.extract && item.format == *format;
        })) {
      navigateLocation({entry.absPath, std::string{}});
      return;
    }
  }
#endif
  if (entry.isDir) {
    navigate(entry.absPath);
    return;
  }
  if (tab.administratorBrowsing && !filemanager::userCanReadPath(path)) {
    m_launcher.openProtectedFileWithDefault(tab.id, path);
    return;
  }
  if (session.browsingTrash() && !session.trashUris().contains(entry.absPath.string())) {
    setStatus("This Wastebin item is no longer available; refresh the view");
    return;
  }
  g_autoptr(GFile) file = session.browsingTrash()
      ? g_file_new_for_uri(session.trashUris().at(entry.absPath.string()).c_str())
      : g_file_new_for_path(entry.absPath.c_str());
  std::function<void()> onSuccess;
  if (m_settings.recentActivityEnabled()
      && !session.browsingTrash()
      && !tab.hasActiveAdminSession()
      && !tab.administratorBrowsing)
    onSuccess = [this, path] { recordFileOpen(path); };
  m_launcher.launchFileUri(tab.id, file, {}, std::move(onSuccess));
}

void FileManager::setStatus(std::string text) {
  m_statusMessage = std::move(text);
  m_statusMessageUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  updateStatusBar();
  m_statusMessageTimer.stop();
  if (!m_statusMessage.empty())
    m_statusMessageTimer.start(std::chrono::seconds(5), [this] {
      m_statusMessage.clear();
      updateStatusBar();
      if (m_surface)
        m_surface->requestRedraw();
    });
}

void FileManager::updateSelectionSummary() {
  updateStatusBar();
  if (m_surface && m_previewPane && m_previewPane->visible())
    m_surface->requestLayout();
  if (m_quickLookOpen)
    m_modalDialog.requestLayout();
}

std::string FileManager::formatCount(std::size_t count) {
  static const auto locale = [] {
    try {
      return std::locale("");
    } catch (const std::runtime_error&) {
      return std::locale::classic();
    }
  }();
  return std::format(locale, "{:L}", count);
}

void FileManager::updateStatusBar() {
  if (!m_statusLabel)
    return;
  auto& tab = activeTab();
  const auto& session = tab.session;
  std::string primary;
  std::string details;
  std::string freeSpace;
  if (!m_statusMessage.empty() && std::chrono::steady_clock::now() < m_statusMessageUntil) {
    primary = m_statusMessage;
  } else if (tab.selectionCount() == 1) {
    const std::filesystem::path path(*tab.selectedPaths().begin());
    const auto entry = std::ranges::find(session.entries(), path, &FileEntry::absPath);
    const std::string name = entry == session.entries().end() ? path.filename().string() : entry->name;
    primary = filemanager::sanitizeFilenameForDisplay(name);
    if (const auto& info = tab.selectionInfo(); info && info->path == path) {
      details = info->type;
      if (!info->target.empty())
        details += "  |  Target: " + filemanager::sanitizeFilenameForDisplay(info->target);
      if (info->hasSize) {
        const auto formatted = compactSize(info->size);
        std::ostringstream out;
        out
            << std::fixed
            << std::setprecision(formatted.value == std::round(formatted.value) ? 0 : 1)
            << formatted.value;
        details += "  |  " + out.str() + " " + formatted.unit;
      }
    } else {
      const bool isDir = entry != session.entries().end() && entry->isDir;
      const std::string entryType = entry != session.entries().end() ? entry->type : std::string{};
      details = guessFileTypeDescription(path, isDir, entryType);
      if (entry != session.entries().end() && !entry->isDir) {
        const auto formatted = compactSize(entry->size);
        std::ostringstream out;
        out
            << std::fixed
            << std::setprecision(formatted.value == std::round(formatted.value) ? 0 : 1)
            << formatted.value;
        details += "  |  " + out.str() + " " + formatted.unit;
      }
      if (tab.selectionInfoPendingPath() != path)
        querySelectionInfoAsync(path);
    }
    if (session.search().active())
      details += "  |  " + filemanager::sanitizeFilenameForDisplay(path.parent_path().string());
  } else if (tab.hasSelection()) {
    cancelSelectionInfoQuery();
    std::size_t folders = 0, files = 0;
    std::uint64_t bytes = 0;
    bool hasSize = false;
    for (const auto& selected : tab.selectedPaths()) {
      const auto found = std::ranges::find(session.entries(), std::filesystem::path(selected), &FileEntry::absPath);
      if (found == session.entries().end())
        continue;
      if (found->isDir)
        ++folders;
      else {
        ++files;
        bytes += found->size;
        hasSize = true;
      }
    }
    primary = formatCount(tab.selectionCount()) + " selected";
    details = formatCount(folders) + " folders, " + formatCount(files) + " files";
    if (hasSize) {
      const auto size = compactSize(bytes);
      std::ostringstream out;
      out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
      details += "  |  " + out.str() + " " + size.unit;
    }
  } else {
    cancelSelectionInfoQuery();
    std::size_t folders = 0, files = 0;
    std::uint64_t bytes = 0;
    for (const auto& entry : session.entries()) {
      if (entry.isDir)
        ++folders;
      else {
        ++files;
        bytes += entry.size;
      }
    }
    primary = formatCount(folders) + (folders == 1 ? " folder" : " folders");
    details = formatCount(files) + (files == 1 ? " file" : " files");
    if (files) {
      const auto size = compactSize(bytes);
      std::ostringstream out;
      out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
      details += " · " + out.str() + " " + size.unit;
    }
    if (!session.search().active() && !session.browsingTrash()) {
      if (tab.statusFreeSpacePath() != session.path())
        queryStatusFreeSpaceAsync(session.path());
      if (tab.statusFreeSpacePath() == session.path() && tab.freeSpaceBytes().has_value()) {
        const auto size = compactSize(*tab.freeSpaceBytes());
        std::ostringstream out;
        out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
        freeSpace = out.str() + " " + size.unit + " free";
      }
    }
  }
  m_statusLabel->setText(primary);
  if (m_statusDetailLabel)
    m_statusDetailLabel->setText(details.empty() ? "" : "  |  " + details);
  if (freeSpace.empty()) {
    if (m_statusLabel->hasTooltip())
      m_statusLabel->clearTooltip();
  } else {
    const auto currentTooltip = m_statusLabel->tooltipContent();
    const auto* currentText = std::get_if<std::string>(&currentTooltip);
    if (!currentText || *currentText != freeSpace)
      m_statusLabel->setTooltip(std::move(freeSpace));
  }
}

void FileManager::updateWindowTitle() {
  m_windowTitle =
      filemanager::windowTitleForPath(activeSession().path(), g_get_home_dir(), activeTab().hasActiveAdminSession());
  if (m_surface)
    m_surface->setTitle(m_windowTitle);
}

std::unique_ptr<Flex> FileManager::buildHeaderAppBar() {
  auto appBar = std::make_unique<Flex>();
  m_appBar = appBar.get();
  appBar->setAlign(FlexAlign::Center);
  appBar->setGap(Style::spaceSm);
  appBar->setMinHeight(34.0F);
  appBar->setPadding(Style::spaceSm, Style::spaceMd);
  appBar->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh, kFilesAppBarOpacity));
  appBar->setBorder(
      colorSpecFromRole(
          ColorRole::Primary, 0.38F /* palette-alpha-exception: The Files app bar has a muted accent rim. An opaque
                                       primary rim changes its appearance. */
      ),
      Style::borderWidth
  );
  appBar->setRadius(Style::scaledRadiusMd(1.0F));

  // The controls are added above this area so they keep their own pointer gestures.
  auto moveArea = std::make_unique<InputArea>();
  moveArea->setParticipatesInLayout(false);
  moveArea->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
  moveArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
  moveArea->setOnPress([this](const InputArea::PointerData& event) {
    if (event.pressed && event.serial && m_surface && m_wayland.seat()) {
      m_dispatcher.cancelPointerCapture();
      m_surface->beginMove(event.serial);
    }
  });
  m_windowMoveArea = static_cast<InputArea*>(appBar->addChild(std::move(moveArea)));

  auto appName = std::make_unique<Flex>();
  m_appBrandContainer = appName.get();
  appName->setHitTestVisible(false);
  appName->setAlign(FlexAlign::Center);
  appName->setGap(9.0F); // The display font's space advance is 360/1000 em at 25 px.
  auto appNameNythera = std::make_unique<Label>();
  m_appBrandNythera = appNameNythera.get();
  appNameNythera->setText("NYTHERA");
  appNameNythera->setFontSize(25.0F);
  appNameNythera->setDirectDisplayFont(true);
  appNameNythera->setColor(colorSpecFromRole(ColorRole::OnSurface));
  appName->addChild(std::move(appNameNythera));
  auto appNameFiles = std::make_unique<Label>();
  m_appBrandFiles = appNameFiles.get();
  appNameFiles->setText("FILES");
  appNameFiles->setFontSize(25.0F);
  appNameFiles->setDirectDisplayFont(true);
  appNameFiles->setColor(colorSpecFromRole(ColorRole::Primary));
  appName->addChild(std::move(appNameFiles));
  appBar->addChild(std::move(appName));

  auto adminHeader = std::make_unique<Flex>();
  m_adminHeaderContainer = adminHeader.get();
  adminHeader->setAlign(FlexAlign::Center);
  adminHeader->setGap(Style::spaceSm);

  auto separator = std::make_unique<Box>();
  m_adminHeaderSeparator = separator.get();
  separator->setFrameSize(1.0F, 18.0F);
  separator->setFill(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  adminHeader->addChild(std::move(separator));

  auto shield = std::make_unique<Glyph>();
  m_adminHeaderShield = shield.get();
  shield->setGlyph("shield");
  shield->setGlyphSize(15.0F);
  shield->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  adminHeader->addChild(std::move(shield));

  auto adminLabel = std::make_unique<Label>();
  m_adminHeaderLabel = adminLabel.get();
  adminLabel->setText("Administrator Mode");
  adminLabel->setFontSize(Style::fontSizeBody);
  adminLabel->setFontWeight(FontWeight::SemiBold);
  adminLabel->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  adminLabel->setMaxLines(1);
  adminHeader->addChild(std::move(adminLabel));

  auto finishButton = std::make_unique<Button>();
  m_adminFinishButton = finishButton.get();
  finishButton->setText("Finish");
  finishButton->setVariant(ButtonVariant::Outline);
  finishButton->setControlHeight(24.0F);
  finishButton->setPadding(3.0F, 8.0F);
  finishButton->setRadius(Style::scaledRadiusSm(1.0F));
  finishButton->setFocusHighlightEnabled(false);
  finishButton->setTooltip("Exit Administrator Mode for this tab");
  finishButton->setOnClick([this] { finishAdministratorMode(activeTab().id, "Exited Administrator Mode."); });
  adminHeader->addChild(std::move(finishButton));

  appBar->addChild(std::move(adminHeader));

  auto appSpacer = std::make_unique<Flex>();
  appSpacer->setFlexGrow(1.0F);
  appSpacer->setHitTestVisible(false);
  appBar->addChild(std::move(appSpacer));
  m_menus.attachMenuBarUi(*appBar, kToolbarHeight);

  updateAdminHeaderUi();
  return appBar;
}

void FileManager::addWindowResizeAreas(Flex& root) {
  for (std::size_t i = 0; i < kWindowResizeHandles.size(); ++i) {
    const auto [edge, cursor] = kWindowResizeHandles[i];
    auto area = std::make_unique<InputArea>();
    area->setParticipatesInLayout(false);
    area->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    area->setCursorShape(cursor);
    area->setOnPress([this, edge](const InputArea::PointerData& event) {
      if (event.pressed && event.serial && m_surface && m_surface->xdgToplevel() && m_wayland.seat()) {
        m_dispatcher.cancelPointerCapture();
        xdg_toplevel_resize(m_surface->xdgToplevel(), m_wayland.seat(), event.serial, edge);
      }
    });
    m_windowResizeAreas[i] = static_cast<InputArea*>(root.addChild(std::move(area)));
  }
}

void FileManager::layoutWindowDragAreas(Renderer& renderer, float width, float height) {
  m_windowMoveArea->setSize(m_appBar->width(), m_appBar->height());
  m_windowMoveArea->layout(renderer);

  constexpr float corner = kWindowResizeCorner;
  constexpr float inset = kWindowResizeInset;
  const std::array<LayoutRect, kWindowResizeHandles.size()> bounds{{
      {corner, 0.0F, std::max(0.0F, width - 2.0F * corner), inset},
      {corner, height - inset, std::max(0.0F, width - 2.0F * corner), inset},
      {0.0F, corner, inset, std::max(0.0F, height - 2.0F * corner)},
      {width - inset, corner, inset, std::max(0.0F, height - 2.0F * corner)},
      {0.0F, 0.0F, corner, corner},
      {width - corner, 0.0F, corner, corner},
      {0.0F, height - corner, corner, corner},
      {width - corner, height - corner, corner, corner},
  }};
  for (std::size_t i = 0; i < bounds.size(); ++i)
    m_windowResizeAreas[i]->arrange(renderer, bounds[i]);
}

void FileManager::updateAdminHeaderUi(int headerWidth) {
  if (headerWidth > 0)
    m_lastHeaderWidth = headerWidth;
  const bool adminActive = activeTab().hasActiveAdminSession();
  // At narrow header widths, prioritise the administrator status and Finish control over branding.
  const bool compactNarrowAdmin = adminActive && m_lastHeaderWidth < 760;

  const auto branding = m_settings.brandingMode();
  const bool showBrand = !compactNarrowAdmin && branding != filemanager::BrandingMode::Off;
  const bool showWordmark = showBrand && branding == filemanager::BrandingMode::Full;
  if (m_appBrandContainer) {
    m_appBrandContainer->setVisible(showBrand);
    m_appBrandContainer->setParticipatesInLayout(showBrand);
    m_appBrandNythera->setText(showWordmark ? "NYTHERA" : "N");
    m_appBrandFiles->setVisible(showWordmark);
    m_appBrandFiles->setParticipatesInLayout(showWordmark);
  }
  if (m_adminHeaderContainer != nullptr) {
    m_adminHeaderContainer->setVisible(adminActive);
    m_adminHeaderContainer->setParticipatesInLayout(adminActive);
  }
  if (m_adminHeaderSeparator != nullptr) {
    m_adminHeaderSeparator->setVisible(adminActive && showBrand);
    m_adminHeaderSeparator->setParticipatesInLayout(adminActive && showBrand);
    m_adminHeaderSeparator->setFill(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  }
  if (m_adminHeaderShield != nullptr)
    m_adminHeaderShield->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  if (m_adminHeaderLabel != nullptr)
    m_adminHeaderLabel->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  if (m_adminFinishButton != nullptr) {
    auto btnPalette = Button::defaultPalette(ButtonVariant::Outline);
    btnPalette.normal = Button::ButtonStateColors{
        .bg = colorSpecFromRole(ColorRole::ErrorContainer),
        .border = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .label = colorSpecFromRole(filemanager::kAdminWarningColorRole),
    };
    btnPalette.hover = Button::ButtonStateColors{
        .bg = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .border = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .label = colorSpecFromRole(ColorRole::OnError),
    };
    btnPalette.pressed = Button::ButtonStateColors{
        .bg = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .border = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .label = colorSpecFromRole(ColorRole::OnError),
    };
    m_adminFinishButton->setCustomPalette(std::move(btnPalette));
  }
}

bool FileManager::enterAdministratorMode(
    filemanager::TabId tabId, std::optional<std::filesystem::path> targetDirectory
) {
  auto* tab = m_tabs.findTab(tabId);
  if (tab == nullptr)
    return false;
  if (tab->session.browsingArchive()) {
    setStatus("Administrator Mode is unavailable inside archives");
    return false;
  }
  if (tab->session.browsingTrash() || (targetDirectory && filemanager::isTrashTabPath(*targetDirectory))) {
    if (tabId == m_tabs.activeTabId())
      setStatus("Administrator Mode is unavailable in Wastebin");
    return false;
  }
  if (tab->hasActiveAdminSession()) {
    if (targetDirectory && !targetDirectory->empty()) {
      tab->session.navigate(*targetDirectory, true);
      if (tabId == m_tabs.activeTabId()) {
        rebuildBreadcrumbs();
        updateNavigationControls();
        updateWindowTitle();
      }
      return true;
    }
    return false;
  }
  if (tab->adminSession.isAuthenticating()) {
    if (tabId == m_tabs.activeTabId())
      setStatus("Administrator authentication is already in progress for this tab");
    return false;
  }
  if (!tab->adminSession.hasCustomTimeoutForTesting()) {
    const int timeoutSeconds = m_settings.adminInactivityTimeoutSeconds();
    tab->adminSession.setInactivityTimeoutMs(static_cast<std::uint32_t>(timeoutSeconds) * 1000U);
  }
  const std::weak_ptr<bool> alive = m_alive;
  const bool started = tab->adminSession.startSessionAsync(
      [this, alive, tabId,
       targetDirectory = std::move(targetDirectory)](bool, const filemanager::AdminSessionStatus& status) {
        const auto locked = alive.lock();
        if (!locked || !*locked)
          return;
        const filemanager::TabId completedTabId = status.tabId != filemanager::kInvalidTabId ? status.tabId : tabId;
        auto* currentTab = m_tabs.findTab(completedTabId);
        if (currentTab == nullptr)
          return;
        if (status.state == filemanager::AdminSessionState::Active) {
          currentTab->administratorBrowsing = true;
          if (m_renamePath && completedTabId == m_tabs.activeTabId())
            cancelRename();
          if (completedTabId == m_tabs.activeTabId()) {
            cancelFileGesture();
            m_menus.closeAll();
            m_menus.setContextMenuTarget(std::nullopt);
          }
          currentTab->cancelStatusFreeSpaceQuery();
          currentTab->cancelSelectionInfoQuery();
          if (targetDirectory && !targetDirectory->empty()) {
            currentTab->session.navigate(*targetDirectory, true);
          } else {
            currentTab->session.enumerate(currentTab->session.path());
          }
          m_tabBar.markDirty();
          if (completedTabId == m_tabs.activeTabId()) {
            rebuildBreadcrumbs();
            if (m_locationInput != nullptr)
              m_locationInput->setValue(currentTab->session.path().string());
            updateNavigationControls();
            updateWindowTitle();
            updateAdminHeaderUi();
            updateStatusBar();
            setStatus("Administrator Mode active.");
          } else {
            currentTab->lastStatusMessage = "Administrator Mode active.";
          }
          if (m_surface)
            m_surface->requestLayout();
          return;
        }
        if (status.state == filemanager::AdminSessionState::Inactive) {
          // Cancelled by user or closed tab.
          if (completedTabId == m_tabs.activeTabId())
            setStatus("Administrator authentication cancelled.");
          else
            currentTab->lastStatusMessage = "Administrator authentication cancelled.";
          return;
        }
        if (status.state == filemanager::AdminSessionState::Failed) {
          const std::string msg = status.message.empty() ? "Administrator authentication failed." : status.message;
          if (completedTabId == m_tabs.activeTabId())
            setStatus(msg);
          else
            currentTab->lastStatusMessage = msg;
        }
      }
  );
  if (started && tabId == m_tabs.activeTabId())
    setStatus("Authenticating Administrator Mode…");
  return started;
}

void FileManager::finishAdministratorMode(filemanager::TabId tabId, std::string statusMessage) {
  auto* tab = m_tabs.findTab(tabId);
  if (tab == nullptr)
    return;
  m_protectedFiles.clearThumbnailsForTab(tabId);
  if (m_propertiesWindow && m_propertiesWindow->usesAdministratorTab(*tab))
    m_propertiesWindow->close();
  if (tabId == m_tabs.activeTabId()) {
    if (m_quickLookOpen)
      m_modalDialog.closeDialog(0);
    m_pendingInlineRenamePath.reset();
    if (m_renamePath) {
      m_refreshAfterRename = false;
      cancelRename();
    }
    cancelFileGesture();
    m_menus.closeAll();
    m_menus.setContextMenuTarget(std::nullopt);
    if (m_grid)
      m_grid->resetTransientStateForDataSwap();
  }
  tab->leaveAdministratorMode();
  tab->needsRefresh = false;
  struct Recovery {
    FileManager* manager;
    std::weak_ptr<bool> alive;
    filemanager::TabId tabId;
    std::uint64_t enumerationGeneration;
    std::filesystem::path currentPath;
    decltype(m_unprivilegedDirectoryAccessibleHook) accessHook;
    std::string statusMessage;
  };
  GTask* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        auto* completed = G_TASK(result);
        const auto& recovery = *static_cast<Recovery*>(g_task_get_task_data(completed));
        const std::unique_ptr<std::filesystem::path> path(
            static_cast<std::filesystem::path*>(g_task_propagate_pointer(completed, nullptr))
        );
        const auto alive = recovery.alive.lock();
        if (!alive || !*alive || !path)
          return;
        auto& manager = *recovery.manager;
        auto* origin = manager.m_tabs.findTab(recovery.tabId);
        if (!origin
            || origin->session.enumerationGeneration() != recovery.enumerationGeneration
            || origin->hasActiveAdminSession())
          return;
        if (*path == origin->session.path())
          origin->session.enumerate(*path);
        else
          origin->session.navigate(*path, false);
        if (origin->active && !recovery.statusMessage.empty())
          manager.setStatus(recovery.statusMessage);
        else
          origin->lastStatusMessage = recovery.statusMessage;
      },
      nullptr
  );
  g_task_set_task_data(
      task,
      new Recovery{
          this, m_alive, tabId, tab->session.enumerationGeneration(), tab->session.path(),
          m_unprivilegedDirectoryAccessibleHook, statusMessage
      },
      [](gpointer data) { delete static_cast<Recovery*>(data); }
  );
  g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
    const auto& recovery = *static_cast<Recovery*>(data);
    const auto accessible = [&](const std::filesystem::path& path) {
      if (path.empty() || filemanager::isTrashTabPath(path))
        return false;
      if (recovery.accessHook) {
        if (const auto overridden = recovery.accessHook(path); overridden.has_value())
          return *overridden;
      }
      std::error_code ec;
      return std::filesystem::is_directory(path, ec) && ::access(path.c_str(), R_OK | X_OK) == 0;
    };
    auto path = recovery.currentPath;
    if (!accessible(path)) {
      path = defaultTabPath();
      if (!accessible(path)) {
        path = g_get_home_dir();
        if (!accessible(path))
          path = "/";
      }
    }
    g_task_return_pointer(worker, new std::filesystem::path(std::move(path)), [](gpointer value) {
      delete static_cast<std::filesystem::path*>(value);
    });
  });
  g_object_unref(task);
  m_tabBar.markDirty();
  if (tabId == m_tabs.activeTabId()) {
    if (m_grid)
      m_grid->notifyDataChanged();
    rebuildBreadcrumbs();
    if (m_locationInput != nullptr)
      m_locationInput->setValue(tab->session.path().string());
    updateNavigationControls();
    updateWindowTitle();
    updateAdminHeaderUi();
    updateStatusBar();
    if (!statusMessage.empty())
      setStatus(std::move(statusMessage));
  } else if (!statusMessage.empty()) {
    tab->lastStatusMessage = std::move(statusMessage);
  }
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::recordAdminTabUserActivity(filemanager::BrowserTab& tab) {
  if (tab.hasActiveAdminSession())
    tab.adminSession.renewActivityAsync();
}

void FileManager::updateNavigationControls() {
  syncSearchUi();
  const auto& session = activeSession();
  if (m_backButton != nullptr)
    m_backButton->setEnabled(!session.backHistory().empty());
  if (m_forwardButton != nullptr)
    m_forwardButton->setEnabled(!session.forwardHistory().empty());
  if (m_upButton != nullptr)
    m_upButton->setEnabled(
        session.path().has_parent_path()
        && session.path().parent_path() != session.path()
        && !session.browsingRecent()
        && (!session.browsingTrash() || session.path() != std::filesystem::path("trash:/"))
    );
  m_places.updateActiveHighlight(session.path(), session.browsingTrash());
  syncSortControls();
}

void FileManager::textPrompt(
    std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
) {
  m_modalDialog.textPrompt(std::move(title), std::move(message), password, std::move(response));
}

void FileManager::setGridMode(bool gridMode) { m_listHeader.setGridMode(gridMode); }

void FileManager::adjustViewScale(float value) { m_listHeader.adjustViewScale(value); }

void FileManager::loadSearchView() {
  auto& tab = activeTab();
  tab.searchViewState = loadInitialTabViewState(m_settings, true);
  tab.searchViewState.showHidden = tab.session.showHidden();
  tab.searchViewState.foldersFirst = tab.session.foldersFirst();
  tab.searchViewState.hiddenLast = tab.session.hiddenLast();
  tab.searchScrollState = {};
}

void FileManager::applyActiveView() {
  auto& tab = activeTab();
  const auto& state = tab.displayedViewState();
  const bool search = tab.session.search().active();
  const bool geometryChanged = m_listHeader.gridMode() != state.gridMode
      || m_listHeader.viewScale() != state.viewScale
      || m_listHeader.searchView() != search;
  m_listHeader.applyTabViewState(state, search);
  if (m_grid) {
    m_grid->bindScrollState(&tab.displayedScrollState());
    if (geometryChanged)
      applyViewScale();
  }
}

void FileManager::applyViewScale() {
  if (!m_grid)
    return;
  cancelFileGesture();
  const auto metrics = filemanager::fileViewMetrics(m_listHeader.viewScale());
  m_grid->setAdapter(nullptr);
  m_grid->setAdapter(m_adapter.get());
  m_grid->setColumns(m_listHeader.gridMode() ? 0 : 1);
  m_grid->setMinCellWidth(metrics.gridCellWidth);
  m_grid->setCellHeight(
      m_listHeader.gridMode()
          ? metrics.gridCellHeight + (m_listHeader.searchView() ? 18.0F * m_listHeader.viewScale() : 0.0F)
          : metrics.listRowHeight
  );
  m_grid->setRowGap(m_listHeader.gridMode() ? 3.0F : 1.0F);
  restoreSelection();
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::rebuildBreadcrumbs() {
  if (m_breadcrumbBar == nullptr)
    return;
  while (!m_breadcrumbBar->children().empty())
    m_breadcrumbBar->removeChild(m_breadcrumbBar->children().front().get());
  auto addCrumb = [this](std::string label, filemanager::BrowserLocation path, bool last, bool archive = false) {
    if (!m_breadcrumbBar->children().empty()) {
      auto separator = std::make_unique<Label>();
      separator->setText("›");
      separator->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      separator->setFontSize(Style::fontSizeCaption);
      m_breadcrumbBar->addChild(std::move(separator));
    }
    auto button = std::make_unique<Button>();
    button->setText(filemanager::sanitizeFilenameForDisplay(label));
    if (archive)
      button->setGlyph("file-zip");
    button->setVariant(ButtonVariant::Ghost);
    button->setFocusHighlightEnabled(false);
    useSubtleSelection(*button);
    button->setSelected(false);
    if (last && button->label())
      button->label()->setFontWeight(FontWeight::SemiBold);
    button->setControlHeight(kToolbarHeight);
    button->setPadding(5.0F);
    button->setOnClick([this, path = std::move(path)] { navigateLocation(path); });
    m_breadcrumbBar->addChild(std::move(button));
  };
  const auto& session = activeSession();
  const auto crumbs = filemanager::fileBreadcrumbs(session.path());
  for (std::size_t i = 0; i < crumbs.size(); ++i) {
    const bool last = i + 1 == crumbs.size();
    if (last && session.browsingArchive())
      addCrumb(crumbs[i].label, {session.path(), std::string{}}, session.archiveDirectory()->empty(), true);
    else
      addCrumb(crumbs[i].label, crumbs[i].path, last);
  }
  if (session.browsingArchive()) {
    std::filesystem::path prefix;
    for (const auto& part : std::filesystem::path(*session.archiveDirectory())) {
      if (part.empty())
        continue;
      prefix /= part;
      addCrumb(part.native(), {session.path(), prefix.native()}, prefix.native() == *session.archiveDirectory());
    }
  }
  auto emptyField = std::make_unique<Button>();
  emptyField->setVariant(ButtonVariant::Ghost);
  emptyField->setFocusHighlightEnabled(false);
  emptyField->setTabStop(false);
  emptyField->setControlHeight(kToolbarHeight);
  emptyField->setFlexGrow(1.0F);
  emptyField->setOnClick([this] { setLocationEditMode(true); });
  m_breadcrumbBar->addChild(std::move(emptyField));
}

void FileManager::setLocationEditMode(bool editing, bool preserveOutsideClickFocus) {
  ++m_locationEditGeneration;
  m_editingLocation = editing;
  if (!editing)
    closeLocationAutocomplete();
  const bool showPath = editing || m_settings.addressBarMode() == filemanager::AddressBarMode::FullPath;
  m_wayland.stopKeyRepeat();
  m_selection.clearTypeahead();
  if (m_locationInput != nullptr) {
    m_locationInput->setVisible(showPath);
    m_locationInput->setParticipatesInLayout(showPath);
    if (!editing)
      m_locationInput->setValue(activeSession().path().string());
  }
  if (m_breadcrumbBar != nullptr) {
    m_breadcrumbBar->setVisible(!showPath);
    m_breadcrumbBar->setParticipatesInLayout(!showPath);
  }
  if (editing && m_locationInput != nullptr) {
    m_locationInput->setValue(activeSession().path().string());
    m_dispatcher.setFocus(m_locationInput->inputArea());
    m_locationInput->selectAll();
  } else if (m_grid != nullptr) {
    if (!preserveOutsideClickFocus
        || m_dispatcher.focusedArea() == (m_locationInput ? m_locationInput->inputArea() : nullptr))
      m_dispatcher.setFocus(m_grid->focusArea());
  }
  if (m_surface != nullptr)
    m_surface->requestLayout();
}

void FileManager::deferCancelLocationEdit() {
  if (!m_editingLocation)
    return;
  const auto generation = m_locationEditGeneration;
  const std::weak_ptr<bool> alive = m_alive;
  DeferredCall::callLater([this, alive, generation] {
    const auto locked = alive.lock();
    if (locked
        && *locked
        && filemanager::locationEditCancelIsCurrent(m_editingLocation, m_locationEditGeneration, generation))
      setLocationEditMode(false, true);
  });
}

void FileManager::pasteLocationFromClipboard() {
  if (!m_windowOpen
      || !m_editingLocation
      || !m_locationInput
      || m_dispatcher.focusedArea() != m_locationInput->inputArea())
    return;

  const auto generation = m_locationEditGeneration;
  const auto original = m_locationInput->textInputState();
  auto onText = [this, alive = std::weak_ptr<bool>(m_alive), generation, original](std::optional<std::string> text) {
    const auto locked = alive.lock();
    if (!locked
        || !*locked
        || !m_windowOpen
        || !m_editingLocation
        || generation != m_locationEditGeneration
        || !m_locationInput
        || m_dispatcher.focusedArea() != m_locationInput->inputArea())
      return;
    if (!text
        || text->empty()
        || text->find('\0') != std::string::npos
        || !g_utf8_validate(text->data(), static_cast<gssize>(text->size()), nullptr))
      return;

    const auto current = m_locationInput->textInputState();
    if (current.surroundingText != original.surroundingText
        || current.cursor != original.cursor
        || current.anchor != original.anchor)
      return;
    m_locationInput->textInputApplyEdit({.commitText = std::move(*text), .hasCommitText = true});
  };
  if (m_readLocationClipboardForTesting)
    m_readLocationClipboardForTesting(std::move(onText));
  else
    m_transfer.readTextSelection(std::move(onText));
}

void FileManager::setSortField(FileDialogSortField field) { m_listHeader.setSortField(field); }

void FileManager::toggleSortOrder() { m_listHeader.toggleSortOrder(); }

void FileManager::syncSortControls() { m_listHeader.syncSortControls(); }

std::unique_ptr<Flex> FileManager::buildStatusRow(float fontSize) {
  auto statusRow = std::make_unique<Flex>();
  statusRow->setAlign(FlexAlign::Center);
  statusRow->setMirrorInRtl(false);
  statusRow->setGap(Style::spaceSm);
  statusRow->setFlexGrow(1.0F);

  auto summary = std::make_unique<Flex>();
  summary->setAlign(FlexAlign::Center);
  summary->setMirrorInRtl(false);
  summary->setFlexGrow(1.0F);
  summary->setClipChildren(true);
  auto primary = std::make_unique<Label>();
  primary->setFontSize(fontSize);
  primary->setMaxLines(1);
  primary->setEllipsize(TextEllipsize::Middle);
  primary->setColor(colorSpecFromRole(ColorRole::OnSurface));
  m_statusLabel = static_cast<Label*>(summary->addChild(std::move(primary)));
  auto detail = std::make_unique<Label>();
  detail->setFontSize(fontSize);
  detail->setMaxLines(1);
  detail->setEllipsize(TextEllipsize::Middle);
  detail->setFlexGrow(1.0F);
  detail->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_statusDetailLabel = static_cast<Label*>(summary->addChild(std::move(detail)));
  statusRow->addChild(std::move(summary));

  m_searchBarController.attachStatusArea(*statusRow, fontSize);

  return statusRow;
}

void FileManager::prepareFrame(bool needsUpdate, bool needsLayout) {
  (void)needsUpdate;
  if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0)
    return;
  m_renderContext.makeCurrent(m_surface->renderTarget());
  if (m_places.isDirty()) {
    const bool focusInSidebar = isNodeDescendantOf(m_dispatcher.focusedArea(), m_sidebar);
    const auto snapshot = focusInSidebar ? m_dispatcher.captureTabFocus() : InputDispatcher::TabFocusSnapshot{};
    m_places.rebuild();
    if (focusInSidebar)
      m_dispatcher.restoreTabFocus(snapshot);
  }
  Renderer& renderer = m_surface->renderTarget().renderer();
  if (m_pickerThumbnails.uploadPending(renderer.textureManager()) && m_grid && activeTab().hasActiveAdminSession())
    m_grid->notifyDataChanged();
  m_adapter->setRenderer(&renderer);
  if (!m_root) {
    m_root = std::make_unique<Flex>();
    m_root->setAlign(FlexAlign::Stretch);
    m_root->setGap(kPaneGap);
    m_root->setPadding(kWindowPadding);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, kFilesWindowOpacity));
    m_root->setSurfaceBackground();
    m_root->setBorder(
        colorSpecFromRole(
            ColorRole::OutlineVariant, 0.72F /* palette-alpha-exception: This rim is part of the approved Files window
                                                glass and has no matching opaque role. */
        ),
        Style::borderWidth
    );
    m_root->setRadius(Style::scaledRadiusLg(1.0F));

    auto sidebar = std::make_unique<Flex>();
    sidebar->setDirection(FlexDirection::Vertical);
    sidebar->setAlign(FlexAlign::Stretch);
    sidebar->setGap(3.0F);
    sidebar->setPadding(7.0F);
    m_sidebarWidth = m_settings.loadSidebarWidth(m_sidebarWidth, kSidebarMinWidth, kSidebarMaxWidth);
    sidebar->setMinWidth(m_sidebarWidth);
    sidebar->setMaxWidth(m_sidebarWidth);
    sidebar->setFill(colorSpecFromRole(ColorRole::SurfaceContainer, kFilesSidebarOpacity));
    sidebar->setBorder(
        colorSpecFromRole(
            ColorRole::OutlineVariant, 0.42F /* palette-alpha-exception: The sidebar rim mixes with the pane behind it.
                                                The palette has no equivalent role. */
        ),
        Style::borderWidth
    );
    sidebar->setRadius(Style::scaledRadiusMd(1.0F));
    auto placesScroll = std::make_unique<ScrollView>();
    placesScroll->setFlexGrow(1.0F);
    placesScroll->setScrollbarVisible(true);
    auto* placesScrollPtr = placesScroll.get();
    auto* placesContent = placesScroll->content();
    placesContent->setAlign(FlexAlign::Stretch);
    sidebar->addChild(std::move(placesScroll));
    m_places.attachUi(placesScrollPtr, placesContent);
    m_places.rebuild();
    m_sidebar = static_cast<Flex*>(m_root->addChild(std::move(sidebar)));

    auto divider = std::make_unique<Flex>();
    divider->setMinWidth(kSidebarDividerWidth);
    divider->setMaxWidth(kSidebarDividerWidth);
    divider->setFill(clearColorSpec());
    divider->setHitTestVisible(true);
    m_sidebarDivider = static_cast<Flex*>(m_root->addChild(std::move(divider)));

    auto mainColumn = std::make_unique<Flex>();
    mainColumn->setDirection(FlexDirection::Vertical);
    mainColumn->setAlign(FlexAlign::Stretch);
    constexpr float toolbarGap = Style::spaceSm;
    constexpr float paneGap = 2.0F;
    mainColumn->setGap(toolbarGap);
    mainColumn->setFlexGrow(1.0F);
    mainColumn->addChild(buildHeaderAppBar());

    auto mainPane = std::make_unique<Flex>();
    mainPane->setDirection(FlexDirection::Vertical);
    mainPane->setAlign(FlexAlign::Stretch);
    mainPane->setGap(paneGap);
    mainPane->setFlexGrow(1.0F);
    m_statusHeight = m_settings.loadStatusHeight(m_statusHeight, kStatusMinHeight, kStatusMaxHeight);

    auto navigation = std::make_unique<Flex>();
    navigation->setAlign(FlexAlign::Center);
    navigation->setGap(2.0F);
    navigation->addChild(makeButton("‹", [this] { navigateBack(); }, &m_backButton));
    navigation->addChild(makeButton("›", [this] { navigateForward(); }, &m_forwardButton));
    navigation->addChild(makeButton("↑", [this] { navigateUp(); }, &m_upButton));

    auto locationBar = std::make_unique<Flex>();
    locationBar->setAlign(FlexAlign::Center);
    locationBar->setGap(2.0F);
    locationBar->setPadding(2.0F, 4.0F);
    locationBar->setFlexGrow(1.0F);
    locationBar->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, kFilesLocationBarOpacity));
    locationBar->setBorder(
        colorSpecFromRole(
            ColorRole::Primary, 0.28F /* palette-alpha-exception: The location bar uses a subdued primary rim distinct
                                         from the focus indicator. */
        ),
        Style::borderWidth
    );
    locationBar->setRadius(Style::scaledRadiusSm(1.0F));
    auto location = std::make_unique<Input>();
    m_locationInput = location.get();
    m_locationInput->setValue(activeSession().path().string());
    m_locationInput->setPlaceholder("Enter a folder path");
    m_locationInput->setFlexGrow(1.0F);
    // This standalone file-manager process does not share the shell's global Input clipboard.
    // Route text copies through the same Wayland clipboard writer used by its other copy actions.
    m_locationInput->setOnCopyText([this](std::string text) {
      writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8");
    });
    const bool fullPath = m_settings.addressBarMode() == filemanager::AddressBarMode::FullPath;
    m_locationInput->setVisible(fullPath);
    m_locationInput->setParticipatesInLayout(fullPath);
    m_locationInput->setOnFocusGain([this] { m_editingLocation = true; });
    m_locationInput->setOnSubmit([this](const std::string& value) {
      navigate(filemanager::expandHomePath(value, g_get_home_dir()));
      setLocationEditMode(false);
    });
    m_locationInput->setOnFocusLoss([this] { deferCancelLocationEdit(); });
    locationBar->addChild(std::move(location));
    auto crumb = std::make_unique<Flex>();
    crumb->setAlign(FlexAlign::Center);
    crumb->setGap(1.0F);
    crumb->setFlexGrow(1.0F);
    crumb->setClipChildren(true);
    crumb->setMinHeight(kToolbarHeight);
    m_breadcrumbBar = static_cast<Flex*>(locationBar->addChild(std::move(crumb)));
    navigation->addChild(std::move(locationBar));
    navigation->addChild(
        makeButton("↻", [this] { activateContextMenuEntry({.id = filemanager::menu_id::kRefresh}); }, &m_refreshButton)
    );
    Button* searchBtn = nullptr;
    auto searchButton = makeButton("", [this] { toggleSearch(); }, &searchBtn);
    searchButton->setGlyph("search");
    searchButton->setTooltip("Search (Ctrl+F)");
    m_searchBarController.setSearchButton(searchBtn);
    navigation->addChild(std::move(searchButton));
    mainPane->addChild(std::move(navigation));

    mainPane->addChild(m_tabBar.buildUi());
    m_searchBarController.buildSearchBar(*mainPane, toolbarGap - paneGap);

    auto fileViewRow = std::make_unique<Flex>();
    fileViewRow->setAlign(FlexAlign::Stretch);
    fileViewRow->setGap(kPaneGap);
    fileViewRow->setFlexGrow(1.0F);
    auto fileView = std::make_unique<Flex>();
    fileView->setDirection(FlexDirection::Vertical);
    fileView->setAlign(FlexAlign::Stretch);
    fileView->setGap(paneGap);
    fileView->setFlexGrow(1.0F);
    fileView->addChild(m_listHeader.buildHeader());
    fileView->addChild(
        ui::separator({
            .color = colorSpecFromRole(
                ColorRole::OutlineVariant, 0.45F /* palette-alpha-exception: The list divider is deliberately softer
                                                    than the opaque palette outline. */
            ),
            .gradientEdges = false,
        })
    );

    auto grid = std::make_unique<VirtualGridView>();
    m_grid = grid.get();
    m_listHeader.attachGrid(m_grid);
    m_grid->setFlexGrow(1.0F);
    m_grid->setFill(colorSpecFromRole(ColorRole::Surface, kFilesGridOpacity));
    m_grid->setBorder(
        colorSpecFromRole(
            ColorRole::OutlineVariant, 0.26F /* palette-alpha-exception: The grid rim is layered over a translucent
                                                pane. An opaque outline changes the bevel. */
        ),
        Style::borderWidth
    );
    m_grid->setRadius(Style::scaledRadiusMd(1.0F));
    m_grid->setAdapter(m_adapter.get());
    m_grid->setSquareCells(false);
    m_listHeader.ensureViewScaleLoaded();
    const auto metrics = filemanager::fileViewMetrics(m_listHeader.viewScale());
    m_grid->setColumns(m_listHeader.gridMode() ? 0 : 1);
    m_grid->setMinCellWidth(metrics.gridCellWidth);
    m_grid->setCellHeight(
        m_listHeader.gridMode()
            ? metrics.gridCellHeight + (m_listHeader.searchView() ? 18.0F * m_listHeader.viewScale() : 0.0F)
            : metrics.listRowHeight
    );
    m_grid->setRowGap(m_listHeader.gridMode() ? 3.0F : 1.0F);
    m_grid->bindScrollState(&activeTab().displayedScrollState());
    m_listHeader.updateColumnHeaders();
    m_grid->setOnRubberBandSelection([this](const std::vector<std::size_t>& hits, bool finished, bool cancelled) {
      m_selection.handleRubberBandSelection(hits, finished, cancelled);
    });
    m_grid->setOnSelectionChanged([this](std::optional<std::size_t> index) {
      m_selection.handleGridSelectionChanged(index);
    });
    fileView->addChild(std::move(grid));
    fileView->addChild(m_listHeader.buildHorizontalScrollbar());
    fileViewRow->addChild(std::move(fileView));
    attachPreviewPane(*fileViewRow, kSidebarDividerWidth, colorSpecFromRole(ColorRole::Surface, kFilesGridOpacity));
    mainPane->addChild(std::move(fileViewRow));

    const float statusFontSize =
        12.0F + (m_statusHeight - kStatusMinHeight) * (10.0F / (kStatusMaxHeight - kStatusMinHeight));
    auto statusDivider = std::make_unique<Flex>();
    statusDivider->setMinHeight(4.0F);
    statusDivider->setMaxHeight(4.0F);
    statusDivider->setHitTestVisible(true);
    statusDivider->setFill(clearColorSpec());
    m_statusDivider = static_cast<Flex*>(mainPane->addChild(std::move(statusDivider)));
    auto statusStrip = std::make_unique<Flex>();
    statusStrip->setAlign(FlexAlign::Center);
    statusStrip->setPadding(2.0F, 10.0F);
    statusStrip->setFill(colorSpecFromRole(ColorRole::SurfaceContainer, kFilesStatusStripOpacity));
    statusStrip->setBorder(
        colorSpecFromRole(
            ColorRole::OutlineVariant,
            0.55F /* palette-alpha-exception: The status strip rim is designed to blend over Files glass. */
        ),
        Style::borderWidth
    );
    statusStrip->setRadius(Style::scaledRadiusSm(1.0F));
    statusStrip->setMinHeight(m_statusHeight);
    statusStrip->setMaxHeight(m_statusHeight);
    statusStrip->setHitTestVisible(true);
    m_statusStrip = static_cast<Flex*>(mainPane->addChild(std::move(statusStrip)));
    m_statusStrip->addChild(buildStatusRow(statusFontSize));
    updateStatusBar();
    m_statusStrip->setPadding(std::clamp((m_statusHeight - statusFontSize) * 0.5F, 2.0F, 7.0F), 10.0F);
    mainColumn->addChild(std::move(mainPane));
    m_root->addChild(std::move(mainColumn));
    addWindowResizeAreas(*m_root);
    m_dispatcher.setSceneRoot(m_root.get());
    m_dispatcher.setFocus(m_grid->focusArea());
    m_surface->setSceneRoot(m_root.get());
    rebuildBreadcrumbs();
    updateNavigationControls();
    setLocationEditMode(false);
    needsLayout = true;
  }
  if (m_tabBar.rebuildIfDirty())
    needsLayout = true;
  if (m_transfers.applyDragScroll())
    needsLayout = true;
  if (m_transfers.isDraggingFiles() || m_transfers.hasDragPreview()) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_transfers.updateDragPreview(renderer);
  }
  if (needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    const float maxSidebar =
        std::min(kSidebarMaxWidth, std::max(kSidebarMinWidth, static_cast<float>(m_surface->width()) - 300.0F));
    const float effectiveSidebar = std::clamp(m_sidebarWidth, kSidebarMinWidth, maxSidebar);
    if (m_sidebar) {
      m_sidebar->setMinWidth(effectiveSidebar);
      m_sidebar->setMaxWidth(effectiveSidebar);
    }
    m_places.updateCapacityBarWidths(effectiveSidebar);
    const bool showSidebar = m_surface->width() >= 760;
    m_sidebar->setVisible(showSidebar);
    m_sidebar->setParticipatesInLayout(showSidebar);
    m_sidebarDivider->setVisible(showSidebar);
    m_sidebarDivider->setParticipatesInLayout(showSidebar);
    const float sidebarSpace = showSidebar ? effectiveSidebar + kSidebarDividerWidth + 2.0F * kPaneGap : 0.0F;
    const float available = static_cast<float>(m_surface->width()) - 2.0F * kWindowPadding - sidebarSpace;
    layoutPreviewPane(available, kSidebarDividerWidth + 2.0F * kPaneGap);
    const int headerWidth = static_cast<int>(available);
    m_menus.syncResponsiveMenuBar(headerWidth);
    updateAdminHeaderUi(headerWidth);
    const float statusWidth =
        std::max(1.0F, static_cast<float>(headerWidth) - m_statusStrip->paddingLeft() - m_statusStrip->paddingRight());
    float summaryWidth = statusWidth;
    m_searchBarController.layoutForFrame(
        static_cast<float>(headerWidth),
        static_cast<float>(m_surface->width()) - (showSidebar ? effectiveSidebar : 0.0F), statusWidth, renderer,
        summaryWidth
    );
    m_statusLabel->setMaxWidth(
        std::max(1.0F, m_statusDetailLabel->text().empty() ? summaryWidth : summaryWidth * 0.6F)
    );
    m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
    m_root->layout(renderer);
    layoutWindowDragAreas(renderer, m_root->width(), m_root->height());
    m_tabBar.syncLayout(renderer);
    m_listHeader.applyPendingFit(renderer);
    if (m_listHeader.applyColumnWidths())
      m_surface->requestLayout();
    m_listHeader.arrangeAfterWidthChange(renderer);
    m_listHeader.positionColumnMarkers();
    finishPendingTransferReveal(renderer);
    m_listHeader.layoutVisibleMarkers(renderer);
    syncTooltipHover();
  }
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    updateSearchThumbnails(renderer);
    updateSearchFolderCounts();
    renderPreviewPane(renderer);
  }
  if (m_transfers.refreshDropTargetAfterScroll() && (m_transfers.isDraggingFiles() || m_transfers.hasDragPreview())) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_transfers.updateDragPreview(renderer);
  }
  if (m_pendingInlineRenamePath && !activeSession().loading()) {
    const auto pending = *m_pendingInlineRenamePath;
    m_pendingInlineRenamePath.reset();
    beginRenamePath(pending);
  }
}

void FileManager::syncProgressWindow() {
  m_progressWindow.sync(m_surface.get(), m_modalDialog.backgroundConflictPending());
}

void FileManager::requestQuit(ToplevelSurface* parent) {
  if (!m_operations.active()) {
    m_quitting = true;
    if (m_surface && m_windowOpen)
      performWindowClose();
    if (m_clipboard.pendingClipboardReads() == 0)
      m_running = false;
    return;
  }
  const auto activeCount =
      std::ranges::count_if(m_operations.operations(), [](const auto& operation) { return !operation.finished; });
  showDialog(
      "Quit Nythera Files?",
      "Quitting will stop "
          + std::to_string(activeCount)
          + (activeCount == 1 ? " active file operation. Incomplete staged copies will be removed."
                              : " active file operations. Incomplete staged copies will be removed."),
      {{"Cancel", 0, filemanager::DialogChoiceStyle::Cancel}, {"Quit", 1, filemanager::DialogChoiceStyle::Destructive}},
      [this](int choice) {
        if (choice != 1)
          return;
        for (const auto& operation : m_operations.operations())
          if (!operation.finished)
            m_operations.cancel(operation.id);
        m_quitting = true;
        if (m_surface && m_windowOpen)
          performWindowClose();
        if (!m_operations.active() && m_clipboard.pendingClipboardReads() == 0)
          m_running = false;
      },
      parent
  );
}

std::optional<std::filesystem::path> FileManager::checksumTarget() const {
  const auto& tab = activeTab();
  if (!tab.capabilities().canChecksum
      || !tab.selectedPath()
      || !tab.isPathSelected(tab.selectedPath()->string())
      || tab.selectionCount() != 1
      || tab.session.loading()
      || tab.session.browsingTrash())
    return std::nullopt;
  if (tab.hasActiveAdminSession() && !filemanager::userCanReadPath(*tab.selectedPath())) {
    const auto entry = std::ranges::find(tab.session.entries(), *tab.selectedPath(), &FileEntry::absPath);
    if (entry != tab.session.entries().end()
        && !entry->isDir
        && !entry->isSymlink
        && !entry->type.starts_with("inode/"))
      return tab.selectedPath();
    return std::nullopt;
  }
  return filemanager::checksum::eligibleSelection(*tab.selectedPath(), 1, true) ? tab.selectedPath() : std::nullopt;
}

std::array<ContextMenuControlEntry, 2> FileManager::checksumMenuEntries() const {
  return filemanager::ChecksumDialogController::menuEntries(checksumTarget().has_value());
}

void FileManager::showChecksumDialog(bool verify) {
  const auto target = checksumTarget();
  if (!target)
    return;
  auto& tab = activeTab();
  filemanager::ChecksumDialogController::ProtectedChecksum protectedChecksum;
  if (tab.hasActiveAdminSession() && !filemanager::userCanReadPath(*target)) {
    protectedChecksum = [this, id = tab.id, generation = tab.adminSession.sessionGeneration(), path = *target](
                            auto algorithm
                        ) -> std::optional<filemanager::AdminSessionClient::OperationTransport> {
      auto* origin = m_tabs.findTab(id);
      if (!origin || !origin->hasActiveAdminSession() || origin->adminSession.sessionGeneration() != generation)
        return std::nullopt;
      return origin->adminSession.runOperation(
          {.kind = filemanager::file_operation_engine::Kind::ProtectedChecksum,
           .destination = path.string(),
           .contentRevision = std::to_string(static_cast<unsigned>(algorithm))},
          generation
      );
    };
  }
  m_checksumDialog.show(*target, verify, std::move(protectedChecksum));
}

void FileManager::showDialog(
    std::string title, std::string message, std::vector<filemanager::DialogChoice> choices,
    std::function<void(int)> response, ToplevelSurface* parent, filemanager::DialogStyle style
) {
  m_modalDialog.showDialog(
      std::move(title), std::move(message), std::move(choices), std::move(response), parent, std::nullopt, style
  );
}

void FileManager::closeDialog(int response) { m_modalDialog.closeDialog(response); }

void FileManager::syncTooltipHover() {
  m_dispatcher.syncPointerHover();
  const auto column = m_filePointerX ? m_listHeader.columnAt(*m_filePointerX) : std::nullopt;
  if (column != m_hoveredFileColumn) {
    m_hoveredFileColumn = column;
    if (m_dispatcher.hoveredArea())
      m_dispatcher.hoveredArea()->requestTooltipRefresh();
  }
  if (m_surface && !activeModalDialog().hoverTooltipsEnabled())
    TooltipManager::instance().onHoverChange(
        m_dispatcher.hoveredArea(), m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
    );
}

void FileManager::handlePointer(const PointerEvent& event) {
  if (m_surface && event.surface == m_surface->wlSurface()) {
    if (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion) {
      m_filePointerX = static_cast<float>(event.sx);
      TooltipManager::instance().setPointerPosition(*m_filePointerX, static_cast<float>(event.sy));
    }
    // Menu routing below can consume the leave before the main dispatcher sees it.
    if (event.type == PointerEvent::Type::Leave) {
      m_filePointerX.reset();
      m_dispatcher.pointerLeave();
    }
    if (event.type == PointerEvent::Type::Axis || (event.type == PointerEvent::Type::Button && event.pressed))
      TooltipManager::instance().forceDestroy();
  } else if (event.type == PointerEvent::Type::Enter) {
    m_filePointerX.reset();
    m_dispatcher.pointerLeave();
  }
  dispatchPointer(event);
  syncTooltipHover();
}

void FileManager::dispatchPointer(const PointerEvent& event) {
  if (m_editingLocation
      && m_locationInput != nullptr
      && m_surface
      && event.surface == m_surface->wlSurface()
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && (event.button == BTN_LEFT || event.button == BTN_RIGHT)) {
    float inputX = 0.0F;
    float inputY = 0.0F;
    Node::absolutePosition(m_locationInput, inputX, inputY);
    if (filemanager::pointerOutsideRect(
            static_cast<float>(event.sx), static_cast<float>(event.sy), inputX, inputY, m_locationInput->width(),
            m_locationInput->height()
        ))
      deferCancelLocationEdit();
    else if (event.button == BTN_RIGHT && m_surface) {
      m_lastInputSerial = event.serial;
      openLocationContextMenu(static_cast<float>(event.sx), static_cast<float>(event.sy), false);
      return;
    }
  }
  if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_RIGHT)
    TooltipManager::instance().forceDestroy();
  if (m_menus.handlePointer(event))
    return;
  if (m_surface
      && event.surface == m_surface->wlSurface()
      && event.type == PointerEvent::Type::Button
      && event.button == BTN_RIGHT
      && event.pressed
      && !m_modalDialog.isOpen()) {
    for (Button* button : {m_backButton, m_forwardButton}) {
      if (!button)
        continue;
      float x = 0.0F, y = 0.0F;
      Node::absolutePosition(button, x, y);
      if (!filemanager::pointerOutsideRect(
              static_cast<float>(event.sx), static_cast<float>(event.sy), x, y, button->width(), button->height()
          )) {
        m_lastInputSerial = event.serial;
        openHistoryMenu(button == m_forwardButton, static_cast<float>(event.sx), static_cast<float>(event.sy));
        return;
      }
    }
  }
  if (event.type == PointerEvent::Type::Button && event.pressed)
    m_lastInputSerial = event.serial;
  if (m_modalDialog.isOpen()) {
    if (m_quickLookOpen
        && m_surface
        && event.surface == m_surface->wlSurface()
        && event.type == PointerEvent::Type::Button
        && event.pressed) {
      closeDialog(0);
      return;
    }
    activeModalDialog().handlePointer(event);
    return;
  }
  if (m_progressWindow.ownsSurface(event.surface)) {
    m_progressWindow.handlePointer(event);
    return;
  }
  if (!m_surface || event.surface != m_surface->wlSurface())
    return;
  if ((event.type == PointerEvent::Type::Button && event.pressed) || event.type == PointerEvent::Type::Axis)
    recordAdminTabUserActivity(activeTab());
  const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  if (m_renamePath
      && m_inlineEditArea
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && (event.button == BTN_LEFT || event.button == BTN_RIGHT)) {
    float editorX = 0.0F;
    float editorY = 0.0F;
    Node::absolutePosition(m_inlineEditArea, editorX, editorY);
    if (filemanager::pointerOutsideRect(
            x, y, editorX, editorY, m_inlineEditArea->width(), m_inlineEditArea->height()
        )) {
      m_dispatcher.setFocus(m_inlineEditArea);
      m_dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true, false);
      if (m_renamePath)
        cancelRename();
    }
  }
  if (m_transfers.isDraggingFiles() && !m_transfer.dragging() && m_transfers.handleFileDragPointer(event, x, y))
    return;
  if (m_tabBar.handlePointer(event, x, y))
    return;
  if (m_listHeader.handlePointer(event, x, y))
    return;
  if (m_grid
      && event.type == PointerEvent::Type::Axis
      && (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL
          || (event.axis == WL_POINTER_AXIS_VERTICAL_SCROLL && (m_keyboardModifiers & KeyMod::Shift)))) {
    const InputArea::PointerData scroll{
        .axis = event.axis,
        .axisSource = event.axisSource,
        .axisValue = event.axisValue,
        .axisDiscrete = event.axisDiscrete,
        .axisValue120 = event.axisValue120,
        .axisLines = event.axisLines,
    };
    if ((m_grid->containsScenePoint(x, y) || m_listHeader.headerAt(x, y))
        && m_listHeader.scrollHorizontally(scroll.scrollDelta(32.0F)))
      return;
  }
  if (m_statusDivider && m_statusStrip) {
    float dx = 0.0F, dy = 0.0F;
    Node::absolutePosition(m_statusDivider, dx, dy);
    const bool over =
        x >= dx && x <= dx + m_statusDivider->width() && y >= dy - 2.0F && y <= dy + m_statusDivider->height() + 2.0F;
    if ((over || m_resizingStatus) && m_pointerSerial)
      m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE);
    if (over && (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      m_statusDivider->setFill(
          colorSpecFromRole(ColorRole::Primary, m_resizingStatus ? 1.0F : kResizeDividerHoverOpacity)
      );
      m_surface->requestRedraw();
    } else if (!m_resizingStatus) {
      m_statusDivider->setFill(clearColorSpec());
    }
    if (m_resizingStatus)
      m_statusDivider->setFill(colorSpecFromRole(ColorRole::Primary));
    if (over && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed) {
      m_dispatcher.cancelPointerCapture();
      m_resizingStatus = true;
      m_statusDragY = y;
      m_statusDragHeight = m_statusHeight;
      return;
    }
    if (m_resizingStatus && event.type == PointerEvent::Type::Motion) {
      m_statusHeight = std::clamp(m_statusDragHeight + m_statusDragY - y, kStatusMinHeight, kStatusMaxHeight);
      const float fontSize =
          12.0F + (m_statusHeight - kStatusMinHeight) * (10.0F / (kStatusMaxHeight - kStatusMinHeight));
      m_statusStrip->setMinHeight(m_statusHeight);
      m_statusStrip->setMaxHeight(m_statusHeight);
      m_statusStrip->setPadding(std::clamp((m_statusHeight - fontSize) * 0.5F, 2.0F, 7.0F), 10.0F);
      m_statusLabel->setFontSize(fontSize);
      m_statusDetailLabel->setFontSize(fontSize);
      m_searchBarController.setStatusFontSize(fontSize);
      m_surface->requestLayout();
      return;
    }
    if (m_resizingStatus && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      m_resizingStatus = false;
      (void)m_settings.saveStatusHeight(m_statusHeight);
      m_dispatcher.cancelPointerCapture();
      return;
    }
  }
  if (event.type == PointerEvent::Type::Leave && (m_transfers.isDraggingFiles() || m_transfers.isDragPending())) {
    m_transfers.handlePointerLeaveDuringDrag();
    return;
  }
  if (event.type == PointerEvent::Type::Enter && event.serial)
    m_pointerSerial = event.serial;
  if ((m_transfer.dragging() && !m_transfer.dragDropped()) || m_transfers.isProtocolDragInside()) {
    if (event.type == PointerEvent::Type::Axis)
      m_transfers.handleAxisDuringProtocolDrag(event);
    return;
  }
  if (m_transfer.dragDropped() && (m_transfers.isDragPending() || m_transfers.isDraggingFiles()))
    cancelFileGesture();
  if (event.type == PointerEvent::Type::Axis
      && (m_keyboardModifiers & KeyMod::Ctrl)
      && !m_transfers.isDragPending()
      && !m_transfers.isDraggingFiles()
      && !m_places.isReordering()
      && m_grid
      && !m_renamePath) {
    float gx = 0, gy = 0;
    Node::absolutePosition(m_grid, gx, gy);
    if (x >= gx && x < gx + m_grid->width() && y >= gy && y < gy + m_grid->height()) {
      const double delta = event.axisValue120 != 0 ? event.axisValue120
          : event.axisDiscrete != 0                ? event.axisDiscrete
                                                   : event.axisValue;
      if (delta != 0 && event.axis == 0) {
        adjustViewScale(
            m_listHeader.viewScale() + (delta < 0 ? filemanager::kFileViewZoomStep : -filemanager::kFileViewZoomStep)
        );
      }
      return;
    }
  }
  if (m_places.handleActiveReorderPointer(event, x, y, m_pointerSerial))
    return;
  if (m_transfers.handleFileDragPointer(event, x, y))
    return;
  if (!m_resizingSidebar && handlePreviewDivider(event, x, y))
    return;
  if (m_sidebarDivider != nullptr && m_sidebar != nullptr && m_sidebarDivider->visible()) {
    float dividerX = 0, dividerY = 0;
    Node::absolutePosition(m_sidebarDivider, dividerX, dividerY);
    const bool overDivider = x >= dividerX - 2.0F
        && x <= dividerX + m_sidebarDivider->width() + 2.0F
        && y >= dividerY
        && y <= dividerY + m_sidebarDivider->height();
    const bool feedback = overDivider || m_resizingSidebar;
    if (overDivider
        && !m_resizingSidebar
        && (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      m_sidebarDivider->setFill(colorSpecFromRole(ColorRole::Primary, kResizeDividerHoverOpacity));
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      m_surface->requestRedraw();
      return;
    }
    m_sidebarDivider->setFill(
        feedback ? colorSpecFromRole(ColorRole::Primary, m_resizingSidebar ? 1.0F : kResizeDividerHoverOpacity)
                 : clearColorSpec()
    );
    if (m_resizingSidebar || overDivider) {
      m_sidebarDivider->setFill(
          colorSpecFromRole(ColorRole::Primary, m_resizingSidebar ? 1.0F : kResizeDividerHoverOpacity)
      );
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed && overDivider) {
      m_dispatcher.cancelPointerCapture();
      m_resizingSidebar = true;
      m_sidebarDragOffset = x;
      m_sidebarDragWidth = m_sidebar->width();
      return;
    }
    if (m_resizingSidebar && event.type == PointerEvent::Type::Motion) {
      const float maxWidth =
          std::min(kSidebarMaxWidth, std::max(kSidebarMinWidth, static_cast<float>(m_surface->width()) - 300));
      // Both coordinates are surface-local logical pixels. Apply one delta from
      // the press, independent of layout origin, render scale and intermediate clamps.
      m_sidebarWidth = std::clamp(m_sidebarDragWidth + x - m_sidebarDragOffset, kSidebarMinWidth, maxWidth);
      m_sidebar->setMinWidth(m_sidebarWidth);
      m_sidebar->setMaxWidth(m_sidebarWidth);
      m_places.updateCapacityBarWidths(m_sidebarWidth);
      m_surface->requestLayout();
      return;
    }
    if (m_resizingSidebar && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      m_resizingSidebar = false;
      (void)m_settings.saveSidebarWidth(m_sidebarWidth);
      m_dispatcher.cancelPointerCapture();
      return;
    }
  }
  if (m_selection.handleActiveRubberBandPointer(event, x, y))
    return;
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    if (m_selection.focusVisible()) {
      m_selection.setFocusVisible(false);
      if (m_grid)
        m_grid->notifyDataChanged();
    }
  }
  if (m_places.tryBeginReorderPress(event, x, y))
    return;
  if (event.type == PointerEvent::Type::Button
      && event.button == BTN_LEFT
      && event.pressed
      && m_selection.handleDragPressSelection(x, y, event.serial))
    return;
  if (event.type == PointerEvent::Type::Button
      && event.button == BTN_LEFT
      && event.pressed
      && m_selection.tryBeginRubberBand(x, y))
    return;
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_dispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_dispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_dispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    if (event.button == BTN_LEFT || event.button == BTN_RIGHT || event.button == BTN_MIDDLE) {
      if (event.pressed && event.button == BTN_LEFT)
        m_selection.recordPointerClickTime(event.time);
      m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      // The virtual canvas only covers populated rows. The remainder of the
      // grid viewport still belongs to the directory background.
      if (event.button == BTN_RIGHT && event.pressed && m_grid && !m_menus.isContextMenuOpen()) {
        float gridX = 0.0F;
        float gridY = 0.0F;
        Node::absolutePosition(m_grid, gridX, gridY);
        if (x >= gridX && x < gridX + m_grid->width() && y >= gridY && y < gridY + m_grid->height())
          m_menus.openEmptyContextMenu(x, y);
      }
    }
    break;
  case PointerEvent::Type::Axis:
    m_dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
}

void FileManager::openLocationContextMenu(float x, float y, bool highlightOnOpen) {
  using namespace filemanager::menu_id;
  if (m_locationInput == nullptr)
    return;
  const bool selected = m_locationInput->hasTextSelection();
  const bool hasClipboardText = m_transfer.selectionOffersText();
  std::vector<ContextMenuControlEntry> entries{
      {.id = kLocationCut, .label = "Cut", .enabled = selected && !m_locationInput->readOnly()},
      {.id = kLocationCopy, .label = "Copy", .enabled = selected},
      {.id = kLocationPaste,
       .label = "Paste",
       .enabled = hasClipboardText && !m_locationInput->readOnly(),
       .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
       .actionGlyph = "U+EA6F"},
      {.id = kLocationSelectAll, .label = "Select All", .enabled = !m_locationInput->value().empty()},
  };
  m_menus.openColumnHeaderMenu(std::move(entries), x, y, highlightOnOpen);
}

void FileManager::openFocusedContextMenu() {
  m_wayland.stopKeyRepeat();
  InputArea* focused = m_dispatcher.focusedArea();
  if (focused != nullptr && isNodeDescendantOf(focused, m_sidebar)) {
    for (const auto& [button, place] : m_places.placeRows()) {
      if (button != nullptr && button->inputArea() == focused) {
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(button, x, y);
        m_menus.setNextPlacesHighlightOnOpen(true);
        m_places.openPlaceMenu(place, x + std::min(24.0F, button->width() * 0.5F), y + button->height() * 0.5F);
        m_menus.setNextPlacesHighlightOnOpen(false);
        return;
      }
    }
  }
  if (m_grid == nullptr)
    return;
  m_selection.setFocusVisible(true);
  m_dispatcher.setFocus(m_grid->focusArea());
  float gx = 0.0F;
  float gy = 0.0F;
  Node::absolutePosition(m_grid, gx, gy);
  const float defaultX = gx + m_grid->width() * 0.5F;
  const float defaultY = gy + m_grid->height() * 0.5F;
  const auto selected = m_grid->selectedIndex();
  if (selected.has_value() && *selected < activeSession().entries().size()) {
    float ax = defaultX;
    float ay = defaultY;
    (void)m_grid->absoluteAnchorForIndex(*selected, ax, ay);
    m_menus.openContextMenu(*selected, ax, ay, true);
  } else {
    m_menus.openEmptyContextMenu(defaultX, defaultY, true);
  }
}

bool FileManager::handleSidebarKeyboardNavigation(std::uint32_t sym, std::uint32_t mods) {
  if (mods != 0U || m_sidebar == nullptr || (!KeySymbol::isUp(sym) && !KeySymbol::isDown(sym)))
    return false;
  InputArea* focused = m_dispatcher.focusedArea();
  if (focused == nullptr || !isNodeDescendantOf(focused, m_sidebar))
    return false;
  std::vector<InputArea*> targets;
  collectFocusableDescendants(m_sidebar, targets);
  if (targets.empty())
    return false;
  const auto it = std::ranges::find(targets, focused);
  if (it == targets.end())
    return false;
  const auto idx = static_cast<std::size_t>(it - targets.begin());
  if (KeySymbol::isUp(sym) && idx > 0)
    m_dispatcher.setFocus(targets[idx - 1]);
  else if (KeySymbol::isDown(sym) && idx + 1 < targets.size())
    m_dispatcher.setFocus(targets[idx + 1]);
  if (m_surface)
    m_surface->requestRedraw();
  return true;
}

void FileManager::handleKeyboard(const KeyboardEvent& event) {
  if (!m_surface || m_wayland.lastKeyboardSurface() != m_surface->wlSurface())
    return;
  handleWindowKeyEvent(event);
}

void FileManager::handleWindowKeyEvent(const KeyboardEvent& event) {
  if (!event.pressed) {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, false, event.preedit, false);
    return;
  }
  if (event.preedit) {
    if (m_dispatcher.focusedArea() != nullptr && m_dispatcher.focusedArea()->textInputClient() != nullptr)
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, true, true, false);
    return;
  }
  if (!event.repeat)
    recordAdminTabUserActivity(activeTab());

  const std::uint32_t mods = event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super);
  InputArea* focused = m_dispatcher.focusedArea();
  const bool inLocationEdit =
      m_locationInput != nullptr && m_editingLocation && focused == m_locationInput->inputArea();
  const bool inInlineRename = m_inlineEditArea != nullptr && focused == m_inlineEditArea;

  if (event.sym == XKB_KEY_Escape && mods == 0U) {
    if (event.repeat)
      return;
    m_quitConfirmation = false;
    m_selection.clearTypeahead();
    if (m_menus.isContextMenuOpen()) {
      m_menus.closeContextMenu();
      return;
    }
    if (inInlineRename) {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
      return;
    }
    if (m_renamePath) {
      cancelRename();
      return;
    }
    if (m_inlineEditArea) {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
      return;
    }
    if (m_editingLocation) {
      setLocationEditMode(false);
      return;
    }
    if (m_transfer.dragging() && !m_transfer.dragDropped()) {
      m_transfer.cancelDrag();
      return;
    }
    if (hasActiveGesture()) {
      m_selection.cancelRubberBand();
      if (m_listHeader.gestureActive()) {
        m_dispatcher.cancelPointerCapture();
        m_listHeader.cancelGesture();
      }
      if (m_tabBar.gestureActive()) {
        m_dispatcher.cancelPointerCapture();
        m_tabBar.cancelGesture();
      }
      m_resizingSidebar = m_resizingStatus = false;
      m_previewResize.reset();
      m_places.cancelGesture();
      m_transfers.cancelLocalDragState();
      return;
    }
    if (activeSession().searchOpen()) {
      if (activeSession().search().status() == filemanager::FileSearchSession::Status::Searching)
        activeSession().stopSearch();
      else
        closeSearch();
      return;
    }
    if (activeTab().hasSelection())
      changeSelection(SelectionChange::None);
    m_clipboard.cancelCutSelection();
    return;
  }

  if (hasActiveGesture(false))
    return;

  if (mods == KeyMod::Ctrl
      && (event.sym == XKB_KEY_Tab || event.sym == XKB_KEY_Page_Down || event.sym == XKB_KEY_KP_Page_Down)) {
    if (m_renamePath)
      cancelRename();
    (void)activateRelativeTab(1);
    return;
  }
  if ((mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_Tab || event.sym == XKB_KEY_ISO_Left_Tab))
      || (mods == KeyMod::Ctrl
          && (event.sym == XKB_KEY_ISO_Left_Tab || event.sym == XKB_KEY_Page_Up || event.sym == XKB_KEY_KP_Page_Up))) {
    if (m_renamePath)
      cancelRename();
    (void)activateRelativeTab(-1);
    return;
  }

  if (!inInlineRename && mods == KeyMod::Ctrl && (event.sym == XKB_KEY_f || event.sym == XKB_KEY_F)) {
    if (!event.repeat)
      focusSearch();
    return;
  }

  if (m_searchBarController.ownsFocusedInput(focused)) {
    if ((mods == 0U && event.sym == XKB_KEY_F5)
        || (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_r || event.sym == XKB_KEY_R))) {
      if (!event.repeat)
        activateContextMenuEntry({.id = filemanager::menu_id::kRefresh});
      return;
    }
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, true, event.preedit, event.repeat);
    return;
  }

  if (inLocationEdit) {
    if (mods == 0U && KeySymbol::isTab(event.sym) && acceptLocationAutocompleteTab())
      return;
    if ((mods == KeyMod::Ctrl && (event.sym == XKB_KEY_l || event.sym == XKB_KEY_L))
        || (mods == KeyMod::Alt && (event.sym == XKB_KEY_d || event.sym == XKB_KEY_D))) {
      if (!event.repeat)
        m_locationInput->selectAll();
    } else if (mods == 0U && KeySymbol::isEnter(event.sym)) {
      if (!event.repeat) {
        navigate(filemanager::expandHomePath(m_locationInput->value(), g_get_home_dir()));
        setLocationEditMode(false);
      }
    } else if (
        (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_v || event.sym == XKB_KEY_V))
        || (mods == KeyMod::Shift && event.sym == XKB_KEY_Insert)
    ) {
      if (!event.repeat)
        pasteLocationFromClipboard();
    } else if ((mods == 0U && event.sym == XKB_KEY_Menu) || (mods == KeyMod::Shift && event.sym == XKB_KEY_F10)) {
      if (!event.repeat) {
        m_wayland.stopKeyRepeat();
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(m_locationInput, x, y);
        openLocationContextMenu(x + 12.0F, y + m_locationInput->height(), true);
      }
    } else {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
    }
    return;
  }

  if (inInlineRename) {
    if (KeySymbol::isTab(event.sym) && (mods == 0U || mods == KeyMod::Shift))
      return;
    if (event.sym == XKB_KEY_F2 || event.sym == XKB_KEY_Menu || event.sym == XKB_KEY_F10)
      return;
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
    return;
  }

  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_t || event.sym == XKB_KEY_T)) {
    if (!event.repeat) {
      if (m_renamePath)
        cancelRename();
      openNewTab();
    }
    return;
  }
  if (mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_t || event.sym == XKB_KEY_T)) {
    if (!event.repeat) {
      if (m_renamePath)
        cancelRename();
      (void)reopenClosedTab();
    }
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_W)) {
    if (!event.repeat) {
      if (m_renamePath)
        cancelRename();
      (void)closeTab(activeTab().id);
    }
    return;
  }
  if (mods == KeyMod::Alt && event.sym >= XKB_KEY_1 && event.sym <= XKB_KEY_9) {
    if (!event.repeat && !m_tabs.tabs().empty()) {
      if (m_renamePath)
        cancelRename();
      const auto index = static_cast<std::size_t>(event.sym - XKB_KEY_1);
      if (event.sym == XKB_KEY_9)
        (void)activateTab(m_tabs.tabs().back()->id);
      else if (index < m_tabs.tabs().size())
        (void)activateTab(m_tabs.tabs()[index]->id);
    }
    return;
  }

  if (mods == 0U && event.sym == XKB_KEY_F8 && !m_menus.isContextMenuOpen()) {
    if (!event.repeat)
      togglePreviewPane();
    return;
  }

  if (m_places.hasActiveDeviceOperations()
      && (((event.sym == XKB_KEY_Delete || event.sym == XKB_KEY_KP_Delete) && (mods == 0U || mods == KeyMod::Shift))
          || (event.sym == XKB_KEY_F2 && mods == 0U)
          || (mods == KeyMod::Ctrl
              && (event.sym == XKB_KEY_v || event.sym == XKB_KEY_V || event.sym == XKB_KEY_n || event.sym == XKB_KEY_N))
          || (mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)))) {
    if (!event.repeat)
      setStatus("Wait for the device operation to finish");
    return;
  }

  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_q || event.sym == XKB_KEY_Q)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = filemanager::menu_id::kQuit});
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = filemanager::menu_id::kFileNewWindow});
    return;
  }
  if (!inInlineRename && mods == KeyMod::Ctrl && event.sym == XKB_KEY_comma) {
    if (!event.repeat)
      openPreferences();
    return;
  }
  if ((mods == KeyMod::Ctrl && (event.sym == XKB_KEY_l || event.sym == XKB_KEY_L))
      || (mods == KeyMod::Alt && (event.sym == XKB_KEY_d || event.sym == XKB_KEY_D))) {
    if (!event.repeat)
      setLocationEditMode(true);
    return;
  }
  if (mods == 0U && event.sym == XKB_KEY_F10) {
    if (!event.repeat) {
      m_wayland.stopKeyRepeat();
      (void)m_menus.openApplicationMenuFromKeyboard(0);
    }
    return;
  }
  if (mods == KeyMod::Alt) {
    std::optional<std::size_t> menuIndex;
    switch (event.sym) {
    case XKB_KEY_f:
    case XKB_KEY_F:
      menuIndex = 0;
      break;
    case XKB_KEY_e:
    case XKB_KEY_E:
      menuIndex = 1;
      break;
    case XKB_KEY_v:
    case XKB_KEY_V:
      menuIndex = 2;
      break;
    case XKB_KEY_t:
    case XKB_KEY_T:
      menuIndex = filemanager::menu_id::kToolsMenuIndex;
      break;
    case XKB_KEY_h:
    case XKB_KEY_H:
      menuIndex = filemanager::menu_id::kHelpMenuIndex;
      break;
    default:
      break;
    }
    if (menuIndex.has_value()) {
      if (!event.repeat) {
        m_wayland.stopKeyRepeat();
        (void)m_menus.openApplicationMenuFromKeyboard(*menuIndex);
      }
      return;
    }
  }
  if ((mods == 0U && event.sym == XKB_KEY_Menu) || (mods == KeyMod::Shift && event.sym == XKB_KEY_F10)) {
    if (!event.repeat)
      openFocusedContextMenu();
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_a || event.sym == XKB_KEY_A)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = kSelectAll});
    return;
  }
  if (mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_a || event.sym == XKB_KEY_A)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = kDeselectAll});
    return;
  }
  if (mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_i || event.sym == XKB_KEY_I)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = kInvertSelection});
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_z || event.sym == XKB_KEY_Z)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = kUndo});
    return;
  }
  if ((mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_z || event.sym == XKB_KEY_Z))
      || (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_y || event.sym == XKB_KEY_Y))) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = kRedo});
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_c || event.sym == XKB_KEY_C)) {
    if (!event.repeat)
      copyToDesktopClipboard(false);
    return;
  }
  if (mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_c || event.sym == XKB_KEY_C)) {
    if (!event.repeat) {
      const auto paths = selectedPaths();
      if (paths.size() == 1)
        writeDesktopClipboard(paths.front(), "text/plain;charset=utf-8");
      else if (paths.empty())
        writeDesktopClipboard(activeSession().path().string(), "text/plain;charset=utf-8");
    }
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_x || event.sym == XKB_KEY_X)) {
    if (!event.repeat)
      copyToDesktopClipboard(true);
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_v || event.sym == XKB_KEY_V)) {
    if (!event.repeat)
      pasteClipboard();
    return;
  }
  if (mods == KeyMod::Alt && KeySymbol::isEnter(event.sym)) {
    if (!event.repeat && activeTab().selectedPath()) {
      openProperties(*activeTab().selectedPath());
    }
    return;
  }
  if (mods == 0U && event.sym == XKB_KEY_F2) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = filemanager::menu_id::kRename});
    return;
  }
  if (mods == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)) {
    if (!event.repeat && !activeSession().browsingTrash())
      activateContextMenuEntry(ContextMenuControlEntry{.id = filemanager::menu_id::kCreateFolder});
    return;
  }
  if ((mods == 0U || mods == KeyMod::Shift) && (event.sym == XKB_KEY_Delete || event.sym == XKB_KEY_KP_Delete)) {
    if (!event.repeat) {
      const bool permanent = mods == KeyMod::Shift || activeTab().administratorBrowsing;
      activateContextMenuEntry(
          ContextMenuControlEntry{
              .id = permanent ? filemanager::menu_id::kDeletePermanently : filemanager::menu_id::kTrash
          }
      );
    }
    return;
  }
  if (mods == 0U && event.sym == XKB_KEY_BackSpace) {
    if (!event.repeat)
      navigateUp();
    return;
  }
  if (mods == KeyMod::Alt && event.sym == XKB_KEY_Left) {
    if (!event.repeat)
      navigateBack();
    return;
  }
  if (mods == KeyMod::Alt && event.sym == XKB_KEY_Right) {
    if (!event.repeat)
      navigateForward();
    return;
  }
  if (mods == KeyMod::Alt && event.sym == XKB_KEY_Up) {
    if (!event.repeat)
      navigateUp();
    return;
  }
  if (mods == KeyMod::Alt && KeySymbol::isHome(event.sym)) {
    if (!event.repeat)
      navigate(std::filesystem::path(g_get_home_dir()));
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_h || event.sym == XKB_KEY_H)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = filemanager::menu_id::kShowHidden});
    return;
  }
  if ((mods == KeyMod::Ctrl && (event.sym == XKB_KEY_plus || event.sym == XKB_KEY_equal || event.sym == XKB_KEY_KP_Add))
      || (mods == (KeyMod::Ctrl | KeyMod::Shift) && event.sym == XKB_KEY_plus)) {
    activateContextMenuEntry({.id = kZoomIn});
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_minus || event.sym == XKB_KEY_KP_Subtract)) {
    activateContextMenuEntry({.id = kZoomOut});
    return;
  }
  if (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_0 || event.sym == XKB_KEY_KP_0)) {
    if (!event.repeat)
      activateContextMenuEntry({.id = kResetZoom});
    return;
  }
  if ((mods == 0U && event.sym == XKB_KEY_F5)
      || (mods == KeyMod::Ctrl && (event.sym == XKB_KEY_r || event.sym == XKB_KEY_R))) {
    if (!event.repeat)
      activateContextMenuEntry({.id = filemanager::menu_id::kRefresh});
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::TabNext, event.sym, event.modifiers)
      || KeybindMatcher::matches(KeybindAction::TabPrevious, event.sym, event.modifiers)) {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
    if (m_grid != nullptr && m_dispatcher.focusedArea() == m_grid->focusArea()) {
      m_selection.setFocusVisible(true);
      m_grid->notifyDataChanged();
      if (m_surface)
        m_surface->requestRedraw();
    }
    return;
  }

  const bool gridIsFocused = focused == nullptr || (m_grid != nullptr && focused == m_grid->focusArea());
  if (!gridIsFocused) {
    if (handleSidebarKeyboardNavigation(event.sym, mods))
      return;
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
    return;
  }

  if (m_grid == nullptr)
    return;

  if (mods == 0U && KeySymbol::isEnter(event.sym)) {
    if (!event.repeat)
      activateSelected();
    return;
  }

  if ((mods & (KeyMod::Alt | KeyMod::Super)) == 0U
      && (KeySymbol::isUp(event.sym)
          || KeySymbol::isDown(event.sym)
          || KeySymbol::isLeft(event.sym)
          || KeySymbol::isRight(event.sym)
          || KeySymbol::isHome(event.sym)
          || KeySymbol::isEnd(event.sym)
          || KeySymbol::isPageUp(event.sym)
          || KeySymbol::isPageDown(event.sym))) {
    (void)m_selection.navigateByKeyboard(event.sym, mods);
    return;
  }

  if ((mods & (KeyMod::Alt | KeyMod::Super)) == 0U && KeySymbol::isSpace(event.sym)) {
    if (!event.repeat) {
      constexpr auto kTypeaheadTimeout = std::chrono::milliseconds(800);
      if (mods == 0U && m_selection.isTypeaheadActive(kTypeaheadTimeout)) {
        (void)m_selection.handleTypeahead(' ');
      } else if (mods != 0U || !openQuickLook()) {
        (void)m_selection.handleSpaceSelection(mods);
      }
    }
    return;
  }

  if ((mods & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0U && event.utf32 > 0x20U && event.utf32 != 0x7FU) {
    if (!event.repeat)
      (void)m_selection.handleTypeahead(event.utf32);
    return;
  }

  m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
}

std::filesystem::path FileManager::defaultTabPath() {
  try {
    return std::filesystem::path(filemanager::startupPath(""));
  } catch (const std::exception&) {
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    return error ? std::filesystem::path("/") : cwd;
  }
}

filemanager::TabId FileManager::openNewTab() {
  return openFolderInNewTab(resolveNewTabPath(), !m_openTabsInBackground);
}

filemanager::TabId FileManager::openFolderInNewTab(const std::filesystem::path& path, bool activate) {
  if (path.empty())
    return filemanager::kInvalidTabId;
  return m_tabs.openTab(path, activeTab().viewState, activate, newTabInsertIndex());
}

filemanager::TabId FileManager::duplicateTab(filemanager::TabId tabId) { return m_tabs.duplicateTab(tabId); }

bool FileManager::activateTab(filemanager::TabId tabId) { return m_tabs.activateTab(tabId); }

bool FileManager::activateRelativeTab(int delta) { return m_tabs.activateRelative(delta); }

bool FileManager::closeTab(filemanager::TabId tabId) {
  const auto* tab = m_tabs.findTab(tabId);
  const bool closesProperties = m_propertiesWindow && tab && m_propertiesWindow->usesAdministratorTab(*tab);
  if (!m_tabs.closeTab(tabId))
    return false;
  if (closesProperties)
    m_propertiesWindow->close();
  return true;
}

bool FileManager::closeOtherTabs(filemanager::TabId keepTabId) { return m_tabs.closeOtherTabs(keepTabId); }

bool FileManager::closeTabsToRight(filemanager::TabId pivotTabId) { return m_tabs.closeTabsToRight(pivotTabId); }

filemanager::TabId FileManager::reopenClosedTab() { return m_tabs.reopenClosedTab(); }

void FileManager::setAlwaysShowTabBar(bool show) {
  m_alwaysShowTabBar = show;
  if (!m_settings.setAlwaysShowTabBar(show))
    setStatus("Unable to save tab preferences");
  m_tabBar.markDirty();
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::setOpenTabsInBackground(bool background) {
  m_openTabsInBackground = background;
  if (!m_settings.setOpenTabsInBackground(background))
    setStatus("Unable to save tab preferences");
}

void FileManager::activatePointerMiddleItem(std::size_t index) {
  const auto& session = activeSession();
  const auto& entries = session.entries();
  if (index >= entries.size())
    return;
  if (m_renamePath || m_places.hasActiveDeviceOperations())
    return;
  const auto& entry = entries[index];
  if (!entry.isDir || session.browsingTrash())
    return;
  openFolderWithMiddleClick(
      session.browsingArchive() ? filemanager::BrowserLocation(session.path(), entry.absPath.native())
                                : filemanager::BrowserLocation(entry.absPath)
  );
}
