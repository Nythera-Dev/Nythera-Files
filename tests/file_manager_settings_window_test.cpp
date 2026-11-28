#include "config/config_service.h"
#include "core/input/keybind_matcher.h"
#include "filemanager/dialogs/help_dialog.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_manager_settings.h"
#include "filemanager/menu_ids.h"
#include "filemanager/settings/palette_colour_preference.h"
#include "filemanager/settings/settings_category.h"
#include "filemanager/settings/settings_window.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  namespace fs = std::filesystem;
  using namespace filemanager;

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int maxLines, TextAlign, TextEllipsize,
        bool
    ) override {
      constexpr float kAdvance = 8.0F;
      const float natural = static_cast<float>(text.size()) * kAdvance;
      float width = natural;
      int lineCount = text.empty() ? 0 : 1;
      if (maxWidth > 0.0F && natural > maxWidth) {
        const float perLine = std::floor(maxWidth / kAdvance) * kAdvance;
        lineCount = perLine > 0.0F ? static_cast<int>(std::ceil(natural / perLine)) : 1;
        if (maxLines > 0)
          lineCount = std::min(lineCount, maxLines);
        width = maxWidth;
      }
      return TextMetrics{
          .width = width,
          .right = width,
          .bottom = fontSize * static_cast<float>(lineCount),
          .lineCount = lineCount,
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextMetrics measureGlyph(char32_t, float) override { return {.width = 16.0F, .right = 16.0F, .bottom = 16.0F}; }
    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  void writeText(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path);
    TEST_CHECK(stream << content);
  }

  std::string readText(const fs::path& path) {
    std::ifstream stream(path);
    TEST_CHECK(stream);
    return {std::istreambuf_iterator<char>(stream), {}};
  }

  bool dispatchSettings(FileManagerSettings& settings, int timeoutMs = 200) {
    std::vector<pollfd> fds;
    const auto start = settings.addPollFds(fds);
    TEST_CHECK(fds.size() == 1);
    const auto ready = ::poll(fds.data(), fds.size(), timeoutMs);
    TEST_CHECK(ready >= 0);
    if (ready > 0)
      settings.dispatch(fds, start);
    return ready > 0;
  }

  template <typename T> void collectNodes(const Node* root, std::vector<const T*>& out) {
    if (root == nullptr)
      return;
    if (const auto* match = dynamic_cast<const T*>(root))
      out.push_back(match);
    for (const auto& child : root->children())
      collectNodes(child.get(), out);
  }

  Select* paletteRoleSelect(const Node& page, std::string_view role) {
    std::vector<const Select*> selects;
    collectNodes(&page, selects);
    const auto key = "palette-colour:theme:colors:" + std::string(role);
    const auto select = std::ranges::find_if(selects, [&](const Select* node) {
      return std::ranges::any_of(node->children(), [&](const auto& child) {
        const auto* area = dynamic_cast<const InputArea*>(child.get());
        return area && area->tabFocusKey() == key;
      });
    });
    return select == selects.end() ? nullptr : const_cast<Select*>(*select);
  }

} // namespace

class FileManagerTestAccess {
public:
  static void runWindowAndCategoryTests() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-settings-window"});
    // Standalone operation without Nythera Shell config installed or running
    ConfigService shellConfig(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings windowA(shellConfig);
    FileManagerSettings windowB(shellConfig);

    WaylandConnection wayland;
    RenderContext renderContext;
    StubRenderer renderer;

    int prefCallbacks = 0;
    int searchCallbacks = 0;
    SettingsWindow win(
        wayland, renderContext, shellConfig, windowA,
        {
            .onPreferencesChanged = [&](const toml::table&) { ++prefCallbacks; },
            .onSearchSettingsChanged = [&](const SearchSettings&) { ++searchCallbacks; },
        }
    );

    // 1. Verify exact 8 sidebar categories in order, no About category, and layout boundary
    TEST_CHECK(kSettingsCategories.size() == 8);
    const std::array<std::string_view, 8> expectedLabels{
        "General", "Appearance", "Navigation", "Files & Folders", "Search", "Integrations", "Shortcuts", "Advanced",
    };
    for (std::size_t i = 0; i < kSettingsCategories.size(); ++i) {
      TEST_CHECK(kSettingsCategories[i].label == expectedLabels[i]);
      TEST_CHECK(kSettingsCategories[i].label != "About");
    }

    win.buildSceneForTesting(renderer, 860, 620);
    TEST_CHECK(win.m_sidebar != nullptr);
    TEST_CHECK(win.m_headerRow != nullptr);
    float sidebarX = 0.0F;
    float sidebarY = 0.0F;
    float headerX = 0.0F;
    float headerY = 0.0F;
    Node::absolutePosition(win.m_sidebar, sidebarX, sidebarY);
    Node::absolutePosition(win.m_headerRow, headerX, headerY);
    TEST_CHECK(sidebarY == 0.0F);
    TEST_CHECK(win.m_sidebar->height() == 620.0F);
    TEST_CHECK(headerX >= sidebarX + win.m_sidebar->width());

    std::vector<const Button*> sidebarButtons;
    collectNodes(win.m_sidebar, sidebarButtons);
    const auto buttonText = [](const Button* b) -> std::string_view {
      return b->label() != nullptr ? std::string_view(b->label()->text()) : std::string_view{};
    };
    for (const auto expected : expectedLabels) {
      TEST_CHECK(std::ranges::any_of(sidebarButtons, [&](const Button* b) { return buttonText(b) == expected; }));
    }
    TEST_CHECK(std::ranges::none_of(sidebarButtons, [&](const Button* b) { return buttonText(b) == "About"; }));

    // A frame between press and release must keep the same capture and activate the row.
    const auto appearance =
        std::ranges::find_if(sidebarButtons, [&](const Button* button) { return buttonText(button) == "Appearance"; });
    TEST_CHECK(appearance != sidebarButtons.end());
    float x = 0.F, y = 0.F;
    Node::absolutePosition(*appearance, x, y);
    x += (*appearance)->width() / 2.F;
    y += (*appearance)->height() / 2.F;
    win.m_input.pointerEnter(x, y, 1);
    TEST_CHECK(win.m_input.pointerButton(x, y, BTN_LEFT, true, 1, 1, false));
    TEST_CHECK(win.m_input.pointerCaptured());
    const float contentWidth = win.m_contentScroll->width();
    win.updateScene(renderer, 860, 620);
    TEST_CHECK(win.m_contentScroll->width() == contentWidth);
    win.updateScene(renderer, 862, 622);
    TEST_CHECK(win.m_contentScroll->width() >= contentWidth);
    TEST_CHECK(win.m_input.pointerCaptured());
    TEST_CHECK(win.m_input.pointerButton(x, y, BTN_LEFT, false, 2, 2, false));
    TEST_CHECK(win.selectedCategory() == SettingsCategory::Appearance);

    // Build every category page in standalone mode (no Shell config)
    for (const auto& desc : kSettingsCategories) {
      win.selectCategory(desc.category);
      win.buildSceneForTesting(renderer, 860, 620);
      TEST_CHECK(win.m_root != nullptr);
    }

    // 2. Automatic saving, live callback, and multi-window consistency
    TEST_CHECK(win.mutatePreferences([](FileManagerSettings& s) {
      return s.setTransparencyMode(PanelTransparencyMode::Glass)
          && s.setShowApplicationMenuBar(false)
          && s.setAlwaysShowTabBar(true)
          && s.setOpenTabsInBackground(true)
          && s.setPreviewMode(PreviewMode::Information)
          && s.setThumbnailPolicy(ThumbnailPolicy::All)
          && s.setThumbnailMaxFileMiB(32)
          && s.setFzfSearchEnabled(false)
          && s.setSearchDefaultMode(SearchMode::Fuzzy)
          && s.setAvoidRedundantExtractionFolders(false)
          && s.setReadlineShortcuts(true)
          && s.setAdminInactivityTimeoutSeconds(1800)
          && s.setThumbnailCacheMiB(2048)
          && s.setStartupLocation(StartupLocation::Last)
          && s.setAddressBarMode(AddressBarMode::FullPath)
          && s.setThemeColorOverride(ColorRole::Primary, rgba(1.F, 0.F, 0.F, 1.F));
    }));
    TEST_CHECK(prefCallbacks == 1);
    const auto excluded = env.work() / "excluded";
    CustomSearch profile;
    profile.name = "Work";
    profile.included.emplace_back(env.work());
    TEST_CHECK(win.mutateSearchPreferences([&](FileManagerSettings& s) {
      SearchLocations exclusions;
      exclusions.emplace_back(excluded);
      return s.saveSearchExclusions(exclusions) && s.editCustomSearch(std::nullopt, profile);
    }));
    TEST_CHECK(searchCallbacks == 1);

    TEST_CHECK(windowB.reload());
    TEST_CHECK(windowB.transparencyMode() == PanelTransparencyMode::Glass);
    TEST_CHECK(!windowB.showApplicationMenuBar());
    TEST_CHECK(windowB.alwaysShowTabBar());
    TEST_CHECK(windowB.openTabsInBackground());
    TEST_CHECK(windowB.previewMode() == PreviewMode::Information);
    TEST_CHECK(windowB.thumbnailPolicy() == ThumbnailPolicy::All);
    TEST_CHECK(windowB.thumbnailMaxFileMiB() == 32);
    TEST_CHECK(!windowB.fzfSearchEnabled());
    TEST_CHECK(windowB.searchDefaultMode() == SearchMode::Fuzzy);
    TEST_CHECK(!windowB.avoidRedundantExtractionFolders());
    TEST_CHECK(windowB.readlineShortcuts());
    TEST_CHECK(windowB.adminInactivityTimeoutSeconds() == 1800);
    TEST_CHECK(windowB.thumbnailCacheMiB() == 2048);
    TEST_CHECK(windowB.searchSettings().exclusions == SearchLocations{excluded});
    TEST_CHECK(windowB.startupLocation() == StartupLocation::Last);
    TEST_CHECK(windowB.addressBarMode() == AddressBarMode::FullPath);
    TEST_CHECK(windowB.themeColorOverride(ColorRole::Primary) == rgba(1.F, 0.F, 0.F, 1.F));
    TEST_CHECK(windowB.searchSettings().profiles.size() == 1);

    // 3. External configuration reload reflects in SettingsWindow
    const auto filesDir = env.config() / "nythera/files";
    {
      auto text = readText(filesDir / "config.toml");
      const auto pos = text.find("always_show_tab_bar = true");
      TEST_CHECK(pos != std::string::npos);
      text.replace(pos, sizeof("always_show_tab_bar = true") - 1, "always_show_tab_bar = false");
      writeText(filesDir / "config.toml", text);
    }
    TEST_CHECK(dispatchSettings(windowA));
    TEST_CHECK(!windowA.alwaysShowTabBar());
    win.selectCategory(SettingsCategory::Navigation);
    win.buildSceneForTesting(renderer, 860, 620);
    std::vector<const Toggle*> toggles;
    collectNodes(win.m_contentScroll, toggles);
    TEST_CHECK(toggles.size() == 9);
    TEST_CHECK(!toggles[0]->checked());
    TEST_CHECK(toggles[1]->checked());

    // 4. Failed configuration write & rollback: UI must remain consistent with persisted state
    TEST_CHECK(!win.mutatePreferences([](FileManagerSettings& s) { return s.setThumbnailMaxFileMiB(999); }));
    TEST_CHECK(win.m_statusIsError);
    TEST_CHECK(!win.m_statusMessage.empty());
    TEST_CHECK(windowA.thumbnailMaxFileMiB() == 32);
    win.selectCategory(SettingsCategory::FilesAndFolders);
    win.buildSceneForTesting(renderer, 860, 620);
    std::vector<const Stepper*> steppers;
    collectNodes(win.m_contentScroll, steppers);
    TEST_CHECK(steppers.size() == 1 && steppers[0]->value() == 32);
  }

  static void runResetWithIncludesAndSymlinksTest() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-settings-reset"});
    const auto filesDir = env.config() / "nythera/files";
    TEST_CHECK(fs::create_directories(filesDir));
    const auto external = env.work() / "shared-prefs.toml";
    const auto linkPath = filesDir / "shared-link";
    fs::create_symlink(external, linkPath);

    writeText(filesDir / "config.toml", "[include]\nfiles = ['shared-link']\n");
    writeText(
        external,
        "# keep this comment\n"
        "[files]\n"
        "always_show_tab_bar = true\n"
        "open_tabs_in_background = true\n"
        "show_hidden = true\n"
        "avoid_redundant_extraction_folders = false\n"
        "[files.saved_places.home]\n"
        "path = '"
            + env.home().string()
            + "'\n"
              "label = 'Home'\n"
    );
    writeText(filesDir / "theme.toml", "[theme]\ntransparency_mode = 'glass'\n");

    ConfigService shellConfig(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shellConfig);
    TEST_CHECK(settings.saveSidebarWidth(240.0F));
    TEST_CHECK(settings.saveSettingsWindowSize(900, 680));

    WaylandConnection wayland;
    RenderContext renderContext;
    StubRenderer renderer;
    SettingsWindow win(wayland, renderContext, shellConfig, settings, {});

    TEST_CHECK(settings.setAddressBarMode(AddressBarMode::FullPath));
    TEST_CHECK(settings.setStartupLocation(StartupLocation::Last));
    TEST_CHECK(settings.setThemeColorOverride(ColorRole::Primary, rgba(1.F, 0.F, 0.F, 1.F)));

    // Page reset on Navigation (2-step confirm)
    win.selectCategory(SettingsCategory::Navigation);
    win.resetCurrentPage();
    TEST_CHECK(win.m_confirmingPageReset);
    TEST_CHECK(settings.alwaysShowTabBar());
    win.resetCurrentPage();
    TEST_CHECK(!win.m_confirmingPageReset);
    TEST_CHECK(!settings.alwaysShowTabBar());
    TEST_CHECK(!settings.openTabsInBackground());
    TEST_CHECK(settings.addressBarMode() == AddressBarMode::Breadcrumbs);
    TEST_CHECK(settings.startupLocation() == StartupLocation::Last);
    // Other categories, comments, symlink, saved_places, and runtime state remain intact
    TEST_CHECK(settings.loadFileView().showHidden);
    TEST_CHECK(!settings.avoidRedundantExtractionFolders());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Glass);
    TEST_CHECK(fs::is_symlink(linkPath));
    TEST_CHECK(readText(external).contains("# keep this comment"));
    TEST_CHECK(settings.loadSavedPlaces().size() == 1);
    TEST_CHECK(settings.loadSidebarWidth(0.0F, 100.0F, 500.0F) == 240.0F);
    TEST_CHECK(settings.loadSettingsWindowSize() == std::pair{900U, 680U});

    // Global reset (2-step confirm)
    win.resetAllPreferences();
    TEST_CHECK(win.m_confirmingGlobalReset);
    win.resetAllPreferences();
    TEST_CHECK(!win.m_confirmingGlobalReset);
    TEST_CHECK(!settings.loadFileView().showHidden);
    TEST_CHECK(settings.avoidRedundantExtractionFolders());
    TEST_CHECK(settings.transparencyMode() == PanelTransparencyMode::Solid);
    TEST_CHECK(settings.startupLocation() == StartupLocation::Home);
    TEST_CHECK(settings.themeColorOverrides().empty());
    TEST_CHECK(fs::is_symlink(linkPath));
    TEST_CHECK(readText(external).contains("# keep this comment"));
    TEST_CHECK(settings.loadSavedPlaces().size() == 1);
    TEST_CHECK(settings.loadSidebarWidth(0.0F, 100.0F, 500.0F) == 240.0F);
    TEST_CHECK(settings.loadSettingsWindowSize() == std::pair{900U, 680U});
  }

  static void runColourEditorTests() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-colour-editor"});
    ConfigService shellConfig(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shellConfig);
    const auto themeFile = env.work() / "custom-palette.json";
    TEST_CHECK(settings.setThemeFile(themeFile));
    TEST_CHECK(settings.setInheritShellTheme(false));
    TEST_CHECK(settings.setBrandingMode(BrandingMode::Compact));
    TEST_CHECK(settings.setThemeColorOverride(ColorRole::Shadow, hex("#AE828180")));

    WaylandConnection wayland;
    RenderContext renderContext;
    StubRenderer renderer;
    std::optional<Color> pickerInitial;
    SettingsWindow window(
        wayland, renderContext, shellConfig, settings, {.onPickColor = [&](std::string title, Color initial, auto) {
          TEST_CHECK(title == "Edit Primary");
          pickerInitial = initial;
        }}
    );
    window.selectCategory(SettingsCategory::Appearance);
    const auto rebuild = [&] { window.buildSceneForTesting(renderer, 860, 620); };
    rebuild();

    std::vector<const ScrollView*> scrolls;
    collectNodes(window.m_contentScroll, scrolls);
    TEST_CHECK(scrolls.size() == 1);

    for (const auto& entry : kColorRoleTokens) {
      const auto* select = paletteRoleSelect(*window.m_contentScroll, entry.token);
      TEST_CHECK(select);
      const auto overrideColor = settings.themeColorOverride(entry.role);
      const auto expected =
          overrideColor ? colorSpecToConfigString(fixedColorSpec(*overrideColor)) : std::string(entry.token);
      TEST_CHECK(select->selectedText() == expected);
    }

    const auto primarySelect = [&] {
      auto* select = paletteRoleSelect(*window.m_contentScroll, "primary");
      TEST_CHECK(select);
      return select;
    };
    const auto activate = [&](const Node& scope, std::string_view label) {
      std::vector<const Button*> scopedButtons;
      collectNodes(&scope, scopedButtons);
      const auto it = std::ranges::find_if(scopedButtons, [label](const Button* button) {
        return button->label() && button->label()->text() == label;
      });
      TEST_CHECK(it != scopedButtons.end() && (*it)->enabled());
      window.m_input.setFocus((*it)->inputArea());
      window.m_input.keyEvent(XKB_KEY_space, 0, 0, true);
    };
    float initialX = 0.F, ignoredY = 0.F;
    Node::absolutePosition(primarySelect(), initialX, ignoredY);
    const auto selectCustom = [&] {
      auto* select = primarySelect();
      const auto original = std::string(select->selectedText());
      // An existing fixed colour adds a swatched value before Custom.
      select->setSelectedIndex(kColorRoleTokens.size() + (original.starts_with('#') ? 1U : 0U));
      TEST_CHECK(select->selectedText() == original);
    };
    selectCustom();
    TEST_CHECK(pickerInitial == colorForRole(ColorRole::Primary));
    TEST_CHECK(!settings.themeColorOverride(ColorRole::Primary));

    const auto secondary = std::ranges::find(kColorRoleTokens, ColorRole::Secondary, &ColorRoleToken::role);
    TEST_CHECK(secondary != kColorRoleTokens.end());
    const auto expected = colorForRole(ColorRole::Secondary);
    primarySelect()->setSelectedIndex(static_cast<std::size_t>(secondary - kColorRoleTokens.begin()));
    TEST_CHECK(!window.m_statusIsError);
    rebuild();
    auto* modified = primarySelect();
    float modifiedX = 0.F;
    Node::absolutePosition(modified, modifiedX, ignoredY);
    TEST_CHECK(modifiedX == initialX);
    const auto stored = settings.themeColorOverride(ColorRole::Primary);
    TEST_CHECK(stored && formatRgbaHex(*stored) == formatRgbaHex(expected));
    TEST_CHECK(modified->selectedText() == colorSpecToConfigString(fixedColorSpec(*stored)));
    selectCustom();
    TEST_CHECK(pickerInitial == *stored);

    const auto* roleRow = modified->parent()->parent();
    activate(*roleRow, "Reset");
    TEST_CHECK(!settings.themeColorOverride(ColorRole::Primary));
    TEST_CHECK(settings.themeColorOverride(ColorRole::Shadow).has_value());
    rebuild();
    TEST_CHECK(primarySelect()->selectedText() == "primary");
    activate(*window.m_contentScroll, "Reset all colours");
    TEST_CHECK(settings.themeColorOverrides().empty());

    FileManagerSettings reopened(shellConfig);
    TEST_CHECK(reopened.themeColorOverrides().empty());
    TEST_CHECK(reopened.themeFile() == themeFile);
    TEST_CHECK(!reopened.inheritShellTheme());
    TEST_CHECK(reopened.brandingMode() == BrandingMode::Compact);
  }

  static void runModifiedFilterTests() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-settings-filter"});
    ConfigService shellConfig(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shellConfig);
    WaylandConnection wayland;
    RenderContext renderContext;
    StubRenderer renderer;
    SettingsWindow window(wayland, renderContext, shellConfig, settings, {});
    KeybindMatcher::setMatcher(KeybindAction::Validate, [&](std::uint32_t sym, std::uint32_t mods) {
      return shellConfig.matchesKeybind(KeybindAction::Validate, sym, mods);
    });
    const auto restoreMatcher =
        nythera::test::makeScopeExit([] { KeybindMatcher::setMatcher(KeybindAction::Validate, {}); });
    const auto rebuild = [&] { window.buildSceneForTesting(renderer, 860, 620); };
    const auto labelVisible = [&](std::string_view text) {
      std::vector<const Label*> labels;
      collectNodes(window.m_contentScroll, labels);
      return std::ranges::any_of(labels, [text](const Label* label) { return label->text() == text; });
    };
    const auto toggleFilter = [&] {
      std::vector<const InputArea*> inputs;
      collectNodes(window.m_modifiedFilter, inputs);
      const auto it = std::ranges::find_if(inputs, [](const InputArea* input) {
        return input->tabFocusKey() == "preferences:modified-only";
      });
      TEST_CHECK(it != inputs.end());
      window.m_input.setFocus(const_cast<InputArea*>(*it));
      window.m_input.keyEvent(XKB_KEY_Return, 0, 0, true);
      rebuild();
    };

    rebuild();
    TEST_CHECK(!window.m_modifiedOnly);
    toggleFilter();
    TEST_CHECK(window.m_modifiedOnly);
    for (const auto& page : kSettingsCategories) {
      window.selectCategory(page.category);
      rebuild();
      TEST_CHECK(labelVisible("No modified preferences on this page."));
    }

    window.selectCategory(SettingsCategory::General);
    TEST_CHECK(window.mutatePreferences([](FileManagerSettings& value) {
      return value.setRestoreTabs(!value.restoreTabs());
    }));
    rebuild();
    TEST_CHECK(labelVisible("Startup"));
    TEST_CHECK(labelVisible("Restore tabs"));
    TEST_CHECK(!labelVisible("Startup location"));
    TEST_CHECK(!labelVisible("Closing"));

    TEST_CHECK(window.mutatePreferences([](FileManagerSettings& value) {
      return value.setThemeColorOverride(ColorRole::Secondary, hex("#247FC680"));
    }));
    window.selectCategory(SettingsCategory::Appearance);
    rebuild();
    TEST_CHECK(labelVisible("Colour overrides"));
    TEST_CHECK(labelVisible("Secondary"));
    TEST_CHECK(!labelVisible("Primary"));
    TEST_CHECK(!labelVisible("Theme"));
    std::vector<const Select*> selects;
    collectNodes(window.m_contentScroll, selects);
    TEST_CHECK(selects.size() == 1);
    TEST_CHECK(selects.front() == paletteRoleSelect(*window.m_contentScroll, "secondary"));
    window.resetCurrentPage();
    window.resetCurrentPage();
    rebuild();
    TEST_CHECK(labelVisible("No modified preferences on this page."));

    window.selectCategory(SettingsCategory::Search);
    TEST_CHECK(window.mutateSearchPreferences([&](FileManagerSettings& value) {
      return value.saveSearchExclusions({env.work()});
    }));
    rebuild();
    TEST_CHECK(labelVisible("Global Search Exclusions"));
    TEST_CHECK(!labelVisible("Custom Search Scopes"));
    TEST_CHECK(window.mutateSearchPreferences([](FileManagerSettings& value) {
      return value.saveSearchExclusions({});
    }));
    rebuild();
    TEST_CHECK(labelVisible("No modified preferences on this page."));
    toggleFilter();
    TEST_CHECK(!window.m_modifiedOnly);
    TEST_CHECK(labelVisible("Custom Search Scopes"));
  }

  static void runPaletteColourPreferenceTest() {
    const nythera::test::ScopedTestEnv env({.prefix = "files-palette-preference"});
    ConfigService shellConfig(ConfigService::SettingsAccess::InheritanceOnly);
    FileManagerSettings settings(shellConfig);
    const auto key = PreferencePath{"theme", "theme_file"};
    std::optional<ColorSpec> saved;
    std::function<void(Color)> apply;
    Color pickerInitial{};
    const auto inherited = colorSpecFromRole(ColorRole::Primary);
    const SettingsPageContext ctx{
        .settings = settings,
        .mutate = [&](auto fn) { return fn(settings); },
        .pickColor =
            [&](std::string, Color initial, auto callback) {
              pickerInitial = initial;
              apply = std::move(callback);
            },
    };
    const auto make = [&](Flex& card, std::optional<ColorSpec> overrideValue) {
      paletteColourPreference(
          card, ctx, "Example colour", {}, key, overrideValue, inherited, "Inherit",
          [&](FileManagerSettings&, ColorSpec color) {
            saved = color;
            return true;
          }
      );
      std::vector<const Select*> selects;
      collectNodes(&card, selects);
      TEST_CHECK(selects.size() == 1);
      return const_cast<Select*>(selects.front());
    };

    Flex base;
    auto* dropdown = make(base, std::nullopt);
    TEST_CHECK(dropdown->selectedText() == "Inherit");
    dropdown->setSelectedIndex(kColorRoleTokens.size() + 1);
    TEST_CHECK(dropdown->selectedText() == "Inherit");
    TEST_CHECK(pickerInitial == resolveColorSpec(inherited));
    TEST_CHECK(apply && !saved);
    apply(hex("#247FC680"));
    TEST_CHECK(saved && colorSpecToConfigString(*saved) == "#247FC680");

    Flex customized;
    dropdown = make(customized, saved);
    TEST_CHECK(dropdown->selectedText() == "#247FC680");
    dropdown->setSelectedIndex(kColorRoleTokens.size() + 2);
    TEST_CHECK(pickerInitial == hex("#247FC680"));
    dropdown->setSelectedIndex(1);
    TEST_CHECK(saved && saved->role == kColorRoleTokens.front().role);
    TEST_CHECK(colorSpecToConfigString(*saved) == kColorRoleTokens.front().token);
    TEST_CHECK(settings.setThemeFile(env.work() / "palette.json"));
    dropdown->setSelectedIndex(0);
    TEST_CHECK(!settings.hasPreferenceKey(key));
  }

  static void runHelpMenuRetentionTest() {
    const auto helpEntries = HelpDialogController::menuEntries();
    TEST_CHECK(helpEntries.size() == 6);
    TEST_CHECK(helpEntries[0].id == menu_id::kKeyboardShortcuts);
    TEST_CHECK(helpEntries[2].id == menu_id::kReportBug);
    TEST_CHECK(helpEntries[3].id == menu_id::kGitHub);
    TEST_CHECK(helpEntries[5].id == menu_id::kAboutFiles);
  }
};

int main() {
  FileManagerTestAccess::runWindowAndCategoryTests();
  FileManagerTestAccess::runResetWithIncludesAndSymlinksTest();
  FileManagerTestAccess::runColourEditorTests();
  FileManagerTestAccess::runModifiedFilterTests();
  FileManagerTestAccess::runPaletteColourPreferenceTest();
  FileManagerTestAccess::runHelpMenuRetentionTest();
  return 0;
}
