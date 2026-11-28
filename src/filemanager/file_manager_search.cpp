#include "filemanager/file_manager.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"

#include <algorithm>

void FileManager::refreshSearchQuery(std::string query) {
  auto& tab = activeTab();
  if (!tab.session.searchOpen())
    return;
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  const bool willSearch =
      !query.empty() || filemanager::resolveSearchFilters(tab.session.searchFilters(), now).active();
  if (!tab.session.search().active() && willSearch) {
    tab.saveBrowsingSelection();
    tab.searchScrollState = {};
  }
  if (tab.session.search().active() || willSearch)
    resetSearchSelection();
  tab.session.setSearchQuery(std::move(query));
  updateStatusBar();
}

void FileManager::focusSearch() {
  if (activeTab().administratorBrowsing || !activeSession().canSearch()) {
    setStatus("Search is available in regular folders");
    return;
  }
  if (m_renamePath)
    cancelRename();
  if (m_editingLocation)
    setLocationEditMode(false);
  if (!activeSession().searchOpen()) {
    m_searchBarController.refreshContentBackends();
    loadSearchView();
    configureSearchBackend(activeSession());
    const auto& settings = m_settings.searchSettings();
    activeSession().configureSearchScope({}, std::nullopt, settings.exclusions, settings.standardOptions);
  }
  activeSession().openSearch();
  syncSearchUi();
  m_searchBarController.focusInput();
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::toggleSearch() {
  if (activeSession().searchOpen())
    closeSearch();
  else
    focusSearch();
}

void FileManager::closeSearch() {
  if (!activeSession().searchOpen())
    return;
  if (activeSession().search().active())
    resetSearchSelection();
  activeSession().closeSearch();
  updateStatusBar();
}

void FileManager::resetSearchSelection() {
  m_searchThumbnails.clear();
  cancelFileGesture();
  m_launcher.closeApplicationPicker();
  m_selection.clearLastPointerIndex();
  m_selection.clearTypeahead();
  auto& tab = activeTab();
  tab.clearSelection();
  tab.cancelSelectionInfoQuery();
  if (m_grid && tab.session.search().active())
    m_grid->scrollView().requestScrollToOffset(0.0F);
}

void FileManager::configureSearchBackend(filemanager::DirectorySession& session) {
  const auto settings = m_settings.preferences()["files"];
  session.configureSearchBackend(
      settings["fzf_search"].value_or(true) ? filemanager::findFzf() : std::string{},
      filemanager::searchModeFromConfig(settings["search_default_mode"].value_or(std::string_view{}))
  );
}

void FileManager::selectSearchMode(filemanager::SearchMode mode) {
  if (!activeSession().searchOpen() || !activeSession().fzfSearchActive())
    return;
  if (activeSession().search().active() && !filemanager::manualSearchMode(activeSession().search().query()))
    resetSearchSelection();
  activeSession().setSearchMode(mode);
  syncSearchUi();
}

void FileManager::toggleSearchOption(bool filemanager::SearchOptions::* option) {
  const auto& selection = activeSession().searchSelection();
  const auto& settings = m_settings.searchSettings();
  bool saved = false;
  if (selection.scope == filemanager::SearchScope::Custom) {
    const auto found = std::ranges::find(settings.profiles, selection.profile, &filemanager::CustomSearch::name);
    if (found != settings.profiles.end()) {
      auto replacement = *found;
      replacement.options.*option = !(replacement.options.*option);
      saved = m_settings.editCustomSearch(*found, replacement);
    }
  } else {
    auto options = settings.standardOptions;
    options.*option = !(options.*option);
    saved = m_settings.saveStandardSearchOptions(options);
  }
  if (!saved)
    setStatus("Could not save search options");
}

void FileManager::selectSearchScope(filemanager::SearchSelection selection) {
  if (!activeSession().searchOpen() || activeSession().searchSelection() == selection)
    return;
  const auto found =
      std::ranges::find(m_settings.searchSettings().profiles, selection.profile, &filemanager::CustomSearch::name);
  auto profile = found == m_settings.searchSettings().profiles.end() ? std::nullopt
                                                                     : std::optional<filemanager::CustomSearch>(*found);
  if (selection.scope == filemanager::SearchScope::Custom && !profile)
    return;
  if (activeSession().search().active())
    resetSearchSelection();
  activeSession().configureSearchScope(
      std::move(selection), std::move(profile), m_settings.searchSettings().exclusions,
      m_settings.searchSettings().standardOptions
  );
  syncSearchUi();
}

void FileManager::refreshSearchSettings(const filemanager::SearchSettings& previous) {
  if (m_settingsWindow)
    m_settingsWindow->onPreferencesReloaded();
  const auto& settings = m_settings.searchSettings();
  const bool exclusionsChanged = previous.exclusions != settings.exclusions;
  const bool standardChanged = previous.standardOptions != settings.standardOptions;
  const auto findIn = [](const filemanager::SearchSettings& s, std::string_view name) {
    const auto it = std::ranges::find(s.profiles, name, &filemanager::CustomSearch::name);
    return it == s.profiles.end() ? std::nullopt : std::optional<filemanager::CustomSearch>(*it);
  };
  for (const auto& tab : m_tabs.tabs()) {
    auto& session = tab->session;
    if (!session.searchOpen())
      continue;
    auto selection = session.searchSelection();
    std::optional<filemanager::CustomSearch> profile;
    bool profileChanged = false;
    if (selection.scope == filemanager::SearchScope::Custom) {
      profile = findIn(settings, selection.profile);
      profileChanged = profile != findIn(previous, selection.profile);
      if (!profile)
        selection = {};
    }
    const bool restart = exclusionsChanged || profileChanged || (!profile && standardChanged);
    if (restart || standardChanged) {
      if (restart && tab->id == activeTab().id && session.search().active())
        resetSearchSelection();
      session.configureSearchScope(
          std::move(selection), std::move(profile), settings.exclusions, settings.standardOptions
      );
    }
  }
  syncSearchUi();
}

// A directory operation already includes its descendants. Search can select both at once.
std::vector<std::string> FileManager::selectedOperationPaths() const {
  auto paths = selectedPaths();
  const auto& tab = activeTab();
  if (!tab.session.search().active() || paths.size() < 2)
    return paths;

  std::unordered_set<std::string> directories;
  for (const auto& entry : tab.session.entries()) {
    if (entry.isDir && !entry.isSymlink && tab.isPathSelected(entry.absPath.string()))
      directories.insert(entry.absPath.string());
  }
  if (!directories.empty()) {
    std::erase_if(paths, [&directories](const std::string& path) {
      for (auto parent = std::filesystem::path(path).parent_path(); parent != parent.root_path();
           parent = parent.parent_path()) {
        if (directories.contains(parent.string()))
          return true;
      }
      return false;
    });
  }
  return paths;
}

void FileManager::updateSearchFolderCounts() {
  auto& session = activeSession();
  if (!m_grid || !session.search().active())
    return;
  std::unordered_map<std::string, std::size_t> visible;
  if (!m_listHeader.gridMode()
      && m_listHeader.columnLayout().visible[filemanager::columnIndex(filemanager::FileColumn::Size)]) {
    const auto& entries = session.entries();
    m_grid->visitVisibleTiles([&](Node&, std::size_t index) {
      if (index < entries.size() && entries[index].isDir)
        visible.emplace(entries[index].absPath.string(), index);
    });
  }
  session.updateVisibleSearchFolders(std::move(visible));
}

void FileManager::updateSearchThumbnails(Renderer& renderer) {
  if (!m_grid || activeTab().hasActiveAdminSession() || !activeTab().capabilities().canGenerateThumbnails) {
    m_searchThumbnails.clear();
    m_searchThumbnails.releaseRetired();
    return;
  }

  const auto& entries = activeSession().entries();
  const bool gridMode = m_listHeader.gridMode();
  std::vector<std::string> eligiblePaths;
  m_grid->visitVisibleTiles([&](Node&, std::size_t index) {
    if (index < entries.size() && filemanager::SearchThumbnails::eligible(entries[index]))
      eligiblePaths.push_back(entries[index].absPath.string());
  });
  m_searchThumbnails.updateVisible(
      eligiblePaths, gridMode ? ThumbnailService::kDefaultTargetPx : 64, m_settings.thumbnailPolicy()
  );
  m_grid->visitVisibleTiles([&](Node& tile, std::size_t index) {
    const auto handle =
        index < entries.size() ? m_searchThumbnails.peek(entries[index].absPath.string()) : TextureHandle{};
    if (gridMode)
      static_cast<FileEntryTile&>(tile).showThumbnail(renderer, handle);
    else
      static_cast<FileEntryRow&>(tile).showThumbnail(renderer, handle);
  });
}

void FileManager::releaseSearchThumbnails() {
  m_searchThumbnails.clear();
  if (m_surface)
    m_renderContext.makeCurrent(m_surface->renderTarget());
  m_searchThumbnails.releaseRetired();
}
