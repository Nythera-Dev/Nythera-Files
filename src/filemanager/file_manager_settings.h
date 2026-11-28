#pragma once

#include "app/poll_source.h"
#include "config/config_merge.h"
#include "config/config_monitor.h"
#include "config/state_store.h"
#include "core/files/directory_scanner.h"
#include "filemanager/configuration_notice.h"
#include "filemanager/file_columns.h"
#include "filemanager/fzf_search.h"
#include "filemanager/preview/preview_options.h"
#include "filemanager/search_scope.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ConfigService;
enum class ColorRole : std::uint8_t;
enum class PanelTransparencyMode : std::uint8_t;
struct Color;

namespace filemanager {

  struct SavedPlace {
    std::string id;
    std::filesystem::path path;
    std::string label;
  };

  struct FileViewSettings {
    bool gridMode = false;
    bool showHidden = false;
    bool foldersFirst = true;
    bool hiddenLast = true;
    FileDialogSortField sortField = FileDialogSortField::Name;
    FileDialogSortOrder sortOrder = FileDialogSortOrder::Ascending;
  };

  struct SearchSettings {
    SearchLocations exclusions;
    std::vector<CustomSearch> profiles;
    SearchOptions standardOptions;
    bool operator==(const SearchSettings&) const = default;
  };

  enum class StartupLocation : std::uint8_t { Home, Last, Custom };
  enum class ExternalOpenMode : std::uint8_t { NewTab, NewWindow };
  enum class BrandingMode : std::uint8_t { Full, Compact, Off };
  enum class NewTabLocation : std::uint8_t { Current, Home, Custom, Startup };
  enum class NewTabPosition : std::uint8_t { End, AfterCurrent };
  enum class MiddleClickFolderAction : std::uint8_t { BackgroundTab, ForegroundTab, NewWindow, None };
  enum class ClickActivation : std::uint8_t { Double, Single };
  enum class AddressBarMode : std::uint8_t { Breadcrumbs, FullPath };
  enum class RecentRetention : std::uint8_t { Days7, Days30, Days90, Indefinite };

  struct WindowGeometryState {
    std::uint32_t width = 1120;
    std::uint32_t height = 760;
    bool maximized = false;
  };

  struct SavedTabsState {
    std::vector<std::filesystem::path> paths;
    std::size_t activeIndex = 0;
  };

  class FileManagerSettings final : public PollSource {
  public:
    using ProblemCallback = std::function<void(const ConfigurationProblem&, const std::filesystem::path&)>;
    using ReloadCallback = std::function<void(const toml::table& previous)>;

    explicit FileManagerSettings(const ConfigService& shellConfig);
    explicit FileManagerSettings(
        const ConfigService& shellConfig, std::filesystem::path configDir, std::filesystem::path statePath
    );
    void setReloadCallback(ReloadCallback callback) { m_onReload = std::move(callback); }
    void setManualReloadCallback(std::function<void()> callback) { m_onManualReload = std::move(callback); }
    void setProblemCallback(ProblemCallback callback);
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;
    bool reload();
    [[nodiscard]] const toml::table& preferences() const { return m_preferences; }
    [[nodiscard]] const SearchSettings& searchSettings() const { return m_searchSettings; }
    [[nodiscard]] bool saveStandardSearchOptions(SearchOptions options);
    [[nodiscard]] bool saveSearchExclusions(const SearchLocations& exclusions);
    [[nodiscard]] bool
    editCustomSearch(const std::optional<CustomSearch>& previous, const std::optional<CustomSearch>& replacement);
    void setSearchReloadCallback(std::function<void(const SearchSettings&)> callback) {
      m_onSearchReload = std::move(callback);
    }
    [[nodiscard]] bool readlineShortcuts() const;
    [[nodiscard]] std::optional<bool> readlineShortcutsOverride() const;
    [[nodiscard]] bool setReadlineShortcuts(std::optional<bool> enabled);
    [[nodiscard]] PanelTransparencyMode transparencyMode() const;
    [[nodiscard]] std::optional<PanelTransparencyMode> transparencyModeOverride() const;
    [[nodiscard]] bool setTransparencyMode(std::optional<PanelTransparencyMode> mode);
    [[nodiscard]] bool hasPreferenceKey(const nythera::config::ConfigKeyPath& path) const;
    [[nodiscard]] bool reset(const nythera::config::ConfigKeyPath& path);
    [[nodiscard]] bool resetKeys(std::span<const nythera::config::ConfigKeyPath> paths);
    [[nodiscard]] const std::string& lastSaveError() const noexcept { return m_lastSaveError; }
    [[nodiscard]] const ConfigService& shellConfig() const noexcept { return m_shellConfig; }

    [[nodiscard]] bool fzfSearchEnabled() const;
    [[nodiscard]] bool setFzfSearchEnabled(bool enabled);
    [[nodiscard]] SearchMode searchDefaultMode() const;
    [[nodiscard]] bool setSearchDefaultMode(SearchMode mode);

    [[nodiscard]] FileViewSettings loadFileView(bool search = false) const;
    [[nodiscard]] bool
    saveFileView(const FileViewSettings& settings, const FileViewSettings& previous, bool search = false);

    [[nodiscard]] bool showApplicationMenuBar() const;
    [[nodiscard]] bool setShowApplicationMenuBar(bool show);

    [[nodiscard]] bool alwaysShowTabBar() const;
    [[nodiscard]] bool setAlwaysShowTabBar(bool show);

    [[nodiscard]] bool openTabsInBackground() const;
    [[nodiscard]] bool setOpenTabsInBackground(bool background);

    [[nodiscard]] int adminInactivityTimeoutSeconds() const;
    [[nodiscard]] bool setAdminInactivityTimeoutSeconds(int seconds);

    void loadListColumnWidths(FileColumns& out, bool search = false) const;
    [[nodiscard]] bool saveListColumnWidths(const FileColumns& columns, bool search = false);

    void loadListColumnLayout(FileColumnLayout& out, bool search = false) const;
    [[nodiscard]] bool saveListColumnLayout(const FileColumnLayout& layout, bool search = false);

    [[nodiscard]] float loadViewScale(float defaultScale = 1.0F, bool search = false) const;
    [[nodiscard]] bool saveViewScale(float scale, bool search = false);

    [[nodiscard]] PreviewMode previewMode() const;
    [[nodiscard]] bool setPreviewMode(PreviewMode mode);
    [[nodiscard]] bool avoidRedundantExtractionFolders() const;
    bool setAvoidRedundantExtractionFolders(bool enabled);
    [[nodiscard]] ThumbnailPolicy thumbnailPolicy() const;
    [[nodiscard]] bool setThumbnailPolicy(ThumbnailPolicy policy);
    [[nodiscard]] int thumbnailMaxFileMiB() const;
    [[nodiscard]] bool setThumbnailMaxFileMiB(int mib);
    [[nodiscard]] int thumbnailCacheMiB() const;
    [[nodiscard]] bool setThumbnailCacheMiB(int mib);
    [[nodiscard]] float loadPreviewWidth() const;
    [[nodiscard]] bool savePreviewWidth(float width);

    [[nodiscard]] float loadSidebarWidth(float defaultWidth, float minWidth, float maxWidth) const;
    [[nodiscard]] bool saveSidebarWidth(float width);

    [[nodiscard]] float loadStatusHeight(float defaultHeight, float minHeight, float maxHeight) const;
    [[nodiscard]] bool saveStatusHeight(float height);

    [[nodiscard]] std::vector<SavedPlace> loadSavedPlaces() const;
    [[nodiscard]] bool
    saveSavedPlaces(const std::vector<SavedPlace>& places, const std::vector<std::string>* order = nullptr);

    [[nodiscard]] std::vector<std::string> loadPlaceOrder() const;
    [[nodiscard]] bool savePlaceOrder(const std::vector<std::string>& order);

    [[nodiscard]] std::vector<std::string> loadHiddenMounts() const;
    [[nodiscard]] bool saveHiddenMounts(const std::vector<std::string>& identities);

    [[nodiscard]] bool placesSectionExpanded(std::string_view key) const;
    [[nodiscard]] bool setPlacesSectionExpanded(std::string_view key, bool expanded);

    [[nodiscard]] static std::string dialogSizeStateKey(std::string_view title, bool conflictDialog);
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> loadDialogSize(std::string_view key) const;
    [[nodiscard]] bool saveDialogSize(std::string_view key, std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> loadPropertiesSize() const;
    [[nodiscard]] bool savePropertiesSize(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> loadSettingsWindowSize() const;
    [[nodiscard]] bool saveSettingsWindowSize(std::uint32_t width, std::uint32_t height);

    // Stage 2: General preferences
    [[nodiscard]] StartupLocation startupLocation() const;
    [[nodiscard]] bool setStartupLocation(StartupLocation location);
    [[nodiscard]] std::filesystem::path startupCustomPath() const;
    [[nodiscard]] bool setStartupCustomPath(const std::filesystem::path& path);
    [[nodiscard]] bool restoreTabs() const;
    [[nodiscard]] bool setRestoreTabs(bool restore);
    [[nodiscard]] bool rememberWindowGeometry() const;
    [[nodiscard]] bool setRememberWindowGeometry(bool remember);
    [[nodiscard]] ExternalOpenMode externalOpenMode() const;
    [[nodiscard]] bool setExternalOpenMode(ExternalOpenMode mode);
    [[nodiscard]] bool confirmCloseMultipleTabs() const;
    [[nodiscard]] bool setConfirmCloseMultipleTabs(bool confirm);
    [[nodiscard]] bool confirmCloseActiveOperations() const;
    [[nodiscard]] bool setConfirmCloseActiveOperations(bool confirm);

    // Stage 2: Appearance preferences
    [[nodiscard]] bool inheritShellTheme() const;
    [[nodiscard]] bool setInheritShellTheme(bool inherit);
    [[nodiscard]] std::filesystem::path themeFile() const;
    [[nodiscard]] bool setThemeFile(const std::filesystem::path& path);
    [[nodiscard]] std::map<ColorRole, Color> themeColorOverrides() const;
    [[nodiscard]] std::optional<Color> themeColorOverride(ColorRole role) const;
    [[nodiscard]] bool setThemeColorOverride(ColorRole role, std::optional<Color> color);
    [[nodiscard]] bool clearAllThemeColorOverrides();
    [[nodiscard]] std::optional<std::string> fontFamilyOverride() const;
    [[nodiscard]] std::string effectiveFontFamily() const;
    [[nodiscard]] bool setFontFamilyOverride(std::optional<std::string> family);
    [[nodiscard]] std::optional<std::string> iconThemeOverride() const;
    [[nodiscard]] bool setIconThemeOverride(std::optional<std::string> theme);
    [[nodiscard]] std::optional<bool> animationsEnabledOverride() const;
    [[nodiscard]] bool effectiveAnimationsEnabled() const;
    [[nodiscard]] bool setAnimationsEnabledOverride(std::optional<bool> enabled);
    [[nodiscard]] std::optional<float> animationSpeedOverride() const;
    [[nodiscard]] float effectiveAnimationSpeed() const;
    [[nodiscard]] bool setAnimationSpeedOverride(std::optional<float> speed);
    [[nodiscard]] std::optional<bool> hazeEnabledOverride() const;
    [[nodiscard]] bool effectiveHazeEnabled() const;
    [[nodiscard]] bool setHazeEnabledOverride(std::optional<bool> enabled);
    [[nodiscard]] BrandingMode brandingMode() const;
    [[nodiscard]] bool setBrandingMode(BrandingMode mode);

    // Stage 2: Navigation preferences
    [[nodiscard]] NewTabLocation newTabLocation() const;
    [[nodiscard]] bool setNewTabLocation(NewTabLocation location);
    [[nodiscard]] std::filesystem::path newTabCustomPath() const;
    [[nodiscard]] bool setNewTabCustomPath(const std::filesystem::path& path);
    [[nodiscard]] NewTabPosition newTabPosition() const;
    [[nodiscard]] bool setNewTabPosition(NewTabPosition position);
    [[nodiscard]] MiddleClickFolderAction middleClickFolder() const;
    [[nodiscard]] bool setMiddleClickFolder(MiddleClickFolderAction action);
    [[nodiscard]] ClickActivation clickActivation() const;
    [[nodiscard]] bool setClickActivation(ClickActivation activation);
    [[nodiscard]] AddressBarMode addressBarMode() const;
    [[nodiscard]] bool setAddressBarMode(AddressBarMode mode);
    [[nodiscard]] bool showPlacesPersonal() const;
    [[nodiscard]] bool setShowPlacesPersonal(bool show);
    [[nodiscard]] bool showPlacesDevices() const;
    [[nodiscard]] bool setShowPlacesDevices(bool show);
    [[nodiscard]] bool showPlacesNetwork() const;
    [[nodiscard]] bool setShowPlacesNetwork(bool show);
    [[nodiscard]] bool showPlacesCapacity() const;
    [[nodiscard]] bool setShowPlacesCapacity(bool show);
    [[nodiscard]] bool recentActivityEnabled() const;
    [[nodiscard]] bool setRecentActivityEnabled(bool enabled);
    [[nodiscard]] bool showRecentSidebar() const;
    [[nodiscard]] bool setShowRecentSidebar(bool show);
    [[nodiscard]] bool showFrequentLocations() const;
    [[nodiscard]] bool setShowFrequentLocations(bool show);
    [[nodiscard]] int frequentLocationsCount() const;
    [[nodiscard]] bool setFrequentLocationsCount(int count);
    [[nodiscard]] RecentRetention recentRetention() const;
    [[nodiscard]] bool setRecentRetention(RecentRetention retention);

    // Stage 2: Runtime state persistence (state.toml)
    [[nodiscard]] std::filesystem::path stateDir() const;
    [[nodiscard]] std::optional<WindowGeometryState> loadMainWindowGeometry() const;
    [[nodiscard]] bool saveMainWindowGeometry(const WindowGeometryState& geometry);
    [[nodiscard]] std::optional<std::filesystem::path> loadLastVisitedLocation() const;
    [[nodiscard]] bool saveLastVisitedLocation(const std::filesystem::path& path);
    [[nodiscard]] SavedTabsState loadLastWindowTabs() const;
    [[nodiscard]] bool saveLastWindowTabs(const SavedTabsState& tabs);

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    using Change = std::pair<nythera::config::ConfigKeyPath, std::optional<std::string>>;
    bool save(std::vector<Change> changes);
    void recordSelfWrite(const std::filesystem::path& path, std::string content);
    bool reloadSearchSettings();
    bool updateRuntimeState(const std::function<bool()>& update);
    [[nodiscard]] std::filesystem::path searchSettingsPath() const;
    void reportProblem(ConfigurationProblem problem);
    void accept(nythera::config::SourcedMergeResult loaded, bool notify);
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
    loadWindowSize(std::string_view key, std::uint32_t minWidth, std::uint32_t minHeight) const;

    const ConfigService& m_shellConfig;
    const std::filesystem::path m_configDir;
    StateStore m_state;
    toml::table m_preferences;
    SearchSettings m_searchSettings;
    std::function<void(const SearchSettings&)> m_onSearchReload;
    ReloadCallback m_onReload;
    std::function<void()> m_onManualReload;
    ProblemCallback m_onProblem;
    std::optional<ConfigurationProblem> m_problem;
    std::string m_lastSaveError;
    std::optional<nythera::config::ApplicationConfigMonitor> m_monitor;
  };

} // namespace filemanager
