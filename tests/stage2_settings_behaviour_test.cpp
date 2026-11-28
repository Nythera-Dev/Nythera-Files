#include "filemanager/file_manager.h"
#include "filemanager/path_autocomplete.h"
#include "filemanager/startup_path.h"
#include "test_isolation.h"
#include "theme/fixed_palette_json.h"
#include "util/file_utils.h"

#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace filemanager;

class FileManagerTestAccess {
public:
  static void testStartupAndNavigation(const nythera::test::ScopedTestEnv& fixture) {
    const auto a = fixture.work() / "alpha";
    const auto b = fixture.work() / "beta";
    const auto c = fixture.work() / "charlie";
    fs::create_directory(a);
    fs::create_directory(b);
    fs::create_directory(c);
    ConfigService inherited(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(inherited);
    TEST_CHECK(settings.startupLocation() == StartupLocation::Home);
    TEST_CHECK(!settings.restoreTabs() && settings.rememberWindowGeometry());
    TEST_CHECK(settings.externalOpenMode() == ExternalOpenMode::NewTab);
    TEST_CHECK(settings.confirmCloseMultipleTabs() && settings.confirmCloseActiveOperations());
    TEST_CHECK(settings.saveLastVisitedLocation(b));
    TEST_CHECK(settings.setStartupLocation(StartupLocation::Last));
    {
      FileManager manager(FileManager::StartupOptions{});
      TEST_CHECK(manager.activeSession().path() == b);
    }
    TEST_CHECK(settings.setStartupLocation(StartupLocation::Custom));
    TEST_CHECK(settings.setStartupCustomPath(c));
    {
      FileManager manager(FileManager::StartupOptions{});
      TEST_CHECK(manager.activeSession().path() == c);
    }

    FileManagerSettings anotherWindow(inherited);
    TEST_CHECK(settings.saveMainWindowGeometry({928, 704, true}));
    TEST_CHECK(anotherWindow.saveLastWindowTabs({{a, fixture.work() / "missing", b}, 2}));
    {
      FileManagerSettings reopened(inherited);
      const auto geometry = reopened.loadMainWindowGeometry();
      TEST_CHECK(geometry.has_value());
      TEST_CHECK(geometry->width == 928 && geometry->height == 704 && geometry->maximized);
      TEST_CHECK(reopened.loadLastWindowTabs().activeIndex == 2);
    }
    TEST_CHECK(settings.setRestoreTabs(true));
    {
      FileManager manager(FileManager::StartupOptions{.explicitPath = c});
      manager.restoreWindowTabs();
      TEST_CHECK(manager.activeSession().path() == c);
      TEST_CHECK(manager.m_tabs.tabs().size() == 1);
    }
    {
      FileManager manager(FileManager::StartupOptions{});
      TEST_CHECK(manager.activeSession().path() == a);
      manager.restoreWindowTabs();
      TEST_CHECK(manager.m_tabs.tabs().size() == 2);
      TEST_CHECK(manager.activeSession().path() == b);
      TEST_CHECK(manager.m_tabs.activeTabId() == manager.m_tabs.tabs().back()->id);

      TEST_CHECK(manager.m_settings.setNewTabLocation(NewTabLocation::Custom));
      TEST_CHECK(manager.m_settings.setNewTabCustomPath(c));
      TEST_CHECK(manager.m_settings.setNewTabPosition(NewTabPosition::AfterCurrent));
      const auto first = manager.m_tabs.tabs().front()->id;
      const auto last = manager.m_tabs.tabs().back()->id;
      TEST_CHECK(manager.activateTab(first));
      manager.m_openTabsInBackground = true;
      TEST_CHECK(manager.resolveNewTabPath() == c);
      const auto inserted = manager.openNewTab();
      TEST_CHECK(manager.m_tabs.tabs()[1]->id == inserted);
      TEST_CHECK(manager.m_tabs.activeTabId() == first);
      manager.navigate(c);
      manager.navigate(b);
      manager.jumpBackHistory(2);
      TEST_CHECK(manager.activeSession().path() == a);
      manager.jumpForwardHistory(1);
      TEST_CHECK(manager.activeSession().path() == c);
      TEST_CHECK(manager.m_tabs.findTab(last)->session.backHistory().empty());

      manager.m_modalDialog.m_presentToplevelForTest = [](auto&, auto*, auto) {};
      TEST_CHECK(!manager.handleWindowCloseRequest());
      TEST_CHECK(manager.m_modalDialog.m_title == "Close window?");
      TEST_CHECK(manager.m_modalDialog.m_choices.size() == 3);
      manager.m_modalDialog.closeDialog(1);
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      TEST_CHECK(manager.m_tabs.tabs().size() == 2);
      TEST_CHECK(manager.m_windowOpen);
      TEST_CHECK(manager.m_settings.setConfirmCloseMultipleTabs(false));
      TEST_CHECK(manager.handleWindowCloseRequest());
    }
  }

  static void testActiveClose(const nythera::test::ScopedTestEnv& fixture) {
    const auto source = fixture.work() / "copy-source";
    const auto target = fixture.work() / "copy-target";
    fs::create_directory(target);
    std::ofstream(source) << "copy contents";
    FileManager manager(target.string());
    manager.m_modalDialog.m_presentToplevelForTest = [](auto&, auto*, auto) {};
    TEST_CHECK(manager.m_operations.copy({source.string()}, target.string()) != 0);
    TEST_CHECK(manager.m_operations.active());
    TEST_CHECK(!manager.handleWindowCloseRequest());
    TEST_CHECK(manager.m_modalDialog.m_title == "File operations are active");
    TEST_CHECK(manager.m_modalDialog.m_choices.size() == 3);
    manager.m_modalDialog.forceCloseImmediate();
    TEST_CHECK(manager.m_settings.setConfirmCloseActiveOperations(false));
    TEST_CHECK(manager.handleWindowCloseRequest());
    TEST_CHECK(manager.m_operations.active());
    manager.requestQuit(nullptr);
    TEST_CHECK(manager.m_modalDialog.m_title == "Quit Nythera Files?");
    manager.m_modalDialog.forceCloseImmediate();
    while (manager.m_operations.active())
      g_main_context_iteration(nullptr, TRUE);
    TEST_CHECK(fs::is_regular_file(target / source.filename()));
  }

  static void testAppearanceAndPrivacy(const nythera::test::ScopedTestEnv& fixture) {
    const auto initial = fixture.work() / "alpha";
    const auto admin = fixture.work() / "admin";
    fs::create_directory(admin);
    FileManager manager(initial.string());
    manager.m_surface = std::make_unique<ToplevelSurface>(manager.m_wayland);
    const auto record = [&] { manager.recordLocationVisit(manager.activeTab()); };
    record();
    record();
    TEST_CHECK(manager.m_recentActivity.locations().size() == 1);
    TEST_CHECK(manager.m_recentActivity.locations().front().visitCount == 1);
    manager.activeTab().administratorBrowsing = true;
    manager.navigate(admin);
    record();
    TEST_CHECK(manager.m_recentActivity.locations().size() == 1);
    TEST_CHECK(manager.m_settings.loadLastVisitedLocation() == initial);
    manager.activeTab().administratorBrowsing = false;
    TEST_CHECK(manager.m_settings.setRecentActivityEnabled(false));
    manager.navigate(fixture.home());
    record();
    TEST_CHECK(manager.m_recentActivity.locations().size() == 1);
    TEST_CHECK(manager.m_settings.loadLastVisitedLocation() == fixture.home());
    TEST_CHECK(manager.m_settings.showRecentSidebar());

    const auto* builtin = nythera::theme::findBuiltinPalette("Nythera");
    TEST_CHECK(builtin != nullptr);
    const auto base = nythera::theme::expandBuiltinPalette(*builtin);
    const auto shellColor = rgba(0.F, 1.F, 0.F, 1.F);
    const auto fileColor = rgba(1.F, 0.F, 0.F, 1.F);
    const auto overrideColor = rgba(0.F, 0.F, 1.F, 1.F);
    auto inherited = base;
    auto input = nythera::theme::fixedPaletteFromTokens(base.dark);
    input.primary = shellColor;
    inherited.dark = nythera::theme::expandFixedPaletteMode(input, true);
    manager.applyFilesPalette(inherited);
    TEST_CHECK(colorForRole(ColorRole::Primary) == shellColor);
    TEST_CHECK(manager.m_settings.setInheritShellTheme(false));
    manager.applyFilesPalette(inherited);
    TEST_CHECK(colorForRole(ColorRole::Primary) != shellColor);
    TEST_CHECK(manager.m_settings.setInheritShellTheme(true));

    const auto themeFile = fixture.work() / "theme.json";
    auto filePalette = nythera::theme::mapGeneratedPaletteMode(base.dark);
    setColorForRole(filePalette, ColorRole::Primary, fileColor);
    TEST_CHECK(nythera::theme::writePaletteFile(themeFile, filePalette, filePalette));
    TEST_CHECK(manager.m_settings.setThemeFile(themeFile));
    manager.applyFilesPalette(inherited);
    TEST_CHECK(colorForRole(ColorRole::Primary) == fileColor);
    TEST_CHECK(manager.m_settings.setThemeColorOverride(ColorRole::Primary, overrideColor));
    manager.applyFilesPalette(inherited);
    TEST_CHECK(colorForRole(ColorRole::Primary) == overrideColor);
    TEST_CHECK(manager.m_settings.clearAllThemeColorOverrides());
    manager.applyFilesPalette(inherited);
    TEST_CHECK(colorForRole(ColorRole::Primary) == fileColor);
  }
};

int main() {
  return nythera::test::runTest([] {
    const nythera::test::ScopedTestEnv fixture({.prefix = "files-stage2"});
    FileManagerTestAccess::testStartupAndNavigation(fixture);
    FileManagerTestAccess::testAppearanceAndPrivacy(fixture);
    FileManagerTestAccess::testActiveClose(fixture);

    const auto path = fixture.work() / "beta";
    const auto oddPath = fixture.work() / "folder\nwith space";
    fs::create_directory(oddPath);
    fs::create_directory(fixture.work() / ".hidden");
    auto complete = completeDirectoryPath((fixture.work() / "b").string(), fixture.home());
    TEST_CHECK(complete.suggestions == std::vector<std::string>{path.string() + "/"});
    TEST_CHECK(complete.commonPrefix == path.string() + "/");
    complete = completeDirectoryPath((fixture.work() / ".h").string(), fixture.home());
    TEST_CHECK(complete.suggestions.size() == 1);

    InstanceCoordinator primary;
    InstanceCoordinator secondary;
    const auto socket = fixture.runtime() / "files.sock";
    fs::path received;
    TEST_CHECK(primary.start(socket, [&](const auto& p) {
      received = p;
      return true;
    }));
    TEST_CHECK(!secondary.start(socket, [](const auto&) { return false; }));
    bool forwarded = false;
    std::vector<pollfd> fds;
    primary.addPollFds(fds);
    {
      std::jthread sender([&] { forwarded = InstanceCoordinator::tryForwardOpenTab(oddPath, socket); });
      TEST_CHECK(::poll(fds.data(), fds.size(), 1000) > 0);
      primary.dispatch(fds, 0);
    }
    TEST_CHECK(forwarded && received == oddPath);
    primary.stop();
    TEST_CHECK(secondary.start(socket, [](const auto&) { return false; }));

    const auto history = fixture.work() / "history.json";
    RecentActivityStore first(history);
    RecentActivityStore second(history);
    const auto now = RecentActivityStore::currentEpochSeconds();
    TEST_CHECK(first.update([&](auto& state) {
      state.recordDirectoryVisit(path, now - 10);
      state.recordDirectoryVisit(path, now - 1);
      return true;
    }));
    TEST_CHECK(second.update([&](auto& state) {
      state.recordDirectoryVisit(oddPath, now);
      state.recordFileOpen(fixture.work() / "stale.txt", now - 8 * 86400);
      return true;
    }));
    TEST_CHECK(first.load());
    TEST_CHECK(first.frequentLocations(2, now).front().path == path);
    TEST_CHECK(first.setPinned(oddPath, true));
    TEST_CHECK(first.frequentLocations(2, now).front().path == oddPath);
    TEST_CHECK(first.excludeLocation(path));
    TEST_CHECK(first.frequentLocations(2, now).size() == 1);
    first.setRetention(RecentRetention::Days7);
    TEST_CHECK(first.recentItems(now).size() == 2);
    TEST_CHECK(first.clearHistory());
    TEST_CHECK(first.recentItems(now).empty());
    TEST_CHECK(first.frequentLocations(2, now).size() == 1);
  });
}
