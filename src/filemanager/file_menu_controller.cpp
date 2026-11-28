#include "filemanager/file_menu_controller.h"

#include "config/config_service.h"
#include "core/files/display_text.h"
#include "core/files/resource_paths.h"
#include "filemanager/file_archive_actions.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_sort.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/search_dropdown.h"
#include "render/render_context.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/palette.h"
#include "ui/tooltip/tooltip_manager.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <ranges>
#include <utility>

namespace filemanager {

  namespace {

    ContextMenuControlEntry createNewMenuEntry(std::optional<bool> symlinkDirectory) {
      std::vector<ContextMenuControlEntry> children{
          {.id = menu_id::kCreateFolder, .label = "Folder…", .actionGlyph = "folder-plus"},
          {.id = menu_id::kCreateEmptyFile, .label = "Empty File…", .actionGlyph = "file-plus"},
          {.id = menu_id::kCreateTextFile, .label = "Text File…", .actionGlyph = "file-text"}
      };
      struct OfficeType {
        std::int32_t id;
        const char* label;
        const char* mime;
        const char* asset;
      };
      constexpr std::array officeTypes{
          OfficeType{
              menu_id::kCreateDocument, "Document…", "application/vnd.oasis.opendocument.text",
              "templates/filemanager/office/blank.odt"
          },
          OfficeType{
              menu_id::kCreateSpreadsheet, "Spreadsheet…", "application/vnd.oasis.opendocument.spreadsheet",
              "templates/filemanager/office/blank.ods"
          },
          OfficeType{
              menu_id::kCreatePresentation, "Presentation…", "application/vnd.oasis.opendocument.presentation",
              "templates/filemanager/office/blank.odp"
          }
      };
      std::vector<ContextMenuControlEntry> officeEntries;
      for (const auto& type : officeTypes) {
        std::error_code assetError;
        if (!std::filesystem::is_regular_file(paths::assetPath(type.asset), assetError) || assetError)
          continue;
        GList* compatible = g_app_info_get_recommended_for_type(type.mime);
        GAppInfo* selected = nullptr;
        for (GList* item = compatible; item; item = item->next) {
          auto* app = G_APP_INFO(item->data);
          if (!FileLaunchController::applicationIconNames(app).empty()) {
            selected = app;
            break;
          }
        }
        if (selected) {
          ContextMenuControlEntry entry{.id = type.id, .label = type.label};
          entry.gutterIconNames = FileLaunchController::applicationIconNames(selected);
          officeEntries.push_back(std::move(entry));
        }
        for (GList* item = compatible; item; item = item->next)
          g_object_unref(item->data);
        g_list_free(compatible);
      }
      if (!officeEntries.empty()) {
        children.push_back({.separator = true});
        children.insert(
            children.end(), std::make_move_iterator(officeEntries.begin()), std::make_move_iterator(officeEntries.end())
        );
      }
      children.push_back({.separator = true});
      if (symlinkDirectory) {
        children.push_back(
            {.id = menu_id::kSelectionSymlink,
             .label = *symlinkDirectory ? "Symlink to Folder" : "Symlink to File",
             .actionGlyph = "link"}
        );
      }
      children.push_back({.id = menu_id::kManualSymlink, .label = "Symlink…", .actionGlyph = "link"});
      ContextMenuControlEntry createNew{
          .id = menu_id::kCreateNewSubmenu,
          .label = "Create New",
          .hasSubmenu = true,
          .submenuEntries = std::move(children),
          .actionGlyph = "plus",
          .attachSubmenuContent = true,
      };
      return createNew;
    }

  } // namespace

  FileMenuController::FileMenuController(
      WaylandConnection& wayland, RenderContext& renderContext, AnimationManager& menuAnimations,
      InputDispatcher& dispatcher, FileManagerSettings& settings, FileOperationManager& operations, TabManager& tabs,
      FileListHeaderController& listHeader, PlacesSidebarController& places, ChecksumDialogController& checksumDialog,
      HelpDialogController& helpDialog, ProtectedFileManager& protectedFiles, FileLaunchController& launcher,
      FileClipboardController& clipboard, NativeDataTransfer& transfer, Callbacks callbacks
  )
      : m_wayland(wayland), m_renderContext(renderContext), m_menuAnimations(menuAnimations), m_dispatcher(dispatcher),
        m_settings(settings), m_operations(operations), m_tabs(tabs), m_listHeader(listHeader), m_places(places),
        m_checksumDialog(checksumDialog), m_helpDialog(helpDialog), m_protectedFiles(protectedFiles),
        m_launcher(launcher), m_clipboard(clipboard), m_transfer(transfer), m_callbacks(std::move(callbacks)) {}

  FileMenuController::~FileMenuController() {
    m_menuAnimations.cancel(m_menuUnderlineAnimation);
    closeAll();
  }

  void FileMenuController::loadInitialPreference() { m_showApplicationMenuBar = m_settings.showApplicationMenuBar(); }

  void FileMenuController::initializePopups(ConfigService& configService, LayerPopupHostRegistry& pickerHosts) {
    m_launcher.initializePicker(configService, pickerHosts);

    m_contextMenuPopup = std::make_unique<ContextMenuPopup>(m_wayland, m_renderContext);
    m_contextMenuPopup->setHighlightOnOpen(false);
    m_contextMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) { activateEntry(entry); });
    m_contextMenuPopup->setOnDismissed([this] {
      m_contextMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) { activateEntry(entry); });
      m_applicationMenuOpen.reset();
      m_applicationMenuHovered.reset();
      for (std::size_t i = 0; i < m_applicationMenuButtons.size(); ++i) {
        if (m_applicationMenuButtons[i] && m_applicationMenuButtons[i]->hovered()) {
          m_applicationMenuHovered = i;
          break;
        }
      }
      updateApplicationMenuUnderline();
    });
  }

  void FileMenuController::attachMenuBarUi(Flex& appBar, float toolbarHeight) {
    auto menuItems = std::make_unique<Flex>();
    menuItems->setAlign(FlexAlign::Center);
    menuItems->setGap(Style::spaceXs);
    m_appMenuItems = menuItems.get();

    auto fileMenu = std::make_unique<Button>();
    fileMenu->setText("File");
    fileMenu->setVariant(ButtonVariant::Tab);
    fileMenu->setControlHeight(toolbarHeight);
    fileMenu->setPadding(0.0F, Style::spaceSm);
    m_fileMenuButton = fileMenu.get();
    fileMenu->setOnClick([this] {
      float x = 0, y = 0;
      Node::absolutePosition(m_fileMenuButton, x, y);
      const bool fromKeyboard = m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard;
      openApplicationMenu(x, y + m_fileMenuButton->height(), 0, fromKeyboard);
    });
    m_applicationMenuButtons[0] = m_fileMenuButton;
    m_fileMenuButton->setOnEnter([this] {
      m_applicationMenuHovered = 0;
      updateApplicationMenuUnderline();
    });
    m_fileMenuButton->setOnLeave([this] {
      if (!m_applicationMenuOpen && m_applicationMenuHovered == 0) {
        m_applicationMenuHovered.reset();
        updateApplicationMenuUnderline();
      }
    });
    menuItems->addChild(std::move(fileMenu));

    const std::array<const char*, 4> menuNames{"Edit", "View", "Tools", "Help"};
    for (std::size_t i = 0; i < menuNames.size(); ++i) {
      const auto menuIndex = i + 1;
      auto item = std::make_unique<Button>();
      item->setText(menuNames[i]);
      item->setVariant(ButtonVariant::Tab);
      item->setFontSize(Style::fontSizeCaption);
      item->setControlHeight(toolbarHeight);
      item->setPadding(0.0F, Style::spaceSm);
      m_applicationMenuButtons[menuIndex] = item.get();
      if (menuIndex <= menu_id::kHelpMenuIndex) {
        item->setOnClick([this, index = menuIndex] {
          const auto* menu = m_applicationMenuButtons[index];
          float x = 0, y = 0;
          Node::absolutePosition(menu, x, y);
          const bool fromKeyboard = m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard;
          openApplicationMenu(x, y + menu->height(), index, fromKeyboard);
        });
      } else {
        item->setOnClick([] {});
      }
      item->setOnEnter([this, index = menuIndex] {
        m_applicationMenuHovered = index;
        updateApplicationMenuUnderline();
      });
      item->setOnLeave([this, index = menuIndex] {
        if (!m_applicationMenuOpen && m_applicationMenuHovered == index) {
          m_applicationMenuHovered.reset();
          updateApplicationMenuUnderline();
        }
      });
      menuItems->addChild(std::move(item));
    }

    auto underline = std::make_unique<Box>();
    underline->setFill(colorSpecFromRole(ColorRole::Primary));
    underline->setRadius(1.0F);
    underline->setFrameSize(0.0F, 2.0F);
    underline->setOpacity(0.0F);
    underline->setHitTestVisible(false);
    underline->setParticipatesInLayout(false);
    underline->setZIndex(1);
    m_applicationMenuUnderline = underline.get();
    menuItems->addChild(std::move(underline));
    appBar.addChild(std::move(menuItems));

    auto compactMenu = std::make_unique<Button>();
    compactMenu->setVariant(ButtonVariant::Ghost);
    compactMenu->setControlHeight(toolbarHeight);
    compactMenu->setPadding(5.0F);
    compactMenu->setGlyph("menu-2");
    compactMenu->setTooltip("Open application menu");
    m_compactMenuButton = compactMenu.get();
    compactMenu->setOnClick([this] {
      float x = 0, y = 0;
      Node::absolutePosition(m_compactMenuButton, x, y);
      const bool fromKeyboard = m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard;
      openApplicationMenu(x, y + m_compactMenuButton->height(), 0, fromKeyboard);
    });
    appBar.addChild(std::move(compactMenu));

    if (m_showApplicationMenuBar) {
      m_compactMenuButton->setVisible(false);
      m_compactMenuButton->setParticipatesInLayout(false);
    } else {
      m_appMenuItems->setVisible(false);
      m_appMenuItems->setParticipatesInLayout(false);
    }
  }

  void FileMenuController::detachUi() {
    m_menuAnimations.cancel(m_menuUnderlineAnimation);
    m_menuUnderlineAnimation = 0;
    m_appMenuItems = nullptr;
    m_fileMenuButton = nullptr;
    m_applicationMenuButtons.fill(nullptr);
    m_applicationMenuUnderline = nullptr;
    m_compactMenuButton = nullptr;
    m_applicationMenuOpen.reset();
    m_applicationMenuHovered.reset();
  }

  void FileMenuController::closeAll() {
    closeContextMenu();
    m_launcher.closeApplicationPicker();
  }

  void FileMenuController::closeContextMenu() {
    if (m_contextMenuPopup)
      m_contextMenuPopup->close();
  }

  bool FileMenuController::isContextMenuOpen() const { return m_contextMenuPopup && m_contextMenuPopup->isOpen(); }

  bool FileMenuController::hasOpenPopup() const { return isContextMenuOpen() || m_launcher.isApplicationPickerOpen(); }

  bool FileMenuController::handlePointer(const PointerEvent& event) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    const bool modalOpen = m_callbacks.isModalOpen && m_callbacks.isModalOpen();
    if (m_contextMenuPopup
        && m_contextMenuPopup->isOpen()
        && event.type == PointerEvent::Type::Button
        && event.pressed
        && !m_contextMenuPopup->ownsSurface(event.surface)) {
      if (m_applicationMenuOpen
          && !m_applicationMenuCollapsed
          && !modalOpen
          && surface
          && event.surface == surface->wlSurface()
          && event.button == BTN_LEFT) {
        for (std::size_t i = 0; i < m_applicationMenuButtons.size(); ++i) {
          auto* button = m_applicationMenuButtons[i];
          if (i == *m_applicationMenuOpen
              || !button
              || !button->containsScenePoint(static_cast<float>(event.sx), static_cast<float>(event.sy)))
            continue;
          float x = 0.0F;
          float y = 0.0F;
          Node::absolutePosition(button, x, y);
          m_dispatcher.cancelPointerCapture();
          if (m_callbacks.setLastInputSerial)
            m_callbacks.setLastInputSerial(event.serial);
          if (openApplicationMenu(x, y + button->height(), i))
            return true;
          break;
        }
      }
      m_contextMenuPopup->close();
      m_dispatcher.cancelPointerCapture();
      return true;
    }
    return m_contextMenuPopup && m_contextMenuPopup->isOpen() && m_contextMenuPopup->onPointerEvent(event);
  }

  bool FileMenuController::handleKeyboard(const KeyboardEvent& event) {
    if (m_launcher.handleKeyboard(event))
      return true;
    if (m_contextMenuPopup && m_contextMenuPopup->isOpen()) {
      m_contextMenuPopup->onKeyboardEvent(event);
      return true;
    }
    return false;
  }

  void FileMenuController::syncResponsiveMenuBar(int surfaceWidth) {
    if (m_appMenuItems && m_compactMenuButton) {
      const bool collapse = !m_showApplicationMenuBar || surfaceWidth < 640;
      if (collapse != m_applicationMenuCollapsed) {
        m_applicationMenuCollapsed = collapse;
        m_appMenuItems->setVisible(!collapse);
        m_appMenuItems->setParticipatesInLayout(!collapse);
        m_compactMenuButton->setVisible(collapse);
        m_compactMenuButton->setParticipatesInLayout(collapse);
        updateApplicationMenuUnderline();
        if (m_callbacks.onRequestLayout)
          m_callbacks.onRequestLayout();
      }
    }
  }

  void FileMenuController::toggleMenuBarVisibility() {
    m_showApplicationMenuBar = !m_showApplicationMenuBar;
    persistApplicationMenuPreference();
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  void FileMenuController::persistApplicationMenuPreference() {
    if (!m_settings.setShowApplicationMenuBar(m_showApplicationMenuBar) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save menu preference");
  }

  void FileMenuController::updateApplicationMenuUnderline() {
    if (!m_applicationMenuUnderline)
      return;
    std::optional<std::size_t> target;
    if (!m_applicationMenuCollapsed) {
      if (m_applicationMenuOpen)
        target = m_applicationMenuOpen;
      else
        target = m_applicationMenuHovered;
    }

    m_menuAnimations.cancel(m_menuUnderlineAnimation);
    m_menuUnderlineStartX = m_applicationMenuUnderline->x();
    m_menuUnderlineStartWidth = m_applicationMenuUnderline->width();
    const float startOpacity = m_applicationMenuUnderline->opacity();
    float targetX = m_menuUnderlineStartX;
    float targetWidth = m_menuUnderlineStartWidth;
    float targetY = m_applicationMenuUnderline->y();
    if (target && *target < m_applicationMenuButtons.size()) {
      const Button* button = m_applicationMenuButtons[*target];
      if (button && button->label()) {
        targetX = button->x() + button->label()->x();
        targetY = button->y() + button->height() - 2.0F;
        targetWidth = button->label()->width();
      } else {
        target.reset();
      }
    }
    const float targetOpacity = target ? 1.0F : 0.0F;
    m_applicationMenuUnderline->setPosition(m_menuUnderlineStartX, targetY);
    m_menuUnderlineAnimation = m_menuAnimations.animate(
        0.0F, 1.0F, 200.0F, Easing::EaseOutCubic,
        [this, targetX, targetWidth, startOpacity, targetOpacity](float progress) {
          if (!m_applicationMenuUnderline)
            return;
          m_applicationMenuUnderline->setPosition(
              std::lerp(m_menuUnderlineStartX, targetX, progress), m_applicationMenuUnderline->y()
          );
          m_applicationMenuUnderline->setFrameSize(std::lerp(m_menuUnderlineStartWidth, targetWidth, progress), 2.0F);
          m_applicationMenuUnderline->setOpacity(std::lerp(startOpacity, targetOpacity, progress));
        },
        {}, this
    );
  }

  ContextMenuControlEntry FileMenuController::newItemMenu() const {
    const auto& tab = m_tabs.activeTab();
    const auto caps = tab.capabilities();
    const auto selection = m_callbacks.symlinkSelection ? m_callbacks.symlinkSelection() : std::nullopt;
    auto entry = createNewMenuEntry(selection ? std::optional(selection->second) : std::nullopt);
    if (tab.administratorBrowsing) {
      const bool allowBasic =
          caps.canCreateBasic && !tab.session.browsingTrash() && tab.session.currentDirectoryWritable();
      entry.enabled = allowBasic;
      for (auto& child : entry.submenuEntries)
        child.enabled = child.enabled && allowBasic;
    }
    return entry;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::fileMenuEntries() const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    const auto selectedPath = m_callbacks.selectedPath ? m_callbacks.selectedPath() : std::nullopt;
    const auto& selectedPaths = m_callbacks.selectedPaths();
    const auto selectedEntry = selectedPath ? std::ranges::find(session.entries(), *selectedPath, &FileEntry::absPath)
                                            : session.entries().end();
    const bool selectedDirectory = selectedEntry != session.entries().end() && selectedEntry->isDir;
    const bool canOpenSelection = selectedEntry != session.entries().end();
    bool selectedDirectorySaved = false;
    if (selectedDirectory)
      selectedDirectorySaved = m_places.isPersonalPlaceSaved(selectedEntry->absPath);
    std::vector<ContextMenuControlEntry> entries{
        {.id = menu_id::kNewTab,
         .label = "New Tab",
         .shortcut = "Ctrl+T",
         .actionIconNames = {"tab-new-symbolic", "tab-new"},
         .actionGlyph = "plus"},
        {.id = menu_id::kFileNewWindow,
         .label = "New Window",
         .enabled = !session.browsingTrash(),
         .shortcut = "Ctrl+N",
         .actionIconNames = {"window-new-symbolic", "window-new"},
         .actionGlyph = "app-window"},
        newItemMenu(),
        {.separator = true},
        {.id = menu_id::kFileOpen,
         .label = "Open",
         .enabled = canOpenSelection,
         .shortcut = "Enter",
         .actionIconNames = {"folder-open-symbolic", "folder-open"},
         .actionGlyph = "folder-open"},
        {.id = menu_id::kFileOpenInNewTab,
         .label = "Open in New Tab",
         .enabled = !session.browsingTrash() && selectedDirectory,
         .actionIconNames = {"tab-new-symbolic", "tab-new"},
         .actionGlyph = "plus"},
        {.id = menu_id::kFileOpenWith,
         .label = "Open With…",
         .enabled = caps.canLaunchApps
             && !session.browsingTrash()
             && selectedPaths.size() == 1
             && canOpenSelection
             && !selectedEntry->isDir},
        {.id = menu_id::kFileOpenNewWindow,
         .label = "Open in New Window",
         .enabled = !session.browsingTrash() && !session.browsingArchive() && selectedDirectory,
         .actionIconNames = {"window-new-symbolic", "window-new"},
         .actionGlyph = "app-window"},
        {.separator = true},
        {.id = menu_id::kFilePinToPlaces,
         .label = "Add to Places",
         .enabled = caps.canPinToPlaces && !session.browsingTrash() && selectedDirectory && !selectedDirectorySaved,
         .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
         .actionGlyph = "bookmark-plus"},
        {.separator = true},
        {.id = menu_id::kFileProperties,
         .label = "Properties",
         .enabled = caps.canOpenProperties
             && !session.browsingTrash()
             && !selectedPaths.empty()
             && selectedEntry != session.entries().end(),
         .shortcut = "Alt+Enter",
         .actionIconNames = {"document-properties-symbolic", "document-properties"},
         .actionGlyph = "file-info"},
        {.separator = true},
        {.id = menu_id::kReopenClosedTab,
         .label = "Reopen Closed Tab",
         .enabled = m_tabs.canReopenClosedTab(),
         .shortcut = "Ctrl+Shift+T"},
        {.id = menu_id::kCloseTab, .label = "Close Tab", .shortcut = "Ctrl+W"},
        {.id = menu_id::kQuit, .label = "Quit", .shortcut = "Ctrl+Q"},
    };
    if (selectedPath
        && selectedPaths.size() == 1
        && tab.hasActiveAdminSession()
        && m_protectedFiles.hasEdit(tab.id, *selectedPath)) {
      const auto properties = std::ranges::find(entries, menu_id::kFileProperties, &ContextMenuControlEntry::id);
      entries.insert(std::next(properties), {.id = menu_id::kSaveProtectedChanges, .label = "Save Changes Back"});
    }
    for (auto& entry : entries)
      if (entry.id == menu_id::kCreateNewSubmenu)
        entry.enabled =
            (caps.canCreate || caps.canCreateBasic) && !session.browsingTrash() && session.currentDirectoryWritable();
    return entries;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::editMenuEntries() const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    const auto& selectedPaths = m_callbacks.selectedPaths();
    const bool hasSelection = !session.loading() && !selectedPaths.empty();
    const bool oneItem = hasSelection && selectedPaths.size() == 1;
    const bool canSelect = m_callbacks.canChangeSelection && m_callbacks.canChangeSelection();
    const bool canMutate = caps.canMutate && !m_places.hasActiveDeviceOperations() && !m_operations.replaying();
    const bool canPaste = (caps.canMutate) && !m_places.hasActiveDeviceOperations() && !m_operations.replaying();
    const bool canRename = caps.canRename && !m_places.hasActiveDeviceOperations() && !m_operations.replaying();
    const bool modalOpen = m_callbacks.isModalOpen && m_callbacks.isModalOpen();
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    const std::size_t pendingClipboard = m_clipboard.pendingClipboardReads();
    const bool canReplay = caps.canUndo
        && !m_places.hasActiveDeviceOperations()
        && !m_operations.replaying()
        && !modalOpen
        && !renaming
        && !m_transfer.busy()
        && pendingClipboard == 0;
    const bool local = !session.browsingTrash();
    const bool canCreate = caps.canCreate && local && canMutate && session.currentDirectoryWritable();
    std::vector<ContextMenuControlEntry> entries{
        {.id = menu_id::kUndo,
         .label = m_operations.undoLabel(),
         .enabled = canReplay && m_operations.canUndo(),
         .shortcut = "Ctrl+Z",
         .actionGlyph = "arrow-back-up"},
        {.id = menu_id::kRedo,
         .label = m_operations.redoLabel(),
         .enabled = canReplay && m_operations.canRedo(),
         .shortcut = "Ctrl+Shift+Z",
         .actionGlyph = "arrow-forward-up"},
        {.separator = true},
        {.id = menu_id::kCut,
         .label = "Cut",
         .enabled =
             caps.canMutate && caps.canCopyItems && local && hasSelection && !m_places.hasActiveDeviceOperations(),
         .shortcut = "Ctrl+X",
         .actionGlyph = "scissors"},
        {.id = menu_id::kCopy,
         .label = "Copy",
         .enabled = caps.canCopyItems && local && hasSelection && !m_places.hasActiveDeviceOperations(),
         .shortcut = "Ctrl+C",
         .actionGlyph = "copy"},
        {.id = menu_id::kPaste,
         .label = "Paste",
         .enabled = local && canPaste,
         .shortcut = "Ctrl+V",
         .actionGlyph = "U+EA6F"},
        {.id = menu_id::kCopyLocation,
         .label = "Copy Location",
         .enabled = caps.canCopyItems && local && oneItem,
         .actionGlyph = "link"},
        {.separator = true},
        {.id = menu_id::kSelectAll,
         .label = "Select All",
         .enabled = canSelect && selectedPaths.size() < session.entries().size(),
         .shortcut = "Ctrl+A"},
        {.id = menu_id::kDeselectAll, .label = "Deselect All", .enabled = canSelect && hasSelection},
        {.id = menu_id::kInvertSelection,
         .label = "Invert Selection",
         .enabled = canSelect && !session.entries().empty()},
        {.separator = true},
    };
    if (session.browsingTrash()) {
      const bool restoring =
          canMutate && hasSelection && std::ranges::none_of(m_operations.operations(), [](const auto& operation) {
            return operation.kind == FileOperationManager::Kind::EmptyWastebin && !operation.finished;
          });
      entries.push_back({.id = menu_id::kRestore, .label = "Restore", .enabled = restoring});
      entries.push_back({.id = menu_id::kRestoreTo, .label = "Restore to…", .enabled = restoring});
      entries.push_back({.separator = true});
      entries.push_back(
          {.id = menu_id::kPreferences, .label = "Preferences…", .shortcut = "Ctrl+,", .actionGlyph = "settings"}
      );
      return entries;
    }
    entries.insert(
        entries.end(),
        {
            {.id = menu_id::kDuplicate,
             .label = "Duplicate",
             .enabled = canCreate && hasSelection,
             .actionGlyph = "copy"},
            {.id = menu_id::kRename,
             .label = "Rename",
             .enabled = canRename && hasSelection,
             .shortcut = "F2",
             .actionGlyph = "pencil"},
            {.id = menu_id::kTrash,
             .label = "Move to Wastebin",
             .enabled = caps.canTrash && canMutate && hasSelection,
             .shortcut = "Delete",
             .actionGlyph = "trash"},
            {.id = menu_id::kDeletePermanently,
             .label = "Delete Permanently",
             .enabled = canMutate && hasSelection,
             .shortcut = "Shift+Delete"},
            {.separator = true},
        }
    );
    if (const auto selection =
            (canCreate && m_callbacks.symlinkSelection) ? m_callbacks.symlinkSelection() : std::nullopt) {
      entries.push_back(
          {.id = menu_id::kSelectionSymlink,
           .label = selection->second ? "Symlink to Folder" : "Symlink to File",
           .actionGlyph = "link"}
      );
    }
    entries.push_back(
        {.id = menu_id::kManualSymlink, .label = "Symlink…", .enabled = canCreate, .actionGlyph = "link"}
    );
    entries.push_back({.separator = true});
    entries.push_back(
        {.id = menu_id::kPreferences, .label = "Preferences…", .shortcut = "Ctrl+,", .actionGlyph = "settings"}
    );
    return entries;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::viewMenuEntries() const {
    const auto& session = m_tabs.activeTab().session;
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    const bool gridMode = m_listHeader.gridMode();
    const auto previewMode = m_settings.previewMode();
    const auto thumbnailPolicy = m_settings.thumbnailPolicy();
    std::vector<ContextMenuControlEntry> entries{
        {.id = menu_id::kListView,
         .label = "List View",
         .radio = true,
         .toggleState = gridMode ? 0 : 1,
         .actionGlyph = "layout-list"},
        {.id = menu_id::kGridView,
         .label = "Grid View",
         .radio = true,
         .toggleState = gridMode ? 1 : 0,
         .actionGlyph = "layout-grid"},
    };
    if (!gridMode) {
      entries.push_back(
          {.id = menu_id::kShowColumns,
           .label = "Show Columns",
           .hasSubmenu = true,
           .submenuEntries = columnVisibilityEntries(),
           .actionGlyph = "columns-3"}
      );
    }
    entries.insert(
        entries.end(),
        {
            {.separator = true},
            filemanager::fileSortByMenuEntry(
                m_listHeader.sortField(), m_listHeader.sortOrder(), session.foldersFirst(), session.hiddenLast(),
                m_listHeader.searchView()
            ),
            {.id = menu_id::kShowHidden,
             .label = "Show Hidden Files",
             .checkmark = true,
             .toggleState = session.displayedHiddenItems() ? 1 : 0,
             .shortcut = "Ctrl+H"},
            {.separator = true},
            {.id = menu_id::kPreviewMode,
             .label = "Preview Pane",
             .enabled = !session.browsingArchive(),
             .hasSubmenu = true,
             .shortcut = "F8",
             .submenuEntries =
                 {
                     {.id = menu_id::kPreviewFull,
                      .label = "Full Preview",
                      .radio = true,
                      .toggleState = previewMode == PreviewMode::Full ? 1 : 0},
                     {.id = menu_id::kPreviewInformation,
                      .label = "Information Only",
                      .radio = true,
                      .toggleState = previewMode == PreviewMode::Information ? 1 : 0},
                     {.id = menu_id::kPreviewHidden,
                      .label = "Hidden",
                      .radio = true,
                      .toggleState = previewMode == PreviewMode::Hidden ? 1 : 0},
                 }},
            {.id = menu_id::kThumbnailSettings,
             .label = "Thumbnails",
             .hasSubmenu = true,
             .submenuEntries =
                 {
                     {.id = menu_id::kThumbnailsNever,
                      .label = "Never",
                      .radio = true,
                      .toggleState = thumbnailPolicy == ThumbnailPolicy::Never ? 1 : 0},
                     {.id = menu_id::kThumbnailsLocal,
                      .label = "Local Locations",
                      .radio = true,
                      .toggleState = thumbnailPolicy == ThumbnailPolicy::Local ? 1 : 0},
                     {.id = menu_id::kThumbnailsAll,
                      .label = "All Locations",
                      .radio = true,
                      .toggleState = thumbnailPolicy == ThumbnailPolicy::All ? 1 : 0},
                     {.separator = true},
                     {.id = menu_id::kInspectThumbnailCache, .label = "Inspect Cache"},
                     {.id = menu_id::kClearThumbnailCache, .label = "Clear Files Thumbnails…"},
                 }},
            {.separator = true},
            {.id = menu_id::kZoomIn,
             .label = "Zoom In",
             .enabled = !renaming && m_listHeader.viewScale() < filemanager::kFileViewMaxScale,
             .shortcut = "Ctrl++",
             .actionGlyph = "zoom-in"},
            {.id = menu_id::kZoomOut,
             .label = "Zoom Out",
             .enabled = !renaming && m_listHeader.viewScale() > filemanager::kFileViewMinScale,
             .shortcut = "Ctrl+-",
             .actionGlyph = "zoom-out"},
            {.id = menu_id::kResetZoom,
             .label = "Reset Zoom",
             .enabled = !renaming && m_listHeader.viewScale() != filemanager::kFileViewDefaultScale,
             .shortcut = "Ctrl+0",
             .actionGlyph = "zoom-reset"},
            {.separator = true},
            {.id = menu_id::kRefresh, .label = "Refresh", .shortcut = "Ctrl+R", .actionGlyph = "refresh"},
            {.id = menu_id::kAlwaysShowTabBar,
             .label = "Always Show Tab Bar",
             .checkmark = true,
             .toggleState = (m_callbacks.alwaysShowTabBar && m_callbacks.alwaysShowTabBar()) ? 1 : 0},
            {.id = menu_id::kOpenTabsInBackground,
             .label = "Open New Tabs in Background",
             .checkmark = true,
             .toggleState = (m_callbacks.openTabsInBackground && m_callbacks.openTabsInBackground()) ? 1 : 0},
            {.id = menu_id::kShowMenuBar,
             .label = "Show Menu Bar",
             .checkmark = true,
             .toggleState = m_showApplicationMenuBar ? 1 : 0},
        }
    );
    return entries;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::archiveMenuEntries() const {
    using namespace menu_id;
    if (m_operations.replaying() || m_places.hasActiveDeviceOperations())
      return {};
    if (m_tabs.activeTab().session.browsingArchive()) {
      return {
          {.id = kExtractSelected,
           .label = "Extract Selected…",
           .enabled = m_tabs.activeTab().session.archiveCatalogue() && !m_callbacks.selectedPaths().empty(),
           .actionGlyph = "file-zip"}
      };
    }
    const auto selection = archiveSelection(
        m_tabs.activeTab(),
        m_callbacks.selectedPathsOrdered ? m_callbacks.selectedPathsOrdered() : std::vector<std::string>{}
    );
    std::vector<ContextMenuControlEntry> entries;
    if (selection.canCompress) {
      const auto quick = [&](int id, std::string_view suffix) {
        return ContextMenuControlEntry{
            .id = id,
            .label = "Compress to \"" + sanitizeFilenameForDisplay(selection.baseName) + std::string(suffix) + "\"",
            .enabled = selection.canWriteHere
        };
      };
      std::vector<ContextMenuControlEntry> children;
      if (selection.canTarGzip)
        children.push_back(quick(kCompressTarGzip, ".tar.gz"));
      if (selection.canZip)
        children.push_back(quick(kCompressZip, ".zip"));
      children.push_back({.id = kCompressTo, .label = "Compress To…"});
      entries.push_back(
          {.id = kArchiveCompress,
           .label = "Compress",
           .hasSubmenu = true,
           .submenuEntries = std::move(children),
           .actionGlyph = "file-zip",
           .attachSubmenuContent = true}
      );
    }
    if (selection.canExtract)
      entries.push_back(
          {.id = kArchiveExtract,
           .label = "Extract",
           .hasSubmenu = true,
           .submenuEntries =
               {{.id = kExtractHere, .label = "Extract Here", .enabled = selection.canWriteHere},
                {.id = kExtractFolder, .label = "Extract to Folder", .enabled = selection.canWriteHere},
                {.id = kExtractTo, .label = "Extract To…"}},
           .actionGlyph = "folder-open",
           .attachSubmenuContent = true}
      );
    return entries;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::toolsMenuEntries() {
    const auto& tab = m_tabs.activeTab();
    std::vector<ContextMenuControlEntry> rows = m_launcher.externalToolMenuRows();
#ifdef NYTHERA_FILES_ARCHIVES
    auto archiveEntries = archiveMenuEntries();
    rows.push_back({.separator = true});
    rows.insert(
        rows.end(), std::make_move_iterator(archiveEntries.begin()), std::make_move_iterator(archiveEntries.end())
    );
    rows.push_back(
        {.id = menu_id::kAvoidRedundantExtractionFolders,
         .label = "Avoid redundant extraction folders",
         .toggleState = m_settings.avoidRedundantExtractionFolders() ? 1 : 0}
    );
#endif
    const auto checksums = m_callbacks.checksumMenuEntries ? m_callbacks.checksumMenuEntries()
                                                           : ChecksumDialogController::menuEntries(false);
    rows.push_back({.separator = true});
    rows.insert(rows.end(), checksums.begin(), checksums.end());
    rows.push_back({.separator = true});
    rows.push_back({
        .id = menu_id::kActAsAdministrator,
        .label = "Act as Administrator",
        .enabled = !tab.session.browsingArchive()
            && !tab.session.browsingTrash()
            && !tab.hasActiveAdminSession()
            && !tab.adminSession.isAuthenticating(),
        .actionIconNames = {"security-high-symbolic", "dialog-password-symbolic"},
        .actionGlyph = "shield",
    });
    return rows;
  }

  std::vector<ContextMenuControlEntry> FileMenuController::helpMenuEntries() const {
    return HelpDialogController::menuEntries();
  }

  std::optional<std::vector<ContextMenuControlEntry>>
  FileMenuController::applicationMenuEntries(std::size_t menuIndex) {
    std::vector<ContextMenuControlEntry> entries;
    switch (menuIndex) {
    case 0:
      entries = fileMenuEntries();
      break;
    case 1:
      entries = editMenuEntries();
      break;
    case 2:
      entries = viewMenuEntries();
      break;
    case menu_id::kToolsMenuIndex:
      entries = toolsMenuEntries();
      break;
    case menu_id::kHelpMenuIndex:
      entries = helpMenuEntries();
      break;
    default:
      return std::nullopt;
    }
    if (m_applicationMenuCollapsed && menuIndex == 0) {
      entries.insert(
          entries.end(),
          {
              {.separator = true},
              {.id = menu_id::kCompactEditMenu,
               .label = "Edit",
               .hasSubmenu = true,
               .submenuEntries = editMenuEntries(),
               .attachSubmenuContent = true},
              {.id = menu_id::kCompactViewMenu,
               .label = "View",
               .hasSubmenu = true,
               .submenuEntries = viewMenuEntries(),
               .attachSubmenuContent = true},
              {.id = menu_id::kCompactToolsMenu,
               .label = "Tools",
               .hasSubmenu = true,
               .submenuEntries = toolsMenuEntries(),
               .attachSubmenuContent = true},
              {.id = menu_id::kCompactHelpMenu,
               .label = "Help",
               .hasSubmenu = true,
               .submenuEntries = helpMenuEntries(),
               .attachSubmenuContent = true},
              {.separator = true},
              {.id = menu_id::kPreferences, .label = "Preferences…", .shortcut = "Ctrl+,", .actionGlyph = "settings"},
          }
      );
    }
    return entries;
  }

  bool FileMenuController::openApplicationMenuFromKeyboard(std::size_t menuIndex) {
    if (m_applicationMenuCollapsed || !m_showApplicationMenuBar) {
      if (menuIndex != 0 || m_compactMenuButton == nullptr) {
        return false;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(m_compactMenuButton, x, y);
      return openApplicationMenu(x, y + m_compactMenuButton->height(), 0, true);
    }
    if (menuIndex >= m_applicationMenuButtons.size() || m_applicationMenuButtons[menuIndex] == nullptr) {
      return false;
    }
    const auto* button = m_applicationMenuButtons[menuIndex];
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(button, x, y);
    return openApplicationMenu(x, y + button->height(), menuIndex, true);
  }

  bool FileMenuController::openApplicationMenu(float x, float y, std::size_t menuIndex, bool highlightOnOpen) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface)
      return false;
    auto entries = applicationMenuEntries(menuIndex);
    if (!entries)
      return false;
    m_contextMenuTarget.reset();
    const auto output = m_wayland.outputForSurface(surface->wlSurface());
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->setHighlightOnOpen(highlightOnOpen);
    if (!m_applicationMenuCollapsed && m_showApplicationMenuBar && menuIndex <= menu_id::kHelpMenuIndex) {
      m_contextMenuPopup->setOnBoundaryHorizontalMove([this, menuIndex](int delta) {
        constexpr std::size_t kMenuCount = menu_id::kHelpMenuIndex + 1;
        const std::size_t nextIndex = (menuIndex + kMenuCount + (delta >= 0 ? 1U : kMenuCount - 1U)) % kMenuCount;
        openApplicationMenuFromKeyboard(nextIndex);
      });
    } else {
      m_contextMenuPopup->setOnBoundaryHorizontalMove({});
    }
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = std::move(*entries),
            .minMenuWidth = 240.0F,
            .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0
        }
    );
    if (m_contextMenuPopup->isOpen())
      m_applicationMenuOpen = menuIndex;
    updateApplicationMenuUnderline();
    return m_contextMenuPopup->isOpen();
  }

  void FileMenuController::openContextMenu(std::size_t index, float x, float y, bool highlightOnOpen) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (index >= session.entries().size() || !m_contextMenuPopup || !surface)
      return;
    m_contextMenuTarget = session.entries()[index].absPath;
    m_launcher.clearOpenWithApplications();
    const std::string path = session.entries()[index].absPath.string();
    if (!m_callbacks.selectedPaths().contains(path) && m_callbacks.onSelectSingleForContextMenu)
      m_callbacks.onSelectSingleForContextMenu(index);

    if (session.browsingArchive()) {
      const auto& selected = session.entries()[index];
      std::vector<ContextMenuControlEntry> entries{
          {.id = menu_id::kOpen, .label = "Open", .enabled = !selected.isSymlink, .actionGlyph = "folder-open"},
          {.id = menu_id::kOpenArchiveMemberWith,
           .label = "Open With…",
           .enabled = !selected.isDir && !selected.isSymlink},
          {.id = menu_id::kCopy, .label = "Copy", .shortcut = "Ctrl+C", .actionGlyph = "copy"},
          {.id = menu_id::kExtractSelected, .label = "Extract Selected…", .actionGlyph = "archive"},
          {.id = menu_id::kProperties, .label = "Properties", .shortcut = "Alt+Enter", .actionGlyph = "file-info"},
      };
      openMainContextMenu(
          {.entries = std::move(entries), .anchor = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1}},
          highlightOnOpen, true
      );
      return;
    }
    const bool targetIsDir = session.entries()[index].isDir;
    const bool canOpenTarget = true;
    std::vector<ContextMenuControlEntry> entries{
        {.id = menu_id::kOpen,
         .label = "Open",
         .enabled = canOpenTarget,
         .actionIconNames = {"folder-open-symbolic", "folder-open"},
         .actionGlyph = "folder-open"},
        {.id = menu_id::kCopy,
         .label = "Copy",
         .enabled = caps.canCopyItems,
         .shortcut = "Ctrl+C",
         .actionIconNames = {"edit-copy-symbolic", "edit-copy"},
         .actionGlyph = "copy"},
        {.id = menu_id::kCut,
         .label = "Cut",
         .enabled = caps.canMutate && caps.canCopyItems,
         .shortcut = "Ctrl+X",
         .actionIconNames = {"edit-cut-symbolic", "edit-cut"},
         .actionGlyph = "scissors"},
        {.id = menu_id::kPaste,
         .label = "Paste",
         .enabled = caps.canMutate,
         .shortcut = "Ctrl+V",
         .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
         .actionGlyph = "U+EA6F"},
        {.id = menu_id::kRename,
         .label = "Rename",
         .enabled = caps.canRename,
         .shortcut = "F2",
         .actionIconNames = {"document-edit-symbolic", "document-edit"},
         .actionGlyph = "pencil"},
        {.id = menu_id::kTrash,
         .label = "Move to Trash",
         .enabled = caps.canTrash && caps.canMutate,
         .shortcut = "Delete",
         .actionIconNames = {"user-trash-symbolic", "user-trash"},
         .actionGlyph = "trash"},
        {.id = menu_id::kDeletePermanently,
         .label = "Delete permanently",
         .enabled = caps.canMutate,
         .shortcut = "Shift+Delete"}
    };
    if (session.browsingTrash())
      entries = {
          {.id = menu_id::kOpen,
           .label = "Open",
           .actionIconNames = {"folder-open-symbolic", "folder-open"},
           .actionGlyph = "folder-open"},
          {.id = menu_id::kRestore, .label = "Restore"},
          {.id = menu_id::kRestoreTo, .label = "Restore to…"},
      };
    else {
      if (!targetIsDir && caps.canLaunchApps && canOpenTarget) {
        m_launcher.appendOpenWithEntries(session.entries()[index], entries);
#ifdef NYTHERA_FILES_ARCHIVES
        if (!tab.administratorBrowsing && !session.entries()[index].isSymlink) {
          const auto format = archive_backend::formatFromName(session.entries()[index].name);
          if (format && std::ranges::any_of(archive_backend::capabilities(), [&](const auto& cap) {
                return cap.format == *format && cap.extract;
              })) {
            entries.front() = {.id = menu_id::kOpen, .label = "Open", .actionGlyph = "folder-open"};
          }
        }
#endif
      } else if (!targetIsDir) {
        entries.insert(
            entries.begin() + 1,
            ContextMenuControlEntry{
                .id = menu_id::kOpenWithSubmenu, .label = "Open With", .enabled = false, .hasSubmenu = true
            }
        );
      }
      if (session.entries()[index].isSymlink) {
        entries.push_back(
            {.id = kGoToLinkTargetMenuId,
             .label = "Go to Link Target",
             .enabled = resolveLinkTarget(*m_contextMenuTarget).has_value(),
             .actionGlyph = "link"}
        );
      }
      if (targetIsDir)
        entries.push_back(
            {.id = menu_id::kOpenFolderInNewTab,
             .label = "Open in New Tab",
             .actionIconNames = {"tab-new-symbolic", "tab-new"},
             .actionGlyph = "plus"}
        );
      if (targetIsDir)
        entries.push_back(
            {.id = menu_id::kOpenFolderNewWindow,
             .label = "Open in New Window",
             .actionIconNames = {"window-new-symbolic", "window-new"},
             .actionGlyph = "app-window"}
        );
      if (targetIsDir)
        entries.push_back(
            {.id = menu_id::kOpenFolderAsAdministrator,
             .label = "Open as Administrator",
             .enabled = !tab.hasActiveAdminSession() && !tab.adminSession.isAuthenticating(),
             .actionIconNames = {"security-high-symbolic", "dialog-password-symbolic"},
             .actionGlyph = "shield"}
        );
      if (targetIsDir)
        entries.push_back(
            {.id = menu_id::kPinTargetToPlaces,
             .label = "Add to Places",
             .enabled = caps.canPinToPlaces,
             .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
             .actionGlyph = "bookmark-plus"}
        );
      entries.push_back(
          {.id = menu_id::kCopyLocation,
           .label = "Copy Location",
           .enabled = caps.canCopyItems,
           .actionIconNames = {"insert-link-symbolic", "insert-link"},
           .actionGlyph = "link"}
      );
      if (m_callbacks.selectedPaths().size() == 1
          && caps.canGenerateThumbnails
          && std::filesystem::path(path).is_absolute())
        entries.push_back({.id = kQuickLookMenuId, .label = "Quick Look", .shortcut = "Space", .actionGlyph = "eye"});
      if (!m_callbacks.selectedPaths().empty())
        entries.push_back(
            {.id = menu_id::kProperties,
             .label = "Properties",
             .enabled = caps.canOpenProperties,
             .shortcut = "Alt+Enter",
             .actionIconNames = {"document-properties-symbolic", "document-properties"},
             .actionGlyph = "file-info"}
        );
      if (m_callbacks.selectedPaths().size() == 1
          && tab.hasActiveAdminSession()
          && m_protectedFiles.hasEdit(tab.id, path))
        entries.push_back({.id = menu_id::kSaveProtectedChanges, .label = "Save Changes Back"});
    }
    const bool canCreate = caps.canCreate && !session.browsingTrash() && session.currentDirectoryWritable();
    const bool canCreateAny =
        (caps.canCreate || caps.canCreateBasic) && !session.browsingTrash() && session.currentDirectoryWritable();
    if (canCreateAny)
      entries.push_back(newItemMenu());
    filemanager::appendFileMenuEntries(
        entries,
        session.browsingTrash()              ? filemanager::FileMenuContext::Trash
            : session.entries()[index].isDir ? filemanager::FileMenuContext::Folder
                                             : filemanager::FileMenuContext::File,
        m_listHeader.gridMode(), session.displayedHiddenItems(), m_listHeader.sortField(), m_listHeader.sortOrder(),
        session.foldersFirst(), session.hiddenLast(), m_listHeader.searchView()
    );
    if (!session.browsingTrash() && !targetIsDir) {
      const auto checksums = m_callbacks.checksumMenuEntries ? m_callbacks.checksumMenuEntries()
                                                             : ChecksumDialogController::menuEntries(false);
      entries.insert(entries.end(), checksums.begin(), checksums.end());
    }
    if (!session.browsingTrash()) {
      entries.push_back(
          {.id = menu_id::kDuplicate,
           .label = "Duplicate",
           .enabled =
               canCreate && !session.loading() && !m_places.hasActiveDeviceOperations() && !m_operations.replaying(),
           .actionGlyph = "copy"}
      );
    }
    auto archiveEntries = archiveMenuEntries();
    entries.insert(
        entries.end(), std::make_move_iterator(archiveEntries.begin()), std::make_move_iterator(archiveEntries.end())
    );
    filemanager::groupFileMenuEntries(
        entries,
        session.browsingTrash()              ? filemanager::FileMenuContext::Trash
            : session.entries()[index].isDir ? filemanager::FileMenuContext::Folder
                                             : filemanager::FileMenuContext::File
    );
    if (session.search().active())
      entries.insert(
          std::next(entries.begin()),
          {.id = menu_id::kOpenContainingFolder, .label = "Open Containing Folder", .actionGlyph = "folder-open"}
      );
    auto* output = m_wayland.lastPointerOutput();
    if (!output)
      output = m_wayland.outputForSurface(surface->wlSurface());
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->setOnBoundaryHorizontalMove({});
    m_contextMenuPopup->setHighlightOnOpen(highlightOnOpen);
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = entries,
            .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0,
            .fitAvailableHeight = !session.browsingTrash(),
        }
    );
  }

  void FileMenuController::openEmptyContextMenu(float x, float y, bool highlightOnOpen) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface)
      return;
    m_contextMenuTarget.reset();
    if (m_callbacks.onClearSelectionForEmptyMenu)
      m_callbacks.onClearSelectionForEmptyMenu();
    std::vector<ContextMenuControlEntry> entries{
        {.id = menu_id::kPaste,
         .label = "Paste here",
         .enabled = caps.canMutate,
         .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
         .actionGlyph = "U+EA6F"}
    };
    if (session.browsingTrash())
      entries = {{.id = menu_id::kEmptyWastebin, .label = "Empty Wastebin…"}};
    else {
      if ((caps.canCreate || caps.canCreateBasic) && session.currentDirectoryWritable())
        entries.push_back(newItemMenu());
      entries.push_back(
          {.id = menu_id::kPinCurrentToPlaces,
           .label = "Add this folder to Places",
           .enabled = caps.canPinToPlaces,
           .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
           .actionGlyph = "bookmark-plus"}
      );
    }
    filemanager::appendFileMenuEntries(
        entries, session.browsingTrash() ? filemanager::FileMenuContext::Trash : filemanager::FileMenuContext::Empty,
        m_listHeader.gridMode(), session.displayedHiddenItems(), m_listHeader.sortField(), m_listHeader.sortOrder(),
        session.foldersFirst(), session.hiddenLast(), m_listHeader.searchView()
    );
    filemanager::groupFileMenuEntries(
        entries, session.browsingTrash() ? filemanager::FileMenuContext::Trash : filemanager::FileMenuContext::Empty
    );
    wl_output* output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output;
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->setOnBoundaryHorizontalMove({});
    m_contextMenuPopup->setHighlightOnOpen(highlightOnOpen);
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = std::move(entries),
            .anchor = PopupAnchorRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0
        }
    );
  }

  void FileMenuController::showPlacesMenu(
      std::vector<ContextMenuControlEntry> entries, float x, float y, bool highlightOnOpen,
      std::function<void(int)> activate
  ) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface || entries.empty())
      return;
    const float menuHeight = static_cast<float>(entries.size()) * 40.0F + 20.0F;
    const bool effectiveHighlight = highlightOnOpen || std::exchange(m_nextPlacesHighlightOnOpen, false);
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->setOnBoundaryHorizontalMove({});
    m_contextMenuPopup->setHighlightOnOpen(effectiveHighlight);
    m_contextMenuPopup->open(
        ContextMenuPopupRequest{
            .entries = std::move(entries),
            .maxMenuWidth = static_cast<float>(surface->width()),
            .anchor =
                PopupAnchorRect{
                    static_cast<std::int32_t>(
                        std::clamp(x, 0.0F, std::max(0.0F, static_cast<float>(surface->width()) - 220.0F))
                    ),
                    static_cast<std::int32_t>(
                        std::clamp(y, 0.0F, std::max(0.0F, static_cast<float>(surface->height()) - menuHeight))
                    ),
                    1, 1
                },
            .parent =
                PopupSurfaceParent{
                    .xdgSurface = surface->xdgSurface(),
                    .output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output,
                    .wlSurface = surface->wlSurface()
                },
            .inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0
        }
    );

    if (activate && m_contextMenuPopup->isOpen())
      m_contextMenuPopup->setOnActivate([activate = std::move(activate)](const auto& entry) { activate(entry.id); });
  }

  std::vector<ContextMenuControlEntry> FileMenuController::columnVisibilityEntries() const {
    std::vector<ContextMenuControlEntry> columns;
    for (auto column : m_listHeader.columnLayout().order) {
      if (m_listHeader.searchView() && column == FileColumn::Accessed)
        continue;
      const auto index = columnIndex(column);
      columns.push_back(
          {.id = menu_id::kFirstColumnAction + static_cast<int>(index),
           .label = std::string(m_listHeader.columnLabel(column)),
           .enabled = column != FileColumn::Name,
           .checkmark = true,
           .toggleState = m_listHeader.columnLayout().visible[index] ? 1 : 0}
      );
    }
    return columns;
  }

  void FileMenuController::openColumnHeaderMenu(
      std::vector<ContextMenuControlEntry> entries, float x, float y, bool highlightOnOpen
  ) {
    openTabContextMenu(std::move(entries), x, y, highlightOnOpen);
  }

  void FileMenuController::openTabContextMenu(
      std::vector<ContextMenuControlEntry> entries, float x, float y, bool highlightOnOpen
  ) {
    openMainContextMenu(
        {.entries = std::move(entries), .anchor = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1}},
        highlightOnOpen, false
    );
  }

  void FileMenuController::openSearchDropdown(
      std::vector<ContextMenuControlEntry> entries, const Node& trigger, bool highlightOnOpen
  ) {
    ContextMenuPopupRequest request{.entries = std::move(entries)};
    anchorSearchDropdown(request, trigger);
    openMainContextMenu(std::move(request), highlightOnOpen, true);
  }

  void FileMenuController::openMainContextMenu(
      ContextMenuPopupRequest request, bool highlightOnOpen, bool preferSurfaceOutput
  ) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!m_contextMenuPopup || !surface)
      return;
    auto* output =
        preferSurfaceOutput ? m_wayland.outputForSurface(surface->wlSurface()) : m_wayland.lastPointerOutput();
    if (!output)
      output = preferSurfaceOutput ? m_wayland.lastPointerOutput() : m_wayland.outputForSurface(surface->wlSurface());
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_contextMenuPopup->setOnBoundaryHorizontalMove({});
    m_contextMenuPopup->setHighlightOnOpen(highlightOnOpen);
    request.parent = {.xdgSurface = surface->xdgSurface(), .output = output, .wlSurface = surface->wlSurface()};
    request.inputSerial = m_callbacks.lastInputSerial ? m_callbacks.lastInputSerial() : 0;
    m_contextMenuPopup->open(std::move(request));
  }

  void FileMenuController::activateEntry(const ContextMenuControlEntry& entry) {
    if (dispatchSearchAndArchiveAction(entry) || dispatchToolsAndAdminAction(entry))
      return;
    if (dispatchTabAndPreviewAction(entry) || dispatchFileAndLaunchAction(entry))
      return;
    if (dispatchPlacesAction(entry) || dispatchSelectionAndClipboardAction(entry))
      return;
    if (dispatchFileMutationAction(entry))
      return;
    (void)dispatchViewAndSortAction(entry);
  }

  bool FileMenuController::dispatchSearchAndArchiveAction(const ContextMenuControlEntry& entry) {
    if (m_callbacks.activateArchiveAction && m_callbacks.activateArchiveAction(entry.id))
      return true;
    if (m_callbacks.activateSearchScopeEntry && m_callbacks.activateSearchScopeEntry(entry))
      return true;
    if (m_callbacks.activateSearchFilterEntry && m_callbacks.activateSearchFilterEntry(entry))
      return true;
    if (entry.id >= menu_id::kFirstSearchMode
        && entry.id < menu_id::kFirstSearchMode + static_cast<int>(kSearchModeLabels.size())) {
      if (m_callbacks.selectSearchMode)
        m_callbacks.selectSearchMode(static_cast<SearchMode>(entry.id - menu_id::kFirstSearchMode));
      return true;
    }
    return false;
  }

  bool FileMenuController::dispatchToolsAndAdminAction(const ContextMenuControlEntry& entry) {
    auto& tab = m_tabs.activeTab();
    const auto caps = tab.capabilities();
    if (entry.id == menu_id::kSaveProtectedChanges) {
      if (const auto selected = m_callbacks.selectedPath ? m_callbacks.selectedPath() : std::nullopt;
          selected && m_callbacks.selectedPaths().size() == 1)
        m_protectedFiles.saveChanges(tab, *selected);
      return true;
    }
    if (entry.id == menu_id::kActAsAdministrator) {
      if (m_callbacks.enterAdministratorMode)
        m_callbacks.enterAdministratorMode(tab.id, std::nullopt);
      return true;
    }
    if (entry.id == menu_id::kOpenFolderAsAdministrator) {
      if (m_contextMenuTarget && m_callbacks.enterAdministratorMode)
        m_callbacks.enterAdministratorMode(tab.id, *m_contextMenuTarget);
      return true;
    }
    if (entry.id == menu_id::kCalculateChecksum || entry.id == menu_id::kVerifyChecksum) {
      if (entry.enabled && caps.canChecksum && m_callbacks.showChecksumDialog)
        m_callbacks.showChecksumDialog(entry.id == menu_id::kVerifyChecksum);
      return true;
    }
    if (m_helpDialog.handleMenuAction(entry.id))
      return true;
    if (entry.id >= menu_id::kFirstExternalToolApp) {
      if (!caps.canRunExternalTools) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("External tools are disabled in Administrator Mode");
        return true;
      }
      m_launcher.launchExternalTool(static_cast<std::size_t>(entry.id - menu_id::kFirstExternalToolApp));
      return true;
    }
    return false;
  }

  bool FileMenuController::dispatchFileAndLaunchAction(const ContextMenuControlEntry& entry) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    const auto selectedPath = m_callbacks.selectedPath ? m_callbacks.selectedPath() : std::nullopt;
    if (entry.id >= menu_id::kFileNewWindow && entry.id <= menu_id::kPreferences) {
      switch (entry.id) {
      case menu_id::kFileNewWindow:
        if (!session.browsingTrash() && m_callbacks.launchNewWindow)
          m_callbacks.launchNewWindow(session.browsingArchive() ? session.path().parent_path() : session.path());
        break;
      case menu_id::kFileOpen:
        if (m_callbacks.activateSelected)
          m_callbacks.activateSelected();
        break;
      case menu_id::kFileOpenWith:
        if (session.browsingArchive()) {
          if (selectedPath && m_callbacks.openArchiveMember)
            m_callbacks.openArchiveMember(*selectedPath, true);
          break;
        }
        if (!caps.canLaunchApps) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus("Opening files with external applications is disabled in Administrator Mode");
          return true;
        }
        if (selectedPath) {
          m_contextMenuTarget = *selectedPath;
          m_launcher.openApplicationChooser(*selectedPath);
        }
        break;
      case menu_id::kFileOpenNewWindow:
        if (!session.browsingArchive() && selectedPath && m_callbacks.launchNewWindow)
          m_callbacks.launchNewWindow(*selectedPath);
        break;
      case menu_id::kFilePinToPlaces:
        if (!caps.canPinToPlaces) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus("Pinning locations to Places is disabled in Administrator Mode");
          return true;
        }
        if (selectedPath && m_callbacks.pinPlace)
          m_callbacks.pinPlace(*selectedPath);
        break;
      case menu_id::kFileProperties:
        if (!caps.canOpenProperties) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus("Properties is unavailable in Administrator Mode");
          return true;
        }
        if (selectedPath && !m_callbacks.selectedPaths().empty() && m_callbacks.openProperties)
          m_callbacks.openProperties(*selectedPath);
        break;
      case menu_id::kQuit:
        if (m_callbacks.requestQuit)
          m_callbacks.requestQuit();
        break;
      case menu_id::kShowMenuBar:
        toggleMenuBarVisibility();
        break;
      case menu_id::kPreferences:
        if (m_callbacks.openPreferences)
          m_callbacks.openPreferences();
        break;
      }
      return true;
    }
    if (entry.id >= menu_id::kFirstOpenWithApp && entry.id < menu_id::kFileNewWindow && m_contextMenuTarget) {
      if (!caps.canLaunchApps) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Opening files with external applications is disabled in Administrator Mode");
        return true;
      }
      const auto index = static_cast<std::size_t>(entry.id - menu_id::kFirstOpenWithApp);
      if (!m_launcher.launchOpenWithApp(index, *m_contextMenuTarget) && m_callbacks.onStatus)
        m_callbacks.onStatus("That application is no longer available");
      return true;
    }
    if (entry.id == menu_id::kOpenWithOther) {
      if (session.browsingArchive())
        return true;
      if (!caps.canLaunchApps) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Opening files with external applications is disabled in Administrator Mode");
        return true;
      }
      if (m_contextMenuTarget)
        m_launcher.openApplicationChooser(*m_contextMenuTarget);
      return true;
    }
    if (entry.id == menu_id::kOpen) {
      if (m_contextMenuTarget && m_callbacks.activatePath)
        m_callbacks.activatePath(*m_contextMenuTarget);
      return true;
    }
    if (entry.id == menu_id::kProperties) {
      if (!caps.canOpenProperties) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Properties is unavailable in Administrator Mode");
        return true;
      }
      if (m_contextMenuTarget && !m_callbacks.selectedPaths().empty() && m_callbacks.openProperties)
        m_callbacks.openProperties(*m_contextMenuTarget);
      return true;
    }
    if (entry.id == menu_id::kOpenFolderNewWindow && !session.browsingArchive() && m_contextMenuTarget) {
      if (m_callbacks.launchNewWindow)
        m_callbacks.launchNewWindow(*m_contextMenuTarget);
      return true;
    }
    return false;
  }

  bool FileMenuController::dispatchTabAndPreviewAction(const ContextMenuControlEntry& entry) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const bool openInBackground = m_callbacks.openTabsInBackground && m_callbacks.openTabsInBackground();
    const bool alwaysShowBar = m_callbacks.alwaysShowTabBar && m_callbacks.alwaysShowTabBar();
    const auto selectedPath = m_callbacks.selectedPath ? m_callbacks.selectedPath() : std::nullopt;
    switch (entry.id) {
    case menu_id::kNewTab:
    case menu_id::kTabContextNew:
      if (m_callbacks.openNewTab)
        m_callbacks.openNewTab();
      return true;
    case menu_id::kReopenClosedTab:
      if (m_callbacks.reopenClosedTab)
        m_callbacks.reopenClosedTab();
      return true;
    case menu_id::kCloseTab:
      if (m_callbacks.closeTab)
        m_callbacks.closeTab(tab.id);
      return true;
    case kQuickLookMenuId:
      if (m_callbacks.openQuickLook)
        m_callbacks.openQuickLook();
      return true;
    case menu_id::kPreviewHidden:
      if (m_callbacks.setPreviewMode)
        m_callbacks.setPreviewMode(PreviewMode::Hidden);
      return true;
    case menu_id::kPreviewInformation:
      if (m_callbacks.setPreviewMode)
        m_callbacks.setPreviewMode(PreviewMode::Information);
      return true;
    case menu_id::kPreviewFull:
      if (m_callbacks.setPreviewMode)
        m_callbacks.setPreviewMode(PreviewMode::Full);
      return true;
    case menu_id::kThumbnailsNever:
      if (m_callbacks.setThumbnailPolicy)
        m_callbacks.setThumbnailPolicy(ThumbnailPolicy::Never);
      return true;
    case menu_id::kThumbnailsLocal:
      if (m_callbacks.setThumbnailPolicy)
        m_callbacks.setThumbnailPolicy(ThumbnailPolicy::Local);
      return true;
    case menu_id::kThumbnailsAll:
      if (m_callbacks.setThumbnailPolicy)
        m_callbacks.setThumbnailPolicy(ThumbnailPolicy::All);
      return true;
    case menu_id::kInspectThumbnailCache:
      if (m_callbacks.inspectThumbnailCache)
        m_callbacks.inspectThumbnailCache(false);
      return true;
    case menu_id::kClearThumbnailCache:
      if (m_callbacks.inspectThumbnailCache)
        m_callbacks.inspectThumbnailCache(true);
      return true;
    case kGoToLinkTargetMenuId:
      if (m_contextMenuTarget && m_callbacks.goToLinkTarget)
        m_callbacks.goToLinkTarget(*m_contextMenuTarget);
      return true;
    case menu_id::kOpenContainingFolder:
      if (m_contextMenuTarget
          && session.search().active()
          && std::ranges::find(session.entries(), *m_contextMenuTarget, &FileEntry::absPath) != session.entries().end()
          && m_callbacks.navigate) {
        m_callbacks.navigate(m_contextMenuTarget->parent_path());
      }
      return true;
    case menu_id::kOpenFolderInNewTab:
      if (!session.browsingArchive()
          && m_contextMenuTarget
          && !session.browsingTrash()
          && m_callbacks.openFolderInNewTab)
        m_callbacks.openFolderInNewTab(*m_contextMenuTarget, !openInBackground);
      return true;
    case menu_id::kFileOpenInNewTab:
      if (!session.browsingTrash() && selectedPath) {
        const auto found = std::ranges::find(session.entries(), *selectedPath, &FileEntry::absPath);
        if (found != session.entries().end() && found->isDir) {
          if (session.browsingArchive())
            m_tabs.openTab({session.path(), found->absPath.native()}, tab.viewState, !openInBackground);
          else if (m_callbacks.openFolderInNewTab)
            m_callbacks.openFolderInNewTab(*selectedPath, !openInBackground);
        }
      }
      return true;
    case menu_id::kAlwaysShowTabBar:
      if (m_callbacks.setAlwaysShowTabBar)
        m_callbacks.setAlwaysShowTabBar(!alwaysShowBar);
      return true;
    case menu_id::kOpenTabsInBackground:
      if (m_callbacks.setOpenTabsInBackground)
        m_callbacks.setOpenTabsInBackground(!openInBackground);
      return true;
    case menu_id::kTabContextDuplicate:
      if (m_callbacks.duplicateTab && m_callbacks.contextMenuTabId)
        m_callbacks.duplicateTab(m_callbacks.contextMenuTabId());
      return true;
    case menu_id::kTabContextClose:
      if (m_callbacks.closeTab && m_callbacks.contextMenuTabId)
        m_callbacks.closeTab(m_callbacks.contextMenuTabId());
      return true;
    case menu_id::kTabContextCloseOthers:
      if (m_callbacks.closeOtherTabs && m_callbacks.contextMenuTabId)
        m_callbacks.closeOtherTabs(m_callbacks.contextMenuTabId());
      return true;
    case menu_id::kTabContextCloseRight:
      if (m_callbacks.closeTabsToRight && m_callbacks.contextMenuTabId)
        m_callbacks.closeTabsToRight(m_callbacks.contextMenuTabId());
      return true;
    default:
      return false;
    }
  }

  bool FileMenuController::dispatchPlacesAction(const ContextMenuControlEntry& entry) {
    const auto caps = m_tabs.activeTab().capabilities();
    const bool openInBackground = m_callbacks.openTabsInBackground && m_callbacks.openTabsInBackground();
    if (entry.id == menu_id::kPlaceOpenInNewTab && m_places.placeMenuTarget()) {
      const auto place = *m_places.placeMenuTarget();
      const bool activate = !openInBackground;
      if (place.identity == "personal:trash" && m_callbacks.openFolderInNewTab)
        m_callbacks.openFolderInNewTab(std::filesystem::path("trash:/"), activate);
      else if (place.mounted && !place.path.empty() && m_callbacks.openFolderInNewTab)
        m_callbacks.openFolderInNewTab(place.path, activate);
      return true;
    }
    if (entry.id == menu_id::kPinTargetToPlaces) {
      if (!caps.canPinToPlaces) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Pinning locations to Places is disabled in Administrator Mode");
        return true;
      }
      if (m_contextMenuTarget && m_callbacks.pinPlace)
        m_callbacks.pinPlace(*m_contextMenuTarget);
      return true;
    }
    if (entry.id == menu_id::kPinCurrentToPlaces) {
      if (!caps.canPinToPlaces) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Pinning locations to Places is disabled in Administrator Mode");
        return true;
      }
      if (m_callbacks.pinPlace)
        m_callbacks.pinPlace(m_tabs.activeTab().session.path());
      return true;
    }
    if (entry.id == menu_id::kPlaceToggleShowHidden) {
      m_places.toggleShowHiddenMounts();
      return true;
    }
    if ((entry.id == menu_id::kPlaceHide || entry.id == menu_id::kPlaceUnhide) && m_places.placeMenuTarget()) {
      m_places.setPlaceHidden(*m_places.placeMenuTarget(), entry.id == menu_id::kPlaceHide);
      return true;
    }
    if ((entry.id == menu_id::kPlaceProperties || entry.id == menu_id::kPlaceOpenNewWindow)
        && m_places.placeMenuTarget()) {
      const auto place = *m_places.placeMenuTarget();
      if (entry.id == menu_id::kPlaceOpenNewWindow) {
        if (place.mounted && !place.path.empty() && m_callbacks.launchNewWindow)
          m_callbacks.launchNewWindow(place.path);
      } else if (!caps.canOpenProperties) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Properties is unavailable in Administrator Mode");
      } else if (place.mounted && !place.path.empty() && m_callbacks.openProperties) {
        m_callbacks.openProperties(place.path);
      } else if (m_callbacks.openVolumeProperties) {
        m_callbacks.openVolumeProperties(place);
      }
      return true;
    }
    if (entry.id >= menu_id::kPlaceOpen && entry.id <= menu_id::kPlaceEject && m_places.placeMenuTarget()) {
      auto place = *m_places.placeMenuTarget();
      if (entry.id == menu_id::kPlaceOpen) {
        if (place.identity == "personal:trash" && m_callbacks.navigateTrash)
          m_callbacks.navigateTrash();
        else if (place.mounted && !place.path.empty() && m_callbacks.navigate)
          m_callbacks.navigate(place.path);
      } else if (entry.id == menu_id::kPlaceRemove) {
        m_places.removeSavedPlace(place.identity);
      } else if (entry.id == menu_id::kPlacePin) {
        if (!caps.canPinToPlaces) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus("Pinning locations to Places is disabled in Administrator Mode");
          return true;
        }
        if (m_callbacks.pinPlace)
          m_callbacks.pinPlace(place.path);
      } else {
        m_places.operatePlace(
            place,
            entry.id == menu_id::kPlaceMount         ? PlaceOperation::Mount
                : entry.id == menu_id::kPlaceUnmount ? PlaceOperation::Unmount
                                                     : PlaceOperation::Eject
        );
      }
      return true;
    }
    return false;
  }

  bool FileMenuController::dispatchSelectionAndClipboardAction(const ContextMenuControlEntry& entry) {
    if (entry.id >= menu_id::kLocationCut && entry.id <= menu_id::kLocationSelectAll) {
      if (m_callbacks.handleLocationInputAction)
        m_callbacks.handleLocationInputAction(entry.id);
      return true;
    }
    if (entry.id == menu_id::kCopyLocation) {
      if (m_contextMenuTarget) {
        m_clipboard.writeDesktopClipboard(m_contextMenuTarget->string(), "text/plain;charset=utf-8");
      } else if (m_callbacks.selectedPathsOrdered) {
        const auto paths = m_callbacks.selectedPathsOrdered();
        if (paths.size() == 1)
          m_clipboard.writeDesktopClipboard(paths.front(), "text/plain;charset=utf-8");
      }
      return true;
    }
    auto& tab = m_tabs.activeTab();
    switch (entry.id) {
    case menu_id::kSelectAll:
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      if (m_callbacks.changeSelection)
        m_callbacks.changeSelection(SelectionChange::All);
      return true;
    case menu_id::kDeselectAll:
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      if (m_callbacks.changeSelection)
        m_callbacks.changeSelection(SelectionChange::None);
      return true;
    case menu_id::kInvertSelection:
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      if (m_callbacks.changeSelection)
        m_callbacks.changeSelection(SelectionChange::Invert);
      return true;
    case menu_id::kCopy:
      if (m_places.hasActiveDeviceOperations()) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Wait for the device operation to finish");
        return true;
      }
      if (m_callbacks.copyToDesktopClipboard)
        m_callbacks.copyToDesktopClipboard(false);
      return true;
    case menu_id::kCut:
      if (m_places.hasActiveDeviceOperations()) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Wait for the device operation to finish");
        return true;
      }
      if (m_callbacks.copyToDesktopClipboard)
        m_callbacks.copyToDesktopClipboard(true);
      return true;
    case menu_id::kPaste:
      if (m_places.hasActiveDeviceOperations()) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Wait for the device operation to finish");
        return true;
      }
      if (m_operations.replaying()) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Wait for Undo or Redo to finish");
        return true;
      }
      m_clipboard.pasteClipboard();
      return true;
    default:
      return false;
    }
  }

  bool FileMenuController::dispatchFileMutationAction(const ContextMenuControlEntry& entry) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    const bool createsItem = entry.id >= menu_id::kCreateFolder && entry.id <= menu_id::kSelectionSymlink;
    if (m_places.hasActiveDeviceOperations()
        && ((entry.id >= menu_id::kCopy && entry.id <= menu_id::kDeletePermanently)
            || createsItem
            || entry.id == menu_id::kDuplicate
            || entry.id == menu_id::kUndo
            || entry.id == menu_id::kRedo)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("Wait for the device operation to finish");
      return true;
    }
    if (m_operations.replaying()
        && ((entry.id >= menu_id::kPaste && entry.id <= menu_id::kDeletePermanently)
            || (entry.id >= menu_id::kRestore && entry.id <= menu_id::kEmptyWastebin)
            || createsItem
            || entry.id == menu_id::kDuplicate)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("Wait for Undo or Redo to finish");
      return true;
    }
    switch (entry.id) {
    case menu_id::kDuplicate:
      if (!caps.canCreate || !caps.canMutate) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("This action is unavailable in Administrator Mode");
        return true;
      }
      if (m_callbacks.duplicateSelection)
        m_callbacks.duplicateSelection();
      return true;
    case menu_id::kSelectionSymlink:
      if (!caps.canCreate || !caps.canMutate) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("This action is unavailable in Administrator Mode");
        return true;
      }
      if (m_callbacks.createSelectionSymlink)
        m_callbacks.createSelectionSymlink();
      return true;
    case menu_id::kManualSymlink:
      if (!caps.canCreate || !caps.canMutate) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("This action is unavailable in Administrator Mode");
        return true;
      }
      if (m_callbacks.showSymlinkDialog)
        m_callbacks.showSymlinkDialog();
      return true;
    case menu_id::kUndo:
    case menu_id::kRedo: {
      if (!caps.canUndo) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Undo and Redo are unavailable in Administrator Mode");
        return true;
      }
      const bool modalOpen = m_callbacks.isModalOpen && m_callbacks.isModalOpen();
      const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
      if (!modalOpen && !renaming && !m_transfer.busy() && m_clipboard.pendingClipboardReads() == 0) {
        if (entry.id == menu_id::kUndo && m_operations.canUndo())
          m_operations.undo();
        else if (entry.id == menu_id::kRedo && m_operations.canRedo())
          m_operations.redo();
      }
      return true;
    }
    case menu_id::kRestore:
    case menu_id::kRestoreTo:
      if (m_callbacks.restoreWastebinSelection)
        m_callbacks.restoreWastebinSelection(entry.id == menu_id::kRestoreTo);
      return true;
    case menu_id::kEmptyWastebin:
      if (m_callbacks.confirmEmptyWastebin)
        m_callbacks.confirmEmptyWastebin();
      return true;
    case menu_id::kRename:
      if (m_callbacks.beginRename)
        m_callbacks.beginRename();
      return true;
    case menu_id::kTrash:
      if (!session.browsingTrash()) {
        if (!caps.canMutate || !caps.canTrash) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus("This action is unavailable in Administrator Mode");
          return true;
        }
        if (m_callbacks.selectedPathsOrdered) {
          auto targets = m_callbacks.selectedPathsOrdered();
          if (!targets.empty())
            m_operations.trash(std::move(targets));
        }
      }
      return true;
    case menu_id::kDeletePermanently:
      if (!session.browsingTrash() && m_callbacks.confirmPermanentDelete)
        m_callbacks.confirmPermanentDelete();
      return true;
    default:
      break;
    }
    if ((entry.id >= menu_id::kCreateDocument && entry.id <= menu_id::kCreatePresentation)
        || (entry.id >= menu_id::kCreateFolder && entry.id <= menu_id::kCreateTextFile)) {
      if (m_callbacks.createNewItem)
        m_callbacks.createNewItem(entry.id);
      return true;
    }
    return false;
  }

  bool FileMenuController::dispatchViewAndSortAction(const ContextMenuControlEntry& entry) {
    auto& tab = m_tabs.activeTab();
    auto& session = tab.session;
    if (entry.id == menu_id::kRefresh) {
      if (session.browsingArchive())
        session.reloadArchive();
      else if (m_callbacks.enumerate)
        m_callbacks.enumerate(session.path());
      return true;
    }
    if (entry.id == menu_id::kToggleViewMode || entry.id == menu_id::kListView || entry.id == menu_id::kGridView) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      if (m_callbacks.setGridMode)
        m_callbacks.setGridMode(
            entry.id == menu_id::kToggleViewMode ? !m_listHeader.gridMode() : entry.id == menu_id::kGridView
        );
      return true;
    }
    if (entry.id >= menu_id::kZoomIn && entry.id <= menu_id::kResetZoom) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      if (m_callbacks.adjustViewScale)
        m_callbacks.adjustViewScale(
            entry.id == menu_id::kResetZoom
                ? kFileViewDefaultScale
                : m_listHeader.viewScale() + (entry.id == menu_id::kZoomIn ? kFileViewZoomStep : -kFileViewZoomStep)
        );
      return true;
    }
    if (entry.id == menu_id::kFitAllColumns) {
      m_listHeader.requestFit(std::nullopt);
      return true;
    }
    if (entry.id >= menu_id::kFirstFitColumn
        && entry.id < menu_id::kFirstFitColumn + static_cast<int>(kFileColumns.size())) {
      m_listHeader.requestFit(static_cast<FileColumn>(entry.id - menu_id::kFirstFitColumn));
      return true;
    }
    if (entry.id >= menu_id::kFirstColumnAction
        && entry.id < menu_id::kFirstColumnAction + static_cast<int>(kFileColumns.size())) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      const auto column = static_cast<FileColumn>(entry.id - menu_id::kFirstColumnAction);
      if (column != FileColumn::Name && !(m_listHeader.searchView() && column == FileColumn::Accessed)) {
        auto& visible = m_listHeader.columnLayout().visible[columnIndex(column)];
        visible = !visible;
        if (column == FileColumn::Size && !visible) {
          if (session.search().active()) {
            session.updateVisibleSearchFolders({});
          } else {
            for (const auto& tabPtr : m_tabs.tabs())
              tabPtr->session.clearFolderCounts();
          }
        }
        if (!m_listHeader.gridMode())
          m_listHeader.updateColumnHeaders();
        m_listHeader.persistColumnLayout();
      }
      return true;
    }
    if (entry.id >= menu_id::kSortName && entry.id <= menu_id::kSortAccessed) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      const auto field = static_cast<FileDialogSortField>(entry.id - menu_id::kSortName);
      if (field != m_listHeader.sortField() && m_callbacks.setSortField)
        m_callbacks.setSortField(field);
      return true;
    }
    if (entry.id == menu_id::kSortType || (entry.id == menu_id::kSortLocation && m_listHeader.searchView())) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      const auto field = entry.id == menu_id::kSortLocation ? FileDialogSortField::Location : FileDialogSortField::Type;
      if (m_listHeader.sortField() != field && m_callbacks.setSortField)
        m_callbacks.setSortField(field);
      return true;
    }
    if (entry.id == menu_id::kSortAscending || entry.id == menu_id::kSortDescending) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      const auto requested =
          entry.id == menu_id::kSortAscending ? FileDialogSortOrder::Ascending : FileDialogSortOrder::Descending;
      if (m_listHeader.sortOrder() != requested && m_callbacks.toggleSortOrder)
        m_callbacks.toggleSortOrder();
      return true;
    }
    if (entry.id == menu_id::kShowHidden) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      if (session.search().active()) {
        if (m_callbacks.toggleSearchHidden)
          m_callbacks.toggleSearchHidden();
      } else {
        session.setShowHidden(!session.showHidden());
        if (m_callbacks.persistFileViewState)
          m_callbacks.persistFileViewState();
        if (m_callbacks.enumerate)
          m_callbacks.enumerate(session.path());
      }
      return true;
    }
    if (entry.id == menu_id::kFoldersFirst || entry.id == menu_id::kHiddenLast) {
      if (m_callbacks.recordAdminTabUserActivity)
        m_callbacks.recordAdminTabUserActivity(tab);
      if (entry.id == menu_id::kFoldersFirst)
        session.setFoldersFirst(!session.foldersFirst());
      else
        session.setHiddenLast(!session.hiddenLast());
      if (m_callbacks.persistFileViewState)
        m_callbacks.persistFileViewState();
      session.sortEntries(true);
      if (m_callbacks.syncSortOrHiddenEntries)
        m_callbacks.syncSortOrHiddenEntries();
      return true;
    }
    return false;
  }

} // namespace filemanager
