#include "filemanager/file_manager.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <locale>
#include <stdexcept>

namespace {
  std::string formatCount(std::size_t count) {
    static const auto locale = [] {
      try {
        return std::locale("");
      } catch (const std::runtime_error&) {
        return std::locale::classic();
      }
    }();
    return std::format(locale, "{:L}", count);
  }
} // namespace

void FileManager::buildSearchBar(Flex& pane) {
  auto bar = std::make_unique<Flex>();
  bar->setAlign(FlexAlign::Center);
  bar->setGap(Style::spaceSm);
  m_searchBar = static_cast<Flex*>(pane.addChild(std::move(bar)));

  auto input = std::make_unique<Input>();
  m_searchInput = input.get();
  input->setPlaceholder("Search this folder and subfolders");
  input->setFlexGrow(1.0F);
  input->setOnChange([this](const std::string& query) {
    if (!activeSession().searchOpen())
      return;
    auto& tab = activeTab();
    if (!tab.session.search().active() && !query.empty()) {
      tab.browsingSelection = filemanager::BrowserTab::BrowsingSelection{
          std::move(tab.selectedPath), std::move(tab.selectedPaths), std::move(tab.selectionAnchor)
      };
      tab.searchScrollState = {};
    }
    if (tab.session.search().active() || !query.empty())
      resetSearchSelection();
    tab.session.setSearchQuery(query);
    updateStatusBar();
  });
  input->setOnSubmit([this](const std::string&) {
    activeSession().submitSearch();
    if (m_grid)
      m_dispatcher.setFocus(m_grid->focusArea());
  });
  auto status = std::make_unique<Label>();
  m_searchStatus = status.get();
  status->setFontSize(Style::fontSizeMini);
  status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  status->setMaxLines(1);
  status->setMaxWidth(185.0F);
  status->setEllipsize(TextEllipsize::End);
  status->setVisible(false);
  input->setTrailingLabel(std::move(status));

  m_searchBar->addChild(std::move(input));

  auto scope = std::make_unique<Button>();
  m_searchScopeButton = scope.get();
  m_compactSearchScope = false;
  scope->setVariant(ButtonVariant::Outline);
  scope->setTrailingGlyph("chevron-down");
  scope->setFontSize(Style::fontSizeCaption);
  scope->setContentAlign(ButtonContentAlign::Start);
  scope->label()->setFlexGrow(1.0F);
  scope->label()->setEllipsize(TextEllipsize::End);
  scope->setOnClick([this] { openSearchScopeMenu(); });
  m_searchBar->addChild(std::move(scope));

  auto mode = std::make_unique<Button>();
  m_searchModeButton = mode.get();
  mode->setVariant(ButtonVariant::Outline);
  mode->setTrailingGlyph("chevron-down");
  mode->setMinWidth(136.0F);
  mode->setMaxWidth(136.0F);
  mode->setContentAlign(ButtonContentAlign::Start);
  mode->label()->setFlexGrow(1.0F);
  mode->setTooltip("Search matching mode");
  mode->setOnClick([this] { openSearchModeMenu(); });
  m_searchBar->addChild(std::move(mode));

  auto close = std::make_unique<Button>();
  close->setVariant(ButtonVariant::Ghost);
  close->setGlyph("x");
  close->setTooltip("Close Search (Esc)");
  close->setOnClick([this] { closeSearch(); });
  m_searchBar->addChild(std::move(close));

  m_searchBar->setVisible(false);
  m_searchBar->setParticipatesInLayout(false);
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
    loadSearchView();
    configureSearchBackend(activeSession());
    const auto& settings = m_settings.searchSettings();
    activeSession().configureSearchScope({}, std::nullopt, settings.exclusions, settings.standardOptions);
  }
  activeSession().openSearch();
  syncSearchUi();
  if (m_searchInput) {
    m_dispatcher.setFocus(m_searchInput->inputArea());
    m_searchInput->selectAll();
  }
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
  m_menus.closeApplicationPicker();
  m_lastPointerIndex.reset();
  m_typeaheadQuery.clear();
  auto& tab = activeTab();
  tab.selectedPath.reset();
  tab.selectedPaths.clear();
  tab.selectionAnchor.reset();
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

void FileManager::openSearchModeMenu() {
  if (!m_searchModeButton || !activeSession().searchOpen() || !activeSession().fzfSearchActive())
    return;
  const auto current = static_cast<std::size_t>(activeSession().searchMode());
  const auto& labels = filemanager::kSearchModeLabels;
  std::vector<ContextMenuControlEntry> entries;
  for (std::size_t index = 0; index < labels.size(); ++index) {
    entries.push_back({
        .id = filemanager::menu_id::kFirstSearchMode + static_cast<int>(index),
        .label = std::string(labels[index]),
        .radio = true,
        .toggleState = index == current ? 1 : 0,
    });
  }
  m_menus.openSearchDropdown(
      std::move(entries), *m_searchModeButton, m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard
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

void FileManager::syncSearchUi() {
  if (m_listHeader.searchView() != activeSession().search().active())
    applyActiveView();
  if (!m_searchBar)
    return;
  const auto& session = activeSession();
  const bool open = session.searchOpen();
  m_searchButton->setEnabled(!activeTab().administratorBrowsing && session.canSearch());
  m_searchButton->setSelected(open);
  if ((open != m_searchBar->visible() || m_searchUiTab != activeTab().id)) {
    m_searchUiTab = activeTab().id;
    m_searchInput->setValue(session.search().query());
    m_searchBar->setVisible(open);
    m_searchBar->setParticipatesInLayout(open);
    if (!open) {
      for (const Node* node = m_dispatcher.focusedArea(); node; node = node->parent()) {
        if (node == m_searchBar) {
          m_dispatcher.setFocus(m_grid ? m_grid->focusArea() : nullptr);
          break;
        }
      }
    }
  }
  if (!open) {
    m_searchStatus->setVisible(false);
    return;
  }

  syncSearchScopeButton();
  m_searchInput->setPlaceholder(
      session.searchSelection().scope == filemanager::SearchScope::Recursive ? "Search this folder and subfolders"
                                                                             : "Search files and folders"
  );

  const bool fzf = session.fzfSearchActive();
  m_searchModeButton->setVisible(fzf);
  m_searchModeButton->setParticipatesInLayout(fzf);
  if (fzf)
    m_searchModeButton->setText(filemanager::kSearchModeLabels[static_cast<std::size_t>(session.searchMode())]);

  using Status = filemanager::FileSearchSession::Status;
  const auto& search = session.search();
  std::string status;
  switch (search.status()) {
  case Status::Idle:
    break;
  case Status::Searching:
    status = "Searching…";
    if (!search.entries().empty())
      status += " " + formatCount(search.entries().size()) + " found";
    break;
  case Status::Finished:
    status = search.entries().empty() ? "No matches" : formatCount(search.entries().size()) + " found";
    if (const auto skipped = search.skippedFolders(); skipped > 0)
      status += " · " + formatCount(skipped) + (skipped == 1 ? " folder skipped" : " folders skipped");
    break;
  case Status::Failed:
    status = "Search failed: " + search.error();
    break;
  }
  if (search.usingFallback())
    status += " · fzf unavailable";
  m_searchStatus->setVisible(!status.empty());
  m_searchStatus->setText(status);
  m_searchStatus->setTooltip(status);
}

// A directory operation already includes its descendants. Search can select both at once.
std::vector<std::string> FileManager::selectedOperationPaths() const {
  auto paths = selectedPaths();
  const auto& tab = activeTab();
  if (!tab.session.search().active() || paths.size() < 2)
    return paths;

  std::unordered_set<std::string> directories;
  for (const auto& entry : tab.session.entries()) {
    if (entry.isDir && !entry.isSymlink && tab.selectedPaths.contains(entry.absPath.string()))
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
  if (!m_grid || !activeSession().search().active() || !activeTab().capabilities().canGenerateThumbnails) {
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
  m_searchThumbnails.updateVisible(eligiblePaths, gridMode ? ThumbnailService::kDefaultTargetPx : 64);
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
