#pragma once

#include "filemanager/dialogs/custom_search_dialog_controller.h"
#include "filemanager/dialogs/modal_dialog_host.h"
#include "filemanager/file_list_header.h"
#include "filemanager/file_manager_settings.h"
#include "filemanager/file_menu_controller.h"
#include "filemanager/search_content.h"
#include "filemanager/tab_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>

class Button;
class Checkbox;
class FileManagerTestAccess;
class Flex;
class Input;
class InputArea;
class Label;
class Node;
class Renderer;
class Spinner;
class ToplevelSurface;

namespace filemanager {

  /// Coordinates the search bar, filter panel, scope/mode dropdowns, and search status strip.
  class SearchBarController {
  public:
    struct Callbacks {
      std::function<ToplevelSurface*()> mainSurface;
      std::function<InputArea*()> gridFocusArea;
      std::function<bool()> isWindowOpen;
      std::function<void()> applyActiveView;
      std::function<void(std::string)> refreshSearchQuery;
      std::function<void()> resetSearchSelection;
      std::function<void()> closeSearch;
      std::function<void(SearchSelection)> selectSearchScope;
      std::function<void(bool SearchOptions::*)> toggleSearchOption;
      std::function<std::string(std::size_t)> formatCount;
    };

    SearchBarController(
        WaylandConnection& wayland, AnimationManager& animations, InputDispatcher& dispatcher,
        FileManagerSettings& settings, TabManager& tabs, FileListHeaderController& listHeader,
        FileMenuController& menus, ModalDialogHost& modalDialog, CustomSearchDialogController& customSearches,
        Callbacks callbacks
    );

    void buildSearchBar(Flex& pane, float extraTopSpacing);
    void attachStatusArea(Flex& statusRow, float fontSize);
    void detachUi();

    void setSearchButton(Button* button) noexcept { m_searchButton = button; }
    void setStatusFontSize(float fontSize);
    [[nodiscard]] bool ownsFocusedInput(const InputArea* focused) const;
    void refreshContentBackends();
    void focusInput();

    void syncUi();
    void syncFilters();
    void syncScopeButton();
    void updateStatus();
    void updateScopeWidth(float paneWidth);
    void updateFilterWidth(float paneWidth, Renderer& renderer);
    void layoutForFrame(
        float headerWidth, float fallbackScopeBudget, float statusWidth, Renderer& renderer, float& summaryWidth
    );

    [[nodiscard]] bool hasStatusArea() const noexcept { return m_searchStatusArea != nullptr; }
    [[nodiscard]] bool isStatusAreaVisible() const;
    [[nodiscard]] float measureStatusAreaWidth(Renderer& renderer) const;

    void openScopeMenu();
    void openModeMenu();
    void openFilterMenu(std::size_t field);
    bool activateScopeEntry(const ContextMenuControlEntry& entry);
    bool activateFilterEntry(const ContextMenuControlEntry& entry);

    void submitFilters();
    void changeMetadata(const std::function<void(SearchFilterDraft&)>& change);
    void openDatePicker(bool second);

  private:
    friend class ::FileManagerTestAccess;

    struct SearchFilterUi {
      Button* toggle = nullptr;
      Flex* panel = nullptr;
      Flex* metadata = nullptr;
      Flex* size = nullptr;
      Label* sizeCaption = nullptr;
      Flex* date = nullptr;
      Flex* separator = nullptr;
      Flex* content = nullptr;
      Flex* options = nullptr;
      std::array<Button*, 6> choices{};
      std::array<Input*, 4> values{};
      std::array<Button*, 2> dates{};
      std::array<Checkbox*, 2> checks{};
      Button* clear = nullptr;
      Button* submit = nullptr;
      ContentBackends backends;
    };

    void buildSearchFilters(Flex& pane);

    WaylandConnection& m_wayland;
    AnimationManager& m_animations;
    InputDispatcher& m_dispatcher;
    FileManagerSettings& m_settings;
    TabManager& m_tabs;
    FileListHeaderController& m_listHeader;
    FileMenuController& m_menus;
    ModalDialogHost& m_modalDialog;
    CustomSearchDialogController& m_customSearches;
    Callbacks m_callbacks;

    Flex* m_searchBar = nullptr;
    Flex* m_searchTools = nullptr;
    SearchFilterUi m_searchFilterUi;
    Input* m_searchInput = nullptr;
    Button* m_searchButton = nullptr;
    Button* m_searchModeButton = nullptr;
    Button* m_searchScopeButton = nullptr;
    bool m_compactSearchScope = false;
    TabId m_searchUiTab = kInvalidTabId;
    Label* m_searchStatus = nullptr;
    Spinner* m_searchSpinner = nullptr;
    Flex* m_searchStatusArea = nullptr;
  };

} // namespace filemanager
