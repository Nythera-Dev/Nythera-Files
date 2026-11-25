#include "config/config_service.h"
#include "filemanager/file_manager_settings.h"
#include "test_isolation.h"

#include <fstream>
#include <iterator>

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
    TEST_CHECK(read(legacyState) == existingState);

    write(shellDir / "shell.toml", "[shell]\nreadline_shortcuts = true\n[shell.panel]\ntransparency_mode = 'glass'\n");
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
    TEST_CHECK(toml::parse_file(external.string())["files"]["show_hidden"].value<bool>() == false);
    TEST_CHECK(settings.setAlwaysShowTabBar(true));
    TEST_CHECK(toml::parse_file(external.string())["files"]["always_show_tab_bar"].value<bool>() == true);
    drain(settings);
    TEST_CHECK(reloads == 0);
    TEST_CHECK(settings.reset({"files", "show_hidden"}));
    TEST_CHECK(!toml::parse_file(external.string())["files"]["show_hidden"]);
    TEST_CHECK(!toml::parse_file(secondary.string())["files"]["show_hidden"]);
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
} // namespace

int main() {
  return nythera::test::runTest([] {
    separationAndInheritance();
    includesAndLiveUpdates();
    validatesWholeCandidate();
  });
}
