#include "filemanager/file_manager.h"
#include "ui/controls/button.h"

#include <algorithm>

namespace {
  using filemanager::SearchScope;
  using namespace filemanager::menu_id;

  bool driveScope(SearchScope scope) { return scope == SearchScope::Local || scope == SearchScope::All; }

  std::optional<filemanager::CustomSearch>
  findProfile(const filemanager::SearchSettings& settings, std::string_view name) {
    const auto found = std::ranges::find(settings.profiles, name, &filemanager::CustomSearch::name);
    return found == settings.profiles.end() ? std::nullopt : std::optional(*found);
  }
} // namespace

void FileManager::updateSearchScopeWidth(float paneWidth) {
  if (!m_searchScopeButton || !activeSession().searchOpen())
    return;
  const bool compact = paneWidth < (activeSession().fzfSearchActive() ? 520.0F : 360.0F);
  if (m_compactSearchScope == compact)
    return;
  m_compactSearchScope = compact;
  syncSearchScopeButton();
}

void FileManager::syncSearchScopeButton() {
  const auto& selection = activeSession().searchSelection();
  const auto label = selection.scope == SearchScope::Custom
      ? std::string_view(selection.profile)
      : filemanager::kSearchScopeLabels[static_cast<std::size_t>(selection.scope)];
  const float width = m_compactSearchScope ? 78.0F : 196.0F;
  m_searchScopeButton->setMinWidth(width);
  m_searchScopeButton->setMaxWidth(width);
  m_searchScopeButton->setText(m_compactSearchScope ? "Scope" : label);
  m_searchScopeButton->setTooltip(
      selection.includeSystem ? std::string(label) + " · Includes system directories" : std::string(label)
  );
}

void FileManager::openSearchScopeMenu() {
  if (!m_searchScopeButton || !activeSession().searchOpen())
    return;
  const auto& selection = activeSession().searchSelection();
  const auto& labels = filemanager::kSearchScopeLabels;
  std::vector<ContextMenuControlEntry> entries;
  for (auto index = 0; index < static_cast<int>(SearchScope::Custom); ++index) {
    entries.push_back({
        .id = kFirstSearchScope + index,
        .label = std::string(labels[index]),
        .radio = true,
        .toggleState = index == static_cast<int>(selection.scope) ? 1 : 0,
    });
  }
  if (driveScope(selection.scope)) {
    entries.push_back({
        .id = kSearchIncludeSystem,
        .label = "Include system directories",
        .checkmark = true,
        .toggleState = selection.includeSystem ? 1 : 0,
        .indentLevel = 1,
    });
  }
  std::vector<ContextMenuControlEntry> custom;
  for (const auto& profile : m_settings.searchSettings().profiles) {
    custom.push_back({
        .id = kUseCustomSearch,
        .label = profile.name,
        .radio = true,
        .toggleState = selection.scope == SearchScope::Custom && selection.profile == profile.name ? 1 : 0,
    });
  }
  if (!custom.empty())
    custom.push_back({.separator = true});
  custom.push_back({.id = kDefineCustomSearch, .label = "Define custom search…"});
  custom.push_back({.id = kManageCustomSearches, .label = "Manage custom searches…"});
  entries.push_back({
      .label = std::string(labels.back()),
      .hasSubmenu = true,
      .radio = true,
      .toggleState = selection.scope == SearchScope::Custom ? 1 : 0,
      .submenuEntries = std::move(custom),
  });

  const auto options = activeSession().searchOptions();
  entries.push_back({.separator = true});
  entries.push_back(
      {.id = kSearchShowHidden,
       .label = "Show hidden items",
       .checkmark = true,
       .toggleState = options.showHidden ? 1 : 0}
  );
  entries.push_back(
      {.id = kSearchFollowLinks,
       .label = "Follow symbolic links",
       .checkmark = true,
       .toggleState = options.followSymlinks ? 1 : 0}
  );
  m_menus.openSearchDropdown(
      std::move(entries), *m_searchScopeButton, m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard
  );
}

bool FileManager::activateSearchScopeEntry(const ContextMenuControlEntry& entry) {
  const bool option = entry.id == kSearchShowHidden || entry.id == kSearchFollowLinks;
  if (!option && (entry.id < kFirstSearchScope || entry.id > kManageCustomSearches))
    return false;
  if (!activeSession().searchOpen())
    return true;
  if (option) {
    toggleSearchOption(
        entry.id == kSearchShowHidden ? &filemanager::SearchOptions::showHidden
                                      : &filemanager::SearchOptions::followSymlinks
    );
  } else if (entry.id < kFirstSearchScope + static_cast<int>(SearchScope::Custom)) {
    const auto scope = static_cast<SearchScope>(entry.id - kFirstSearchScope);
    const auto& previous = activeSession().searchSelection();
    selectSearchScope(
        {.scope = scope, .includeSystem = driveScope(scope) && driveScope(previous.scope) && previous.includeSystem}
    );
  } else if (entry.id == kSearchIncludeSystem) {
    auto selection = activeSession().searchSelection();
    if (driveScope(selection.scope)) {
      selection.includeSystem = !selection.includeSystem;
      selectSearchScope(std::move(selection));
    }
  } else if (entry.id == kUseCustomSearch) {
    selectSearchScope({.scope = SearchScope::Custom, .profile = entry.label});
  } else if (entry.id == kDefineCustomSearch) {
    editCustomSearch(std::nullopt);
  } else if (entry.id == kManageCustomSearches) {
    manageCustomSearches();
  }
  return true;
}

void FileManager::toggleSearchOption(bool filemanager::SearchOptions::* option) {
  const auto& selection = activeSession().searchSelection();
  const auto& settings = m_settings.searchSettings();
  bool saved = false;
  if (selection.scope == SearchScope::Custom) {
    if (auto previous = findProfile(settings, selection.profile)) {
      auto replacement = *previous;
      replacement.options.*option = !(replacement.options.*option);
      saved = m_settings.editCustomSearch(previous, replacement);
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
  auto profile = findProfile(m_settings.searchSettings(), selection.profile);
  if (selection.scope == SearchScope::Custom && !profile)
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
  const auto& settings = m_settings.searchSettings();
  const bool exclusionsChanged = previous.exclusions != settings.exclusions;
  const bool standardChanged = previous.standardOptions != settings.standardOptions;
  for (const auto& tab : m_tabs.tabs()) {
    auto& session = tab->session;
    if (!session.searchOpen())
      continue;
    auto selection = session.searchSelection();
    std::optional<filemanager::CustomSearch> profile;
    bool profileChanged = false;
    if (selection.scope == SearchScope::Custom) {
      profile = findProfile(settings, selection.profile);
      profileChanged = profile != findProfile(previous, selection.profile);
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
