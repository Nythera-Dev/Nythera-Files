#pragma once

#include "filemanager/dialogs/modal_dialog_host.h"
#include "filemanager/file_manager_settings.h"
#include "filemanager/tab_manager.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

class ConfigService;
class FileManagerTestAccess;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;

namespace filemanager {

  /// Owns the custom search profile manager and profile editor dialogs.
  class CustomSearchDialogController {
  public:
    struct Callbacks {
      std::function<TabId()> activeTabId;
      std::function<std::filesystem::path()> currentPath;
      std::function<std::optional<std::string>()> activeCustomProfile;
      std::function<bool()> isWindowOpen;
      std::function<void(std::string)> selectCustomSearch;
      std::function<void()> closeDestinationPicker;
      std::function<void(std::string)> onStatus;
    };

    CustomSearchDialogController(
        WaylandConnection& wayland, RenderContext& renderContext, ConfigService& configService,
        FileManagerSettings& settings, ModalDialogHost& modalDialog, Callbacks callbacks
    );

    void editCustomSearch(std::optional<CustomSearch> profile, std::function<void()> refreshParent = {});
    void manageCustomSearches();

    void forceCloseImmediate();
    void updateTheme();
    void requestLayoutIfOpen();

    [[nodiscard]] bool isChildOpen() const;
    [[nodiscard]] wl_surface* childSurface() const;
    void handleChildPointer(const PointerEvent& event);
    void handleChildKeyboard(const KeyboardEvent& event);

    [[nodiscard]] ModalDialogHost& childHost() noexcept { return m_customSearchChild; }
    [[nodiscard]] const ModalDialogHost& childHost() const noexcept { return m_customSearchChild; }

  private:
    friend class ::FileManagerTestAccess;

    ConfigService& m_configService;
    FileManagerSettings& m_settings;
    ModalDialogHost& m_modalDialog;
    ModalDialogHost m_customSearchChild;
    Callbacks m_callbacks;
    std::function<void()> m_refreshSearchProfileList;
  };

} // namespace filemanager
