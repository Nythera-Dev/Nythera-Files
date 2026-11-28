#pragma once

#include "filemanager/file_manager_settings.h"
#include "filemanager/settings/settings_category.h"
#include "filemanager/settings/settings_pages.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "wayland/toplevel_surface.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class Box;
class ConfigService;
class FileManagerTestAccess;
class Flex;
class InputArea;
class Label;
class Node;
class RenderContext;
class Renderer;
class RovingListNavHost;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;

namespace filemanager {

  class SettingsWindow {
  public:
    struct Callbacks {
      std::function<void(const toml::table& previous)> onPreferencesChanged;
      std::function<void(const SearchSettings& previous)> onSearchSettingsChanged;
      std::function<void()> onManageCustomSearches;
      std::function<void(std::optional<CustomSearch>)> onEditCustomSearch;
      std::function<void(std::function<void(std::string)>)> onInspectThumbnailCache;
      std::function<void()> onClearThumbnailCache;
      std::function<bool(const std::filesystem::path&, std::string&)> onExportTheme;
      std::function<void(std::string, Color, std::function<void(Color)>)> onPickColor;
      std::function<bool()> onClearRecentHistory;
    };

    SettingsWindow(
        WaylandConnection& wayland, RenderContext& renderContext, ConfigService& shellConfig,
        FileManagerSettings& settings, Callbacks callbacks
    );
    ~SettingsWindow();

    bool open(ToplevelSurface* parent = nullptr);
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }
    [[nodiscard]] wl_surface* wlSurface() const noexcept;
    [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
    [[nodiscard]] ToplevelSurface* surface() const noexcept { return m_surface.get(); }

    void prepareFrame(bool update, bool layout);
    [[nodiscard]] bool handlePointer(const PointerEvent& event);
    void handleKeyboard(const KeyboardEvent& event);

    void updateTheme();
    void onPreferencesReloaded();

    void selectCategory(SettingsCategory category);
    [[nodiscard]] SettingsCategory selectedCategory() const noexcept { return m_selectedCategory; }

  private:
    friend class ::FileManagerTestAccess;

    void deferClose();
    void requestRebuild();
    void updateScene(Renderer& renderer, std::uint32_t width, std::uint32_t height);
    void layoutScene(Flex& root, Renderer& renderer, float width, float height);
    void buildScene(Renderer& renderer, std::uint32_t width, std::uint32_t height);
    void buildSceneForTesting(Renderer& renderer, std::uint32_t width, std::uint32_t height);
    [[nodiscard]] bool mutatePreferences(const std::function<bool(FileManagerSettings&)>& fn);
    [[nodiscard]] bool mutateSearchPreferences(const std::function<bool(FileManagerSettings&)>& fn);
    [[nodiscard]] bool currentPageHasOverrides() const;
    [[nodiscard]] bool anySettingsHasOverrides() const;
    void resetCurrentPage();
    void resetAllPreferences();
    void showStatus(std::string message, bool isError);
    void dismissStatus();
    void addResizeEdges(Flex& root);
    void layoutResizeEdges(float width, float height);
    [[nodiscard]] SettingsPageContext makePageContext();

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    ConfigService& m_shellConfig;
    FileManagerSettings& m_settings;
    Callbacks m_callbacks;

    AnimationManager m_animations;
    InputDispatcher m_input;
    std::unique_ptr<ToplevelSurface> m_surface;
    std::unique_ptr<SelectDropdownPopup> m_dropdown;
    std::unique_ptr<Flex> m_root;
    Box* m_background = nullptr;
    Flex* m_sidebar = nullptr;
    RovingListNavHost* m_sidebarNav = nullptr;
    ScrollView* m_contentScroll = nullptr;
    InputArea* m_moveHandle = nullptr;
    Flex* m_headerRow = nullptr;
    Flex* m_modifiedFilter = nullptr;
    Flex* m_statusBanner = nullptr;
    Label* m_statusMessageLabel = nullptr;
    std::array<InputArea*, 8> m_resizeEdges{};

    ScrollViewState m_sidebarScrollState;
    ScrollViewState m_contentScrollState;
    SettingsCategory m_selectedCategory = SettingsCategory::General;
    bool m_modifiedOnly = false;
    bool m_confirmingPageReset = false;
    bool m_confirmingGlobalReset = false;
    bool m_contentDirty = true;
    bool m_suppressExternalReload = false;
    std::string m_thumbnailCacheUsage;
    std::uint64_t m_openGeneration = 0;
    std::string m_statusMessage;
    bool m_statusIsError = false;
    std::uint32_t m_preferredWidth = 860;
    std::uint32_t m_preferredHeight = 620;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
