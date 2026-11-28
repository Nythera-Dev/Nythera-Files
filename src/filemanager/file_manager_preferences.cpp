#include "core/deferred_call.h"
#include "filemanager/file_manager.h"
#include "filemanager/path_autocomplete.h"
#include "filemanager/startup_path.h"
#include "render/animation/motion_service.h"
#include "render/animation/surface_haze.h"
#include "system/icon_resolver.h"
#include "theme/fixed_palette.h"
#include "theme/fixed_palette_json.h"
#include "ui/controls/color_picker.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/palette.h"

#include <algorithm>

namespace {
  using filemanager::isExistingDirectory;

  void jumpHistory(
      filemanager::BrowserLocation current, std::vector<filemanager::BrowserLocation>& source,
      std::vector<filemanager::BrowserLocation>& destination, std::size_t steps,
      const std::function<void(filemanager::BrowserLocation)>& navigate
  ) {
    if (steps == 0 || steps > source.size())
      return;
    for (; steps > 0; --steps) {
      destination.push_back(std::move(current));
      current = std::move(source.back());
      source.pop_back();
    }
    navigate(std::move(current));
  }
} // namespace

std::filesystem::path FileManager::resolveStartupLocationPath() const {
  using filemanager::StartupLocation;
  const auto mode = m_settings.startupLocation();
  const auto path = mode == StartupLocation::Last
      ? m_settings.loadLastVisitedLocation().value_or(std::filesystem::path{})
      : mode == StartupLocation::Custom ? m_settings.startupCustomPath()
                                        : std::filesystem::path{};
  return isExistingDirectory(path) ? path : defaultTabPath();
}

std::filesystem::path FileManager::initialWindowLocation() const {
  if (m_startupOptions.explicitPath)
    return *m_startupOptions.explicitPath;
  if (m_settings.restoreTabs()) {
    const auto saved = m_settings.loadLastWindowTabs();
    const auto first = std::ranges::find_if(saved.paths, isExistingDirectory);
    if (first != saved.paths.end())
      return *first;
  }
  return resolveStartupLocationPath();
}

void FileManager::restoreWindowTabs() {
  if (!m_startupOptions.explicitPath && m_settings.restoreTabs()) {
    const auto saved = m_settings.loadLastWindowTabs();
    std::size_t insertion = 0;
    auto selected = m_tabs.activeTabId();
    for (std::size_t i = 0; i < saved.paths.size(); ++i) {
      if (!filemanager::isExistingDirectory(saved.paths[i]))
        continue;
      const auto id = insertion++ == 0
          ? m_tabs.activeTabId()
          : m_tabs.openTab(saved.paths[i], activeTab().viewState, false, std::nullopt, false);
      if (insertion > 1)
        m_tabs.findTab(id)->needsRefresh = true;
      if (i == saved.activeIndex)
        selected = id;
    }
    if (auto* selectedTab = m_tabs.findTab(selected))
      selectedTab->needsRefresh = false;
    (void)m_tabs.activateTab(selected);
  }
}

std::filesystem::path FileManager::resolveNewTabPath() const {
  using filemanager::NewTabLocation;
  switch (m_settings.newTabLocation()) {
  case NewTabLocation::Current:
    if (activeSession().canSearch()
        && !activeTab().administratorBrowsing
        && isExistingDirectory(activeSession().path()))
      return activeSession().path();
    break;
  case NewTabLocation::Custom: {
    const auto custom = m_settings.newTabCustomPath();
    if (isExistingDirectory(custom))
      return custom;
    break;
  }
  case NewTabLocation::Startup:
    return resolveStartupLocationPath();
  case NewTabLocation::Home:
    break;
  }
  return defaultTabPath();
}

std::optional<std::size_t> FileManager::newTabInsertIndex() const {
  if (m_settings.newTabPosition() != filemanager::NewTabPosition::AfterCurrent)
    return std::nullopt;
  const auto& tabs = m_tabs.tabs();
  const auto it = std::ranges::find_if(tabs, [this](const auto& tab) { return tab->id == m_tabs.activeTabId(); });
  if (it == tabs.end())
    return std::nullopt;
  return static_cast<std::size_t>(it - tabs.begin() + 1);
}

void FileManager::openFolderWithMiddleClick(filemanager::BrowserLocation target) {
  using filemanager::MiddleClickFolderAction;
  const auto action = m_settings.middleClickFolder();
  if (action == MiddleClickFolderAction::None)
    return;
  if (action == MiddleClickFolderAction::NewWindow) {
    if (!target.archiveDirectory && !activeTab().administratorBrowsing && !activeTab().hasActiveAdminSession())
      launchNewWindow(target.path);
    return;
  }
  const bool activate = action == MiddleClickFolderAction::ForegroundTab;
  if (target.archiveDirectory)
    (void)m_tabs.openTab(std::move(target), activeTab().viewState, activate, newTabInsertIndex());
  else
    (void)openFolderInNewTab(target.path, activate);
}

void FileManager::startInstanceCoordinator() {
  (void)m_instanceCoordinator.start({}, [this](const std::filesystem::path& path) {
    if (!m_windowOpen || m_quitting || m_settings.externalOpenMode() != filemanager::ExternalOpenMode::NewTab)
      return false;
    if (openFolderInNewTab(path, true) == filemanager::kInvalidTabId)
      return false;
    const std::weak_ptr<bool> alive = m_alive;
    DeferredCall::callLater([this, alive] {
      const auto life = alive.lock();
      if (life && *life && m_windowOpen && m_surface)
        m_wayland.activateSurface(m_surface->wlSurface());
    });
    return true;
  });
}

void FileManager::recordLocationVisit(filemanager::BrowserTab& tab) {
  const auto& session = tab.session;
  const auto& path = session.path();
  if (!m_surface
      || session.browsingTrash()
      || session.browsingRecent()
      || session.browsingArchive()
      || tab.administratorBrowsing
      || tab.hasActiveAdminSession()
      || !isExistingDirectory(path)) {
    m_lastRecordedLocation.reset();
    return;
  }
  const auto location = std::pair{tab.id, path};
  if (m_lastRecordedLocation == location)
    return;
  m_lastRecordedLocation = location;
  (void)m_settings.saveLastVisitedLocation(path);
  if (m_settings.recentActivityEnabled()) {
    (void)m_recentActivity.update([&](auto& store) {
      store.recordDirectoryVisit(path);
      return true;
    });
  }
}

void FileManager::recordFileOpen(const std::filesystem::path& path) {
  if (m_settings.recentActivityEnabled()) {
    (void)m_recentActivity.update([&](auto& store) {
      store.recordFileOpen(path);
      return true;
    });
  }
}

void FileManager::saveWindowSessionState() {
  if (m_surface && m_settings.rememberWindowGeometry()) {
    const auto width = m_surface->floatingWidth();
    const auto height = m_surface->floatingHeight();
    if (width != 0 && height != 0)
      (void)m_settings.saveMainWindowGeometry({width, height, m_surface->isMaximized()});
  }
  filemanager::SavedTabsState saved;
  for (const auto& tab : m_tabs.tabs()) {
    if (tab->administratorBrowsing
        || tab->hasActiveAdminSession()
        || tab->session.browsingArchive()
        || !isExistingDirectory(tab->session.path()))
      continue;
    if (tab->id == m_tabs.activeTabId())
      saved.activeIndex = saved.paths.size();
    saved.paths.push_back(tab->session.path());
  }
  (void)m_settings.saveLastWindowTabs(saved);
}

bool FileManager::handleWindowCloseRequest() {
  using filemanager::DialogChoiceStyle;
  if (!m_windowOpen)
    return false;
  if (m_modalDialog.isOpen())
    return false;
  if (m_tabs.tabs().size() > 1 && m_settings.confirmCloseMultipleTabs()) {
    const auto activeId = m_tabs.activeTabId();
    showDialog(
        "Close window?", "This window has multiple tabs.",
        {{"Cancel", 0, DialogChoiceStyle::Cancel},
         {"Close current tab", 1, DialogChoiceStyle::Normal},
         {"Close entire window", 2, DialogChoiceStyle::Destructive}},
        [this, activeId](int choice) {
          if (choice == 1)
            (void)closeTab(activeId);
          else if (choice == 2 && confirmCloseWithOperations())
            performWindowClose();
        },
        m_surface.get()
    );
    return false;
  }
  return confirmCloseWithOperations();
}

bool FileManager::confirmCloseWithOperations() {
  using filemanager::DialogChoiceStyle;
  if (!m_operations.active() || !m_settings.confirmCloseActiveOperations())
    return true;
  showDialog(
      "File operations are active", "Choose what to do with the running operations.",
      {{"Keep window open", 0, DialogChoiceStyle::Cancel},
       {"Continue in background", 1, DialogChoiceStyle::Normal},
       {"Cancel operations and close", 2, DialogChoiceStyle::Destructive}},
      [this](int choice) {
        if (choice == 0)
          return;
        if (choice == 2) {
          for (const auto& operation : m_operations.operations())
            if (!operation.finished)
              m_operations.cancel(operation.id);
        }
        performWindowClose();
      },
      m_surface.get()
  );
  return false;
}

void FileManager::performWindowClose() {
  if (!m_surface || !m_windowOpen)
    return;
  m_surface->setCloseRequestCallback({});
  ToplevelSurface::handleToplevelClose(m_surface.get(), nullptr);
  syncProgressWindow();
}

void FileManager::applyFilesPalette(const nythera::theme::GeneratedPalette& inherited) {
  const bool inherit = m_settings.inheritShellTheme();
  const bool light = inherit && m_themeService.resolvedShellMode() == "light";
  auto dark = nythera::theme::mapGeneratedPaletteMode(inherited.dark);
  auto lightPalette = nythera::theme::mapGeneratedPaletteMode(inherited.light);
  if (!inherit) {
    const auto* builtin = nythera::theme::findBuiltinPalette("Nythera");
    if (builtin) {
      const auto defaults = nythera::theme::expandBuiltinPalette(*builtin);
      dark = nythera::theme::mapGeneratedPaletteMode(defaults.dark);
      lightPalette = nythera::theme::mapGeneratedPaletteMode(defaults.light);
    }
  }
  if (const auto path = m_settings.themeFile(); !path.empty()) {
    if (const auto file = nythera::theme::parseUiPaletteFile(path)) {
      dark = file->dark;
      lightPalette = file->light;
    }
  }
  const auto overrides = m_settings.themeColorOverrides();
  m_filesDarkPalette = applyColorRoleOverrides(dark, overrides);
  m_filesLightPalette = applyColorRoleOverrides(lightPalette, overrides);
  setResolvedThemeLight(light);
  setPalette(light ? m_filesLightPalette : m_filesDarkPalette);
}

void FileManager::showPreferenceColorPicker(std::string title, Color initial, std::function<void(Color)> apply) {
  if (!m_settingsWindow || !m_settingsWindow->isOpen())
    return;
  const auto color = std::make_shared<Color>(initial);
  m_modalDialog.showDialog(
      std::move(title), {},
      {{"Cancel", 0, filemanager::DialogChoiceStyle::Cancel}, {"Apply", 1, filemanager::DialogChoiceStyle::Normal}},
      [color, apply = std::move(apply)](int result) {
        if (result == 1)
          apply(*color);
      },
      m_settingsWindow->surface(),
      filemanager::ModalDialogHost::CustomContent{
          .buildFields =
              [color](Flex& root, float) {
                auto picker = std::make_unique<ColorPicker>();
                picker->setFillWidth();
                picker->setAlphaEnabled(true);
                picker->setColor(*color);
                picker->setOnColorChanged([color](const Color& next) { *color = next; });
                root.addChild(std::move(picker));
              },
          .useMeasuredMinHeight = true,
          .selectDropdowns = true,
          .rememberSize = false,
          .roundedCorners = true,
          .preferredWidth = 440,
      }
  );
}

void FileManager::watchThemeFile() {
  const auto path = m_settings.themeFile();
  if (path == m_watchedThemeFile)
    return;
  m_watchedThemeFile = path;
  if (m_themeFileWatch)
    m_settingsFileWatcher.unwatch(std::exchange(m_themeFileWatch, 0));
  if (!path.empty()) {
    m_themeFileWatch = m_settingsFileWatcher.watch(
        path,
        [this] {
          m_themeService.apply();
          if (m_settingsWindow)
            m_settingsWindow->updateTheme();
          if (m_surface)
            m_surface->requestLayout();
        },
        FileWatcher::WatchTrigger::WriteCompleted
    );
  }
}

void FileManager::applyThemeAndAppearance() {
  watchThemeFile();
  MotionService::instance().setEnabled(m_settings.effectiveAnimationsEnabled());
  MotionService::instance().setSpeed(m_settings.effectiveAnimationSpeed());
  const bool inherit = m_settings.inheritShellTheme();
  const auto& panel = m_configService.config().shell.panel;
  const bool translucent = m_settings.transparencyMode() != PanelTransparencyMode::Solid;
  const bool animated = !inherit || panel.hazeMode == PanelHazeMode::Animated;
  const bool transparent = !inherit || panel.hazeBackground == PanelHazeBackground::Transparent;
  // Explicit Files On takes precedence over Shell's per-surface exclusions.
  const bool inheritProfiles = inherit && !m_settings.hazeEnabledOverride().has_value();
  SurfaceHaze::instance().configure(
      translucent && m_settings.effectiveHazeEnabled(), animated && m_settings.effectiveAnimationsEnabled(),
      transparent, inheritProfiles ? panel.hazeSurfaces : decltype(panel.hazeSurfaces){}
  );
  IconResolver::setOverrideTheme(m_settings.iconThemeOverride());
  m_renderContext.setTextFontFamily(m_settings.effectiveFontFamily());
  m_themeService.apply();
}

void FileManager::jumpBackHistory(std::size_t steps) {
  auto& session = activeSession();
  jumpHistory(session.location(), session.backHistory(), session.forwardHistory(), steps, [this](auto location) {
    navigateLocation(std::move(location), false);
  });
}

void FileManager::jumpForwardHistory(std::size_t steps) {
  auto& session = activeSession();
  jumpHistory(session.location(), session.forwardHistory(), session.backHistory(), steps, [this](auto location) {
    navigateLocation(std::move(location), false);
  });
}

void FileManager::openHistoryMenu(bool forward, float x, float y) {
  const auto& history = forward ? activeSession().forwardHistory() : activeSession().backHistory();
  std::vector<ContextMenuControlEntry> entries;
  for (std::size_t i = 1; i <= history.size(); ++i) {
    const auto& location = history[history.size() - i];
    const auto label = location.archiveDirectory ? location.path.string() + " / " + *location.archiveDirectory
                                                 : location.path.string();
    entries.push_back({.id = static_cast<int>(i), .label = label});
  }
  if (!entries.empty()) {
    m_menus.showPlacesMenu(std::move(entries), x, y, false, [this, forward](int id) {
      if (forward)
        jumpForwardHistory(static_cast<std::size_t>(id));
      else
        jumpBackHistory(static_cast<std::size_t>(id));
    });
  }
}

void FileManager::closeLocationAutocomplete() {
  if (std::exchange(m_showingLocationSuggestions, false) && m_menus.hasOpenPopup())
    m_menus.closeAll();
}

bool FileManager::acceptLocationAutocompleteTab() {
  if (!m_locationInput || !m_surface || activeTab().administratorBrowsing || activeTab().hasActiveAdminSession())
    return false;
  const auto input = m_locationInput->value();
  auto result = filemanager::completeDirectoryPath(input, g_get_home_dir());
  if (result.suggestions.empty())
    return false;
  if (result.suggestions.size() == 1 || result.commonPrefix.size() > input.size())
    m_locationInput->setValue(result.commonPrefix);
  if (result.suggestions.size() == 1)
    return true;

  float x = 0.0F, y = 0.0F;
  Node::absolutePosition(m_locationInput, x, y);
  std::vector<ContextMenuControlEntry> entries;
  for (std::size_t i = 0; i < result.suggestions.size(); ++i)
    entries.push_back({.id = static_cast<int>(i), .label = result.suggestions[i]});
  m_showingLocationSuggestions = true;
  m_menus.showPlacesMenu(
      std::move(entries), x, y + m_locationInput->height(), true,
      [this, suggestions = std::move(result.suggestions)](int id) {
        m_showingLocationSuggestions = false;
        setLocationEditMode(true);
        m_locationInput->setValue(suggestions.at(static_cast<std::size_t>(id)));
      }
  );
  return true;
}
