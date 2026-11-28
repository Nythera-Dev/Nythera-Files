#include "config/config_service.h"
#include "config/files_config_validator.h"
#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "filemanager/file_manager_settings.h"
#include "filemanager/settings/settings_category.h"
#include "net/http_client.h"
#include "render/core/color.h"
#include "test_isolation.h"
#include "theme/builtin_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/palette.h"
#include "theme/theme_service.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  using filemanager::FileManagerSettings;

  void write(const fs::path& path, std::string_view content) {
    std::ofstream stream(path);
    TEST_CHECK(stream << content);
  }

  std::string read(const fs::path& path) {
    std::ifstream stream(path);
    TEST_CHECK(stream);
    return {std::istreambuf_iterator<char>(stream), {}};
  }

  bool dispatch(FileManagerSettings& settings, int timeout = 200) {
    std::vector<pollfd> fds;
    const auto start = settings.addPollFds(fds);
    TEST_CHECK(fds.size() == 1);
    const auto ready = ::poll(fds.data(), fds.size(), timeout);
    TEST_CHECK(ready >= 0);
    if (ready > 0)
      settings.dispatch(fds, start);
    return ready > 0;
  }

  void drain(FileManagerSettings& settings) {
    for (int pass = 0; pass < 4 && dispatch(settings, 20); ++pass) {
    }
  }

  void separationAndInheritance() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-config-separation"});
    const auto shellDir = env.config() / "nythera";
    const auto stateDir = env.state() / "nythera";
    const auto filesDir = shellDir / "files";
    TEST_CHECK(fs::create_directory(shellDir));
    TEST_CHECK(fs::create_directory(stateDir));
    const auto legacyState = stateDir / "state.toml";
    constexpr auto existingState = "[shell]\nwallpaper = '/somewhere'\n[files]\nshow_menu_bar = false\n";
    write(legacyState, existingState);
    const auto legacyMode = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    fs::permissions(legacyState, legacyMode);
    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shell);
    TEST_CHECK(!settings.readlineShortcuts());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Solid);
    TEST_CHECK(!fs::exists(filesDir));
    TEST_CHECK(settings.saveSidebarWidth(215));
    TEST_CHECK(settings.savePropertiesSize(500, 450));
    TEST_CHECK(!fs::exists(filesDir));
    const auto state = toml::parse_file((stateDir / "files/state.toml").string());
    TEST_CHECK(state["files"]["sidebar_width"].value<std::string>() == "215.000000");
    TEST_CHECK(state["files"]["properties_window_size"].value<std::string>() == "500x450");
    TEST_CHECK(!state["files"]["show_menu_bar"]);
    TEST_CHECK(read(legacyState) == existingState && fs::status(legacyState).permissions() == legacyMode);

    const auto previous = settings.loadFileView();
    auto next = previous;
    next.showHidden = true;
    TEST_CHECK(settings.saveFileView(next, previous));
    const auto config = toml::parse_file((filesDir / "config.toml").string());
    TEST_CHECK(config["files"]["show_hidden"].value<bool>() == true);
    TEST_CHECK(config["files"].as_table()->size() == 1);
    TEST_CHECK(!config["files"]["sidebar_width"]);
    TEST_CHECK(settings.avoidRedundantExtractionFolders());
    TEST_CHECK(settings.setAvoidRedundantExtractionFolders(false));
    TEST_CHECK(!settings.avoidRedundantExtractionFolders());
    TEST_CHECK(
        toml::parse_file((filesDir / "config.toml").string())["files"]["avoid_redundant_extraction_folders"]
            .value<bool>()
        == false
    );
    TEST_CHECK(settings.previewMode() == filemanager::PreviewMode::Full);
    TEST_CHECK(settings.thumbnailPolicy() == filemanager::ThumbnailPolicy::Local);
    TEST_CHECK(settings.thumbnailMaxFileMiB() == 64 && settings.thumbnailCacheMiB() == 1024);
    TEST_CHECK(settings.setPreviewMode(filemanager::PreviewMode::Information));
    TEST_CHECK(settings.setThumbnailPolicy(filemanager::ThumbnailPolicy::Never));
    TEST_CHECK(settings.savePreviewWidth(355));
    TEST_CHECK(settings.previewMode() == filemanager::PreviewMode::Information);
    TEST_CHECK(settings.thumbnailPolicy() == filemanager::ThumbnailPolicy::Never);
    TEST_CHECK(settings.loadPreviewWidth() == 355);
    const auto previewState = toml::parse_file((stateDir / "files/state.toml").string());
    TEST_CHECK(previewState["files"]["preview_width"].value<std::string>() == "355.000000");
    TEST_CHECK(!previewState["files"]["preview_mode"]);

    TEST_CHECK(read(legacyState) == existingState);

    write(shellDir / "shell.toml", "[shell]\nreadline_shortcuts = true\n[shell.panel]\ntransparency_mode = 'glass'\n");
    shell.forceReload();
    TEST_CHECK(settings.readlineShortcuts());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Glass);
    TEST_CHECK(fs::create_directory(shellDir / "shell"));
    write(
        shellDir / "shell/shell.toml",
        "[shell]\nreadline_shortcuts = false\n[shell.panel]\ntransparency_mode = 'soft'\n"
    );
    shell.forceReload();
    TEST_CHECK(!settings.readlineShortcuts());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Soft);
    write(
        shellDir / "shell/shell.toml",
        "[shell]\nreadline_shortcuts = true\n[shell.panel]\ntransparency_mode = 'glass'\n"
    );
    shell.forceReload();
    TEST_CHECK(settings.readlineShortcuts());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Glass);
    write(filesDir / "theme.toml", "[theme]\ntransparency_mode = 'soft'\n");
    write(filesDir / "config.toml", "[files]\nreadline_shortcuts = false\nshow_hidden = true\n");
    drain(settings);
    TEST_CHECK(!settings.readlineShortcuts());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Soft);
    TEST_CHECK(settings.reset({"files", "readline_shortcuts"}));
    TEST_CHECK(settings.reset({"theme", "transparency_mode"}));
    TEST_CHECK(settings.readlineShortcuts());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Glass);
    FileManagerSettings reopened(shell);
    TEST_CHECK(reopened.loadFileView().showHidden);
    TEST_CHECK(reopened.loadSidebarWidth(0, 100, 500) == 215);
    TEST_CHECK(reopened.loadPropertiesSize() == std::pair{500U, 450U});
    TEST_CHECK(settings.saveSavedPlaces({{"z", env.home(), "First"}, {"a", env.work(), "Second"}}));
    TEST_CHECK(reopened.reload());
    const auto places = reopened.loadSavedPlaces();
    TEST_CHECK(places.size() == 2 && places[0].id == "z" && places[1].id == "a");
  }

  void includesAndLiveUpdates() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-config-live"});
    const auto root = env.config() / "nythera/files";
    TEST_CHECK(fs::create_directories(root));
    const auto primary = root / "config.toml";
    const auto secondary = root / "a.toml";
    const auto external = env.work() / "shared.toml";
    const auto included = root / "shared-link";
    fs::create_symlink(external, included);
    write(primary, "[include]\nfiles = ['shared-link']\n");
    write(secondary, "[files]\nshow_hidden = false\n[files.saved_places.'my.folder']\nlabel = 'Shared folder'\n");
    write(
        external,
        std::format(
            "# shared include\n[files]\n# user's view\nshow_hidden = true\n"
            "[files.saved_places.'my.folder']\npath = '{}'\n",
            env.home().string()
        )
    );

    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shell);
    TEST_CHECK(settings.loadSavedPlaces().size() == 1 && settings.loadFileView().showHidden);
    int reloads = 0;
    settings.setReloadCallback([&](const toml::table&) { ++reloads; });
    const auto previous = settings.loadFileView();
    auto next = previous;
    next.showHidden = false;
    TEST_CHECK(settings.saveFileView(next, previous));
    TEST_CHECK(read(external).contains("# shared include") && read(external).contains("# user's view"));
    TEST_CHECK(fs::is_symlink(included));
    TEST_CHECK(!settings.hasPreferenceKey({"files", "show_hidden"}));
    TEST_CHECK(!toml::parse_file(external.string())["files"]["show_hidden"]);
    TEST_CHECK(!toml::parse_file(secondary.string())["files"]["show_hidden"]);
    TEST_CHECK(settings.setAlwaysShowTabBar(true));
    TEST_CHECK(toml::parse_file(external.string())["files"]["always_show_tab_bar"].value<bool>() == true);
    drain(settings);
    TEST_CHECK(reloads == 0);
    TEST_CHECK(settings.saveSavedPlaces({}));
    TEST_CHECK(!toml::parse_file(external.string())["files"]["saved_places"]["my.folder"]);
    TEST_CHECK(!toml::parse_file(secondary.string())["files"]["saved_places"]["my.folder"]);
    drain(settings);
    TEST_CHECK(reloads == 0);

    const auto edited = env.work() / "editor-swap";
    write(edited, "[files]\nalways_show_tab_bar = false\nshow_hidden = true\n");
    fs::rename(edited, external);
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 1 && settings.loadFileView().showHidden && !settings.alwaysShowTabBar());
    TEST_CHECK(!settings.hasPreferenceKey({"files", "always_show_tab_bar"}));

    write(external, "[files]\nshow_hidden = [\n");
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 1 && settings.loadFileView().showHidden);
    write(external, "[files]\nshow_hidden = 1\n");
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 1 && settings.loadFileView().showHidden);
    TEST_CHECK(!settings.setAlwaysShowTabBar(true));
    TEST_CHECK(read(external).contains("show_hidden = 1"));
    fs::remove(external);
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 1 && settings.loadFileView().showHidden);
    write(external, "[files]\nshow_hidden = false\n");
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 2 && !settings.loadFileView().showHidden);

    const auto includedDir = env.work() / "parts";
    TEST_CHECK(fs::create_directory(includedDir));
    // Use an absolute path so the include directory lives outside the Files config root.
    write(primary, std::format("[include]\nfiles = ['shared-link', '{}']\n", includedDir.string()));
    drain(settings);
    write(includedDir / "new.toml", "[files]\nshow_menu_bar = false\n");
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 3 && !settings.showApplicationMenuBar());

    const auto rootFile = root / "new-root.toml";
    write(rootFile, "[files]\nopen_tabs_in_background = true\n");
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 4 && settings.openTabsInBackground());

    const auto rootTarget = env.work() / "late.toml";
    fs::create_symlink(rootTarget, root / "z-late.toml");
    drain(settings);
    write(rootTarget, "[files]\nopen_tabs_in_background = false\n");
    TEST_CHECK(dispatch(settings));
    TEST_CHECK(reloads == 5 && !settings.openTabsInBackground());
  }

  void validatesWholeCandidate() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-config-validation"});
    const auto root = env.config() / "nythera/files";
    TEST_CHECK(fs::create_directories(root));
    const auto early = root / "a.toml";
    const auto winner = root / "z.toml";
    const std::string base = std::format("[files.saved_places.entry]\npath = '{}'\n", env.home().string());
    constexpr auto winning = "[files.saved_places.entry]\nlabel = 'Home'\n";
    write(early, base);
    write(winner, winning);
    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shell);
    TEST_CHECK(settings.loadSavedPlaces().size() == 1);
    TEST_CHECK(!settings.reset({"files", "saved_places", "entry", "path"}));
    TEST_CHECK(read(early) == base && read(winner) == winning);
    TEST_CHECK(!settings.saveViewScale(100));
    TEST_CHECK(read(early) == base && read(winner) == winning);

    fs::remove(early);
    write(winner, "[include]\nautoload = false\n");
    TEST_CHECK(settings.reload());
    TEST_CHECK(!settings.setAlwaysShowTabBar(true));
    TEST_CHECK(!fs::exists(root / "config.toml"));
  }

  void stage2PreferenceRoundTrips() {
    using namespace filemanager;
    using nythera::config::ConfigKeyPath;
    const nythera::test::ScopedTestEnv env({.prefix = "files-stage2-roundtrip"});
    const auto root = env.config() / "nythera/files";
    TEST_CHECK(fs::create_directories(root));
    const auto config = root / "config.toml";
    write(config, "# Files preferences\n");
    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shell);
    FileManagerSettings reopened(shell);

    std::vector<ConfigKeyPath> tested;
    const auto roundTrip = [&](const ConfigKeyPath& path, auto set, auto get, const auto& value) {
      tested.push_back(path);
      const auto defaults = (settings.*get)();
      TEST_CHECK((settings.*set)(value));
      TEST_CHECK(settings.hasPreferenceKey(path));
      TEST_CHECK(reopened.reload());
      TEST_CHECK((reopened.*get)() == value);
      TEST_CHECK((settings.*set)(defaults));
      TEST_CHECK(!settings.hasPreferenceKey(path));
      TEST_CHECK(reopened.reload());
      TEST_CHECK((reopened.*get)() == defaults);
      TEST_CHECK(!reopened.hasPreferenceKey(path));
      TEST_CHECK(!reopened.preferences()[path[0]][path[1]]);
    };
    const auto boolean = [&](std::string key, auto set, auto get) {
      roundTrip({"files", std::move(key)}, set, get, !(settings.*get)());
    };
    boolean("restore_tabs", &FileManagerSettings::setRestoreTabs, &FileManagerSettings::restoreTabs);
    boolean(
        "remember_window_geometry", &FileManagerSettings::setRememberWindowGeometry,
        &FileManagerSettings::rememberWindowGeometry
    );
    boolean(
        "confirm_close_multiple_tabs", &FileManagerSettings::setConfirmCloseMultipleTabs,
        &FileManagerSettings::confirmCloseMultipleTabs
    );
    boolean(
        "confirm_close_active_operations", &FileManagerSettings::setConfirmCloseActiveOperations,
        &FileManagerSettings::confirmCloseActiveOperations
    );
    boolean(
        "show_places_personal", &FileManagerSettings::setShowPlacesPersonal, &FileManagerSettings::showPlacesPersonal
    );
    boolean("show_places_devices", &FileManagerSettings::setShowPlacesDevices, &FileManagerSettings::showPlacesDevices);
    boolean("show_places_network", &FileManagerSettings::setShowPlacesNetwork, &FileManagerSettings::showPlacesNetwork);
    boolean(
        "show_places_capacity", &FileManagerSettings::setShowPlacesCapacity, &FileManagerSettings::showPlacesCapacity
    );
    boolean(
        "recent_activity_enabled", &FileManagerSettings::setRecentActivityEnabled,
        &FileManagerSettings::recentActivityEnabled
    );
    boolean("show_recent_sidebar", &FileManagerSettings::setShowRecentSidebar, &FileManagerSettings::showRecentSidebar);
    boolean(
        "show_frequent_locations", &FileManagerSettings::setShowFrequentLocations,
        &FileManagerSettings::showFrequentLocations
    );

    roundTrip(
        {"files", "startup_location"}, &FileManagerSettings::setStartupLocation, &FileManagerSettings::startupLocation,
        StartupLocation::Custom
    );
    roundTrip(
        {"files", "startup_custom_path"}, &FileManagerSettings::setStartupCustomPath,
        &FileManagerSettings::startupCustomPath, env.work()
    );
    roundTrip(
        {"files", "external_open_mode"}, &FileManagerSettings::setExternalOpenMode,
        &FileManagerSettings::externalOpenMode, ExternalOpenMode::NewWindow
    );
    roundTrip(
        {"files", "new_tab_location"}, &FileManagerSettings::setNewTabLocation, &FileManagerSettings::newTabLocation,
        NewTabLocation::Custom
    );
    roundTrip(
        {"files", "new_tab_custom_path"}, &FileManagerSettings::setNewTabCustomPath,
        &FileManagerSettings::newTabCustomPath, env.work()
    );
    roundTrip(
        {"files", "new_tab_position"}, &FileManagerSettings::setNewTabPosition, &FileManagerSettings::newTabPosition,
        NewTabPosition::AfterCurrent
    );
    roundTrip(
        {"files", "middle_click_folder"}, &FileManagerSettings::setMiddleClickFolder,
        &FileManagerSettings::middleClickFolder, MiddleClickFolderAction::ForegroundTab
    );
    roundTrip(
        {"files", "click_activation"}, &FileManagerSettings::setClickActivation, &FileManagerSettings::clickActivation,
        ClickActivation::Single
    );
    roundTrip(
        {"files", "address_bar_mode"}, &FileManagerSettings::setAddressBarMode, &FileManagerSettings::addressBarMode,
        AddressBarMode::FullPath
    );
    roundTrip(
        {"files", "frequent_locations_count"}, &FileManagerSettings::setFrequentLocationsCount,
        &FileManagerSettings::frequentLocationsCount, 15
    );
    roundTrip(
        {"files", "recent_retention"}, &FileManagerSettings::setRecentRetention, &FileManagerSettings::recentRetention,
        RecentRetention::Days7
    );
    roundTrip(
        {"files", "branding_mode"}, &FileManagerSettings::setBrandingMode, &FileManagerSettings::brandingMode,
        BrandingMode::Compact
    );
    roundTrip(
        {"theme", "inherit_shell"}, &FileManagerSettings::setInheritShellTheme, &FileManagerSettings::inheritShellTheme,
        false
    );
    roundTrip(
        {"theme", "theme_file"}, &FileManagerSettings::setThemeFile, &FileManagerSettings::themeFile,
        env.work() / "palette.json"
    );
    roundTrip(
        {"theme", "transparency_mode"}, &FileManagerSettings::setTransparencyMode,
        &FileManagerSettings::transparencyModeOverride, std::optional{PanelTransparencyMode::Soft}
    );
    roundTrip(
        {"files", "font_family"}, &FileManagerSettings::setFontFamilyOverride, &FileManagerSettings::fontFamilyOverride,
        std::optional<std::string>{"Sans"}
    );
    roundTrip(
        {"files", "icon_theme"}, &FileManagerSettings::setIconThemeOverride, &FileManagerSettings::iconThemeOverride,
        std::optional<std::string>{"Example icons"}
    );
    roundTrip(
        {"files", "readline_shortcuts"}, &FileManagerSettings::setReadlineShortcuts,
        &FileManagerSettings::readlineShortcutsOverride, std::optional{settings.readlineShortcuts()}
    );
    roundTrip(
        {"files", "animations_enabled"}, &FileManagerSettings::setAnimationsEnabledOverride,
        &FileManagerSettings::animationsEnabledOverride, std::optional{settings.effectiveAnimationsEnabled()}
    );
    roundTrip(
        {"files", "animation_speed"}, &FileManagerSettings::setAnimationSpeedOverride,
        &FileManagerSettings::animationSpeedOverride, std::optional{1.25F}
    );
    roundTrip(
        {"files", "haze_enabled"}, &FileManagerSettings::setHazeEnabledOverride,
        &FileManagerSettings::hazeEnabledOverride, std::optional{settings.effectiveHazeEnabled()}
    );

    boolean(
        "show_menu_bar", &FileManagerSettings::setShowApplicationMenuBar, &FileManagerSettings::showApplicationMenuBar
    );
    boolean("always_show_tab_bar", &FileManagerSettings::setAlwaysShowTabBar, &FileManagerSettings::alwaysShowTabBar);
    boolean(
        "open_tabs_in_background", &FileManagerSettings::setOpenTabsInBackground,
        &FileManagerSettings::openTabsInBackground
    );
    boolean("fzf_search", &FileManagerSettings::setFzfSearchEnabled, &FileManagerSettings::fzfSearchEnabled);
    boolean(
        "avoid_redundant_extraction_folders", &FileManagerSettings::setAvoidRedundantExtractionFolders,
        &FileManagerSettings::avoidRedundantExtractionFolders
    );
    roundTrip(
        {"files", "preview_mode"}, &FileManagerSettings::setPreviewMode, &FileManagerSettings::previewMode,
        PreviewMode::Information
    );
    roundTrip(
        {"files", "thumbnail_policy"}, &FileManagerSettings::setThumbnailPolicy, &FileManagerSettings::thumbnailPolicy,
        ThumbnailPolicy::Never
    );
    roundTrip(
        {"files", "thumbnail_max_file_mib"}, &FileManagerSettings::setThumbnailMaxFileMiB,
        &FileManagerSettings::thumbnailMaxFileMiB, 32
    );
    roundTrip(
        {"files", "thumbnail_cache_mib"}, &FileManagerSettings::setThumbnailCacheMiB,
        &FileManagerSettings::thumbnailCacheMiB, 512
    );
    roundTrip(
        {"files", "admin_inactivity_timeout_seconds"}, &FileManagerSettings::setAdminInactivityTimeoutSeconds,
        &FileManagerSettings::adminInactivityTimeoutSeconds, 1200
    );
    roundTrip(
        {"files", "search_default_mode"}, &FileManagerSettings::setSearchDefaultMode,
        &FileManagerSettings::searchDefaultMode, SearchMode::Fuzzy
    );

    const auto checkReset = [&](ConfigKeyPath path) {
      tested.push_back(path);
      TEST_CHECK(reopened.hasPreferenceKey(path));
      TEST_CHECK(settings.reset(path));
      TEST_CHECK(reopened.reload());
      TEST_CHECK(!reopened.preferences()[path[0]][path[1]]);
    };
    const auto viewRoundTrip = [&](std::string key, bool search, auto member, auto value) {
      const auto previous = settings.loadFileView(search);
      auto next = previous;
      next.*member = value;
      TEST_CHECK(settings.saveFileView(next, previous, search));
      TEST_CHECK(reopened.reload());
      TEST_CHECK(reopened.loadFileView(search).*member == value);
      checkReset({"files", std::move(key)});
      TEST_CHECK(reopened.loadFileView(search).*member == previous.*member);
    };
    for (bool search : {false, true}) {
      viewRoundTrip(search ? "search_grid_view" : "grid_view", search, &FileViewSettings::gridMode, true);
      viewRoundTrip(
          search ? "search_sort_field" : "sort_field", search, &FileViewSettings::sortField, FileDialogSortField::Size
      );
      viewRoundTrip(
          search ? "search_sort_descending" : "sort_descending", search, &FileViewSettings::sortOrder,
          FileDialogSortOrder::Descending
      );
      TEST_CHECK(settings.saveViewScale(1.2F, search));
      TEST_CHECK(reopened.reload());
      TEST_CHECK(reopened.loadViewScale(1.F, search) == 1.2F);
      checkReset({"files", search ? "search_view_scale" : "view_scale"});
      TEST_CHECK(reopened.loadViewScale(1.F, search) == 1.F);

      auto layout = search ? kDefaultSearchColumnLayout : FileColumnLayout{};
      layout.visible[columnIndex(FileColumn::Size)] = false;
      TEST_CHECK(settings.saveListColumnLayout(layout, search));
      TEST_CHECK(reopened.reload());
      FileColumnLayout restored;
      reopened.loadListColumnLayout(restored, search);
      TEST_CHECK(restored.visible == layout.visible && restored.order == layout.order);
      checkReset({"files", search ? "search_column_layout" : "list_column_layout"});

      FileColumns widths;
      widths[FileColumn::Size] = 120;
      TEST_CHECK(settings.saveListColumnWidths(widths, search));
      TEST_CHECK(reopened.reload());
      FileColumns restoredWidths;
      reopened.loadListColumnWidths(restoredWidths, search);
      TEST_CHECK(restoredWidths.widths == widths.widths);
      checkReset({"files", search ? "search_column_widths" : "list_column_widths"});
    }
    viewRoundTrip("show_hidden", false, &FileViewSettings::showHidden, true);
    viewRoundTrip("folders_first", false, &FileViewSettings::foldersFirst, false);
    viewRoundTrip("hidden_last", false, &FileViewSettings::hiddenLast, false);
    const auto primary = rgba(0.25F, 0.F, 1.F, 1.F);
    TEST_CHECK(settings.setThemeColorOverride(ColorRole::Primary, primary));
    TEST_CHECK(reopened.reload());
    TEST_CHECK(reopened.themeColorOverride(ColorRole::Primary).has_value());
    TEST_CHECK(formatRgbaHex(*reopened.themeColorOverride(ColorRole::Primary)) == formatRgbaHex(primary));
    checkReset({"theme", "colors"});

    for (const auto& path : allResettablePreferenceKeys())
      TEST_CHECK(std::ranges::contains(tested, path));

    TEST_CHECK(settings.setConfirmCloseMultipleTabs(false));
    const auto valid = read(config);
    TEST_CHECK(valid.contains("# Files preferences"));
    TEST_CHECK(!settings.setStartupCustomPath("relative/path"));
    TEST_CHECK(!settings.setFrequentLocationsCount(21));
    TEST_CHECK(read(config) == valid);
    write(config, "[files]\nconfirm_close_multiple_tabs = 'false'\n");
    TEST_CHECK(!settings.reload());
    TEST_CHECK(!settings.confirmCloseMultipleTabs());
  }

  void shellObservesValidatedPreferenceWrites() {
    using namespace nythera::config;
    using filemanager::SettingsCategory;
    const nythera::test::ScopedTestEnv env({.prefix = "files-observed-preferences"});
    const auto dir = env.config() / "nythera/files";
    TEST_CHECK(fs::create_directories(dir));
    const auto primary = dir / "config.toml";
    const auto target = env.work() / "shared.toml";
    const auto link = dir / "included";
    fs::create_symlink(target, link);
    const std::string initial = "# Preserve this include\n[include]\nfiles = ['included']\n";
    write(primary, initial);
    write(target, "# Preserve shared preferences\n[files]\nplace_order = ['personal:recent']\n");

    // The pre-Stage 2 Shell observer rejected these keys, including the saved
    // search widths, even though Files validated the whole edit.
    const auto legacyValidator = [](const SourcedMergeResult& loaded) -> std::optional<ConfigurationProblem> {
      for (const auto key :
           {"avoid_redundant_extraction_folders", "confirm_close_multiple_tabs", "restore_tabs",
            "search_column_widths"}) {
        if (loaded.config.merged["files"][key])
          return ConfigurationProblem{{}, std::format("Invalid Files setting 'files.{}'", key)};
      }
      return validateFilesConfiguration(loaded);
    };
    ApplicationConfigMonitor stale({.appId = "files", .configDir = dir, .validator = legacyValidator});
    int staleProblems = 0;
    stale.setProblemCallback([&](const auto&, const auto&) { ++staleProblems; });
    ApplicationConfigMonitor observer({
        .appId = "files",
        .configDir = dir,
        .validator = [&](const SourcedMergeResult& loaded) {
          auto result = legacyValidator(loaded);
          if (result && matchesValidatedApplicationConfigWrite("files", dir, loaded))
            result.reset();
          return result;
        },
    });
    int problems = 0;
    observer.setProblemCallback([&](const auto&, const auto&) { ++problems; });

    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shell);
    FileManagerSettings reopened(shell);
    TEST_CHECK(settings.setConfirmCloseMultipleTabs(false));
    TEST_CHECK(settings.setAvoidRedundantExtractionFolders(false));
    TEST_CHECK(!stale.reload(false) && staleProblems == 1);
    TEST_CHECK(observer.reload(false) && problems == 0);
    TEST_CHECK(settings.setAvoidRedundantExtractionFolders(true));
    TEST_CHECK(settings.saveListColumnWidths(filemanager::FileColumns{}, true));
    TEST_CHECK(settings.setRestoreTabs(true));
    TEST_CHECK(reopened.reload());
    TEST_CHECK(reopened.restoreTabs() && !reopened.confirmCloseMultipleTabs());
    TEST_CHECK(reopened.avoidRedundantExtractionFolders());
    TEST_CHECK(observer.reload(false) && problems == 0);
    TEST_CHECK(reopened.preferences()["files"]["search_column_widths"].is_string());
    TEST_CHECK(!reopened.preferences()["files"]["avoid_redundant_extraction_folders"]);

    TEST_CHECK(settings.resetKeys(filemanager::categoryResettableKeys(SettingsCategory::General)));
    TEST_CHECK(observer.reload(false) && problems == 0);
    TEST_CHECK(reopened.reload());
    TEST_CHECK(!reopened.restoreTabs() && reopened.confirmCloseMultipleTabs());
    TEST_CHECK(reopened.preferences()["files"]["search_column_widths"]);
    TEST_CHECK(settings.resetKeys(filemanager::allResettablePreferenceKeys()));
    TEST_CHECK(observer.reload(false) && problems == 0);
    TEST_CHECK(reopened.reload());
    TEST_CHECK(!reopened.preferences()["files"]["search_column_widths"]);
    TEST_CHECK(reopened.loadPlaceOrder() == std::vector<std::string>{"personal:recent"});
    TEST_CHECK(read(primary) == initial && fs::is_symlink(link));
    const auto valid = read(target);
    TEST_CHECK(valid.contains("# Preserve shared preferences"));

    // An unrelated edit, even immediately after a valid transaction, must
    // invalidate the attestation for the merged configuration.
    write(target, "[files]\nrestore_tabs = 'true'\n");
    TEST_CHECK(!observer.reload(false) && problems == 1);
    TEST_CHECK(!settings.reload() && !settings.restoreTabs());
  }

  void searchSettingsPersistence() {
    using namespace filemanager;
    const nythera::test::ScopedTestEnv env({.prefix = "files-search-settings", .setXdgDirs = true});
    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    const auto dir = env.config() / "test-files";
    const auto state = env.state() / "test-files" / "state.toml";
    const auto location = dir / "search/config.toml";
    const CustomSearch original{"Projects", {"/", "/home/me/projects"}, {"/var/cache"}, true};
    FileManagerSettings settings(shell, dir, state);
    TEST_CHECK(settings.searchSettings() == SearchSettings{});
    TEST_CHECK(settings.saveSearchExclusions({"/home/me/private"}));
    TEST_CHECK(settings.editCustomSearch(std::nullopt, original));
    TEST_CHECK(!settings.editCustomSearch(std::nullopt, original));
    TEST_CHECK(!settings.editCustomSearch(std::nullopt, CustomSearch{" ", {"/"}}));
    TEST_CHECK(!settings.editCustomSearch(std::nullopt, CustomSearch{"Empty", {}}));
    TEST_CHECK(!settings.editCustomSearch(std::nullopt, CustomSearch{"Relative", {"relative"}}));
    int updates = 0;
    settings.setSearchReloadCallback([&](const SearchSettings&) { ++updates; });
    auto renamed = original;
    renamed.name = "Work";
    renamed.included = {"/home/me/projects"};
    TEST_CHECK(settings.editCustomSearch(original, renamed));
    renamed.includeMounts = false;
    TEST_CHECK(updates == 1);
    FileManagerSettings reopened(shell, dir, state);
    TEST_CHECK(reopened.searchSettings().profiles.size() == 1);
    TEST_CHECK(reopened.searchSettings().profiles.front() == renamed);
    TEST_CHECK(reopened.searchSettings().exclusions == SearchLocations{"/home/me/private"});
    TEST_CHECK(!reopened.editCustomSearch(original, std::nullopt));
    TEST_CHECK(reopened.editCustomSearch(renamed, std::nullopt));
    TEST_CHECK(settings.reload() && updates == 2 && settings.searchSettings().profiles.empty());

    write(location, "exclusions = ['/new/private']\n");
    drain(settings);
    TEST_CHECK(updates == 3 && settings.searchSettings().exclusions == SearchLocations{"/new/private"});
    write(location, "exclusions = ['relative']\n");
    TEST_CHECK(!settings.reload());
    TEST_CHECK(settings.searchSettings().exclusions == SearchLocations{"/new/private"});
    write(location, "exclusions = ['/new/private']\n");
    write(dir / "config.toml", "[files]\nfzf_search = 'invalid'\n");
    TEST_CHECK(!settings.reload());
    TEST_CHECK(settings.editCustomSearch(std::nullopt, original));
    TEST_CHECK(settings.searchSettings().profiles.front() == original);
  }

  void sharedThemeInheritanceAndStandaloneFallback() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-shared-theme"});
    const auto nytheraDir = env.config() / "nythera";
    const auto sharedTheme = nytheraDir / "theme.toml";
    const auto shellConfig = nytheraDir / "shell" / "config.toml";
    const auto filesTheme = nytheraDir / "files" / "theme.toml";

    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shell);
    HttpClient http;
    nythera::theme::ThemeService theme(shell, http);

    std::string resolvedMode;
    nythera::theme::GeneratedPalette resolvedPalette;
    theme.setResolvedCallback([&](const nythera::theme::GeneratedPalette& gen, std::string_view mode) {
      resolvedPalette = gen;
      resolvedMode = std::string(mode);
    });
    theme.apply();
    for (auto& call : DeferredCall::takePending()) {
      call();
    }

    const auto* nytheraDefault = nythera::theme::findBuiltinPalette("Nythera");
    const auto* ayu = nythera::theme::findBuiltinPalette("Ayu");
    TEST_CHECK(nytheraDefault != nullptr && ayu != nullptr);
    const auto nytheraGen = nythera::theme::expandBuiltinPalette(*nytheraDefault);
    const auto ayuGen = nythera::theme::expandBuiltinPalette(*ayu);

    // Standalone Files falls back to built-in Nythera palette without creating ~/.config/nythera/shell/.
    TEST_CHECK(!fs::exists(nytheraDir / "shell"));
    TEST_CHECK(resolvedMode == "dark");
    TEST_CHECK(resolvedPalette.dark.at("primary") == nytheraGen.dark.at("primary"));
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Solid);

    // Creating shared ~/.config/nythera/theme.toml + palettes/ updates standalone Files live via watchFd.
    shell.addReloadCallback([&] { theme.apply(); }, "files-theme-test");
    TEST_CHECK(nythera::theme::saveCustomPaletteFromGenerated("files-shared", ayuGen));
    TEST_CHECK(fs::exists(nytheraDir / "palettes" / "files-shared.json"));
    TEST_CHECK(fs::create_directories(nytheraDir) || fs::is_directory(nytheraDir));
    write(sharedTheme, "[theme]\nmode = \"light\"\nsource = \"custom\"\ncustom_palette = \"files-shared\"\n");

    pollfd pfd{.fd = shell.watchFd(), .events = POLLIN, .revents = 0};
    TEST_CHECK(pfd.fd >= 0 && ::poll(&pfd, 1, 500) == 1 && (pfd.revents & POLLIN) != 0);
    shell.checkReload();
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    TimerManager::instance().tick();
    for (auto& call : DeferredCall::takePending()) {
      call();
    }

    TEST_CHECK(!fs::exists(nytheraDir / "shell"));
    TEST_CHECK(shell.config().theme.mode == ThemeMode::Light);
    TEST_CHECK(shell.config().theme.source == PaletteSource::Custom);
    TEST_CHECK(resolvedMode == "light");
    TEST_CHECK(resolvedPalette.dark.at("primary") == ayuGen.dark.at("primary"));

    // Shell override in shell/config.toml wins over shared theme.toml; Files' files/theme.toml
    // remains independent and does not alter global/Shell theme state.
    TEST_CHECK(fs::create_directories(shellConfig.parent_path()));
    TEST_CHECK(fs::create_directories(filesTheme.parent_path()));
    write(shellConfig, "[theme]\nmode = \"dark\"\n");
    write(filesTheme, "[theme]\ntransparency_mode = \"soft\"\n");
    shell.forceReload();
    drain(settings);
    for (auto& call : DeferredCall::takePending()) {
      call();
    }

    TEST_CHECK(shell.config().theme.mode == ThemeMode::Dark);
    TEST_CHECK(resolvedMode == "dark");
    TEST_CHECK(resolvedPalette.dark.at("primary") == ayuGen.dark.at("primary"));
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Soft);
    TEST_CHECK(shell.config().shell.panel.transparencyMode == PanelTransparencyMode::Solid);

    // Independent Files state vs read-only Shell state inheritance:
    // Writing Files' state preserves Shell's state.toml, and InheritanceOnly reads Shell's
    // wallpaper state read-only without creating state dirs or tightening loose file modes.
    TEST_CHECK(settings.saveSidebarWidth(240));
    const auto filesState = env.state() / "nythera" / "files" / "state.toml";
    const auto shellStateDir = env.state() / "nythera" / "shell";
    const auto shellStateFile = shellStateDir / "state.toml";
    TEST_CHECK(fs::exists(filesState));
    TEST_CHECK(!fs::exists(shellStateDir));

    TEST_CHECK(fs::create_directories(shellStateDir));
    write(shellStateFile, "[wallpaper]\ndefault = \"/tmp/state-wall.png\"\nlast = \"/tmp/state-last.png\"\n");
    const auto looseMode = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    fs::permissions(shellStateFile, looseMode);
    shell.forceReload();
    TEST_CHECK(shell.getPaletteWallpaperPath() == "/tmp/state-last.png");
    TEST_CHECK(fs::status(shellStateFile).permissions() == looseMode);
    TEST_CHECK(!toml::parse_file(filesState.string()).contains("wallpaper"));

    // External update to shell/state.toml reloads via watchFd.
    write(shellStateFile, "[wallpaper]\ndefault = \"/tmp/state-wall-2.png\"\nlast = \"/tmp/state-last-2.png\"\n");
    pollfd statePfd{.fd = shell.watchFd(), .events = POLLIN, .revents = 0};
    TEST_CHECK(statePfd.fd >= 0 && ::poll(&statePfd, 1, 500) == 1 && (statePfd.revents & POLLIN) != 0);
    shell.checkReload();
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    TimerManager::instance().tick();
    TEST_CHECK(shell.getPaletteWallpaperPath() == "/tmp/state-last-2.png");
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    separationAndInheritance();
    includesAndLiveUpdates();
    validatesWholeCandidate();
    stage2PreferenceRoundTrips();
    shellObservesValidatedPreferenceWrites();
    searchSettingsPersistence();
    sharedThemeInheritanceAndStandaloneFallback();
  });
}
