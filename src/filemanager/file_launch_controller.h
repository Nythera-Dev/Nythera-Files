#pragma once

#include "filemanager/external_tools.h"
#include "filemanager/menu_ids.h"
#include "filemanager/protected_file_manager.h"
#include "filemanager/tab_manager.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/dialogs/search_picker_popup.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class ConfigService;
class LayerPopupHostRegistry;
class RenderContext;

namespace filemanager {

  /// Coordinates MIME probing, application discovery, Open With, external tools,
  /// and protected/standard file launching.
  class FileLaunchController {
  public:
    struct Callbacks {
      std::function<ToplevelSurface*()> mainSurface;
      std::function<const std::unordered_set<std::string>&()> selectedPaths;
      std::function<std::vector<std::string>()> selectedPathsOrdered;
      std::function<void(std::string)> onStatus;
    };

    FileLaunchController(
        WaylandConnection& wayland, RenderContext& renderContext, TabManager& tabs,
        ProtectedFileManager& protectedFiles, Callbacks callbacks
    );
    ~FileLaunchController();

    void initializePicker(ConfigService& configService, LayerPopupHostRegistry& pickerHosts);
    void closeApplicationPicker();

    [[nodiscard]] bool isApplicationPickerOpen() const;
    bool handleAppPickerPointer(const PointerEvent& event);
    bool handleKeyboard(const KeyboardEvent& event);

    void clearOpenWithApplications();
    void appendOpenWithEntries(const FileEntry& target, std::vector<ContextMenuControlEntry>& entries);
    void openApplicationChooser(const std::filesystem::path& target);
    bool launchOpenWithApp(std::size_t index, const std::filesystem::path& target);

    [[nodiscard]] std::vector<ContextMenuControlEntry> externalToolMenuRows();
    bool launchExternalTool(std::size_t index);

    void launchFileUri(TabId tabId, GFile* file, std::string success = {});
    void openProtectedFileWithDefault(TabId tabId, const std::filesystem::path& path);

    [[nodiscard]] static bool
    launchApplicationForFile(GAppInfo* app, const std::filesystem::path& path, GError** error);
    [[nodiscard]] static std::string applicationDisplayName(GAppInfo* app);
    [[nodiscard]] static std::vector<std::string> applicationIconNames(GAppInfo* app);

  private:
    void showApplicationChooser(std::string mime, const std::filesystem::path& target);
    bool launchProtectedApplication(const BrowserTab& tab, GAppInfo* app, const std::filesystem::path& path);
    [[nodiscard]] std::vector<std::string> externalToolTargets(external_tools::Tool tool) const;

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    TabManager& m_tabs;
    ProtectedFileManager& m_protectedFiles;
    Callbacks m_callbacks;

    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::unique_ptr<SearchPickerPopup> m_applicationPicker;
    std::vector<std::shared_ptr<GAppInfo>> m_openWithApplications;
    std::vector<std::pair<external_tools::Tool, external_tools::Choice>> m_externalTools;
    std::vector<std::shared_ptr<GAppInfo>> m_applicationChooserApps;
    std::string m_applicationChooserMime;
    std::shared_ptr<GCancellable> m_applicationMimeRequest;
  };

} // namespace filemanager
