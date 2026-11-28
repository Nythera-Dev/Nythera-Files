#include "config/config_service.h"
#include "filemanager/settings/palette_colour_preference.h"
#include "render/text/font_family_catalog.h"
#include "system/icon_resolver.h"
#include "theme/fixed_palette_json.h"

#include <algorithm>
#include <charconv>
#include <format>

namespace filemanager {
  namespace {
    using StringOverride = bool (FileManagerSettings::*)(std::optional<std::string>);

    struct PaletteGroup {
      std::string_view prefix;
      std::string_view title;
    };

    constexpr PaletteGroup kPaletteGroups[]{
        {"primary", "Primary"}, {"secondary", "Secondary"}, {"tertiary", "Tertiary"},
        {"error", "Error"},     {"surface", "Surfaces"},    {"", "Interaction and effects"},
    };

    const PaletteGroup& paletteGroup(std::string_view token) {
      if (token.starts_with("on_"))
        token.remove_prefix(3);
      const auto family = token.substr(0, token.find('_'));
      return *std::ranges::find_if(kPaletteGroups, [family](const PaletteGroup& group) {
        return group.prefix == family || group.prefix.empty();
      });
    }

    void addColourRole(Flex& group, const SettingsPageContext& ctx, const ColorRoleToken& entry) {
      std::string label(entry.token);
      std::ranges::replace(label, '_', ' ');
      label.front() = static_cast<char>(label.front() - 'a' + 'A');

      const auto role = entry.role;
      const auto overrideColor = ctx.settings.themeColorOverride(role);
      paletteColourPreference(
          group, ctx, std::move(label), {}, {"theme", "colors", std::string(entry.token)},
          overrideColor ? std::optional(fixedColorSpec(*overrideColor)) : std::nullopt, colorSpecFromRole(role), {},
          [role](FileManagerSettings& settings, ColorSpec spec) {
            return settings.setThemeColorOverride(role, resolveColorSpec(spec));
          },
          12.F
      );
    }

    void catalogPreference(
        Flex& card, const SettingsPageContext& ctx, std::string title, PreferencePath key,
        std::vector<std::string> options, const std::optional<std::string>& overrideValue, StringOverride set
    ) {
      options.insert(options.begin(), "Inherit / default");
      auto selected = options.begin();
      if (overrideValue) {
        selected = std::find(options.begin() + 1, options.end(), *overrideValue);
        if (selected == options.end()) {
          options.push_back(*overrideValue);
          selected = options.end() - 1;
        }
      }
      const auto index = static_cast<std::size_t>(selected - options.begin());
      preferenceRow(
          card, ctx, std::move(title), {}, key,
          ui::select({
              .options = std::move(options),
              .selectedIndex = index,
              .width = 195.F * ctx.scale,
              .onSelectionChanged = [mutate = ctx.mutate, set](std::size_t i, std::string_view value) {
                auto next = i == 0 ? std::nullopt : std::optional(std::string(value));
                mutate([set, next = std::move(next)](FileManagerSettings& settings) { return (settings.*set)(next); });
              },
          })
      );
    }
  } // namespace

  void buildAppearanceSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    auto& theme = *addPreferenceCard(parent, "Theme", "palette", ctx.scale);
    togglePreference(
        theme, ctx, "Inherit Nythera Shell appearance", "Files overrides apply on top.", {"theme", "inherit_shell"},
        ctx.settings.inheritShellTheme(), &FileManagerSettings::setInheritShellTheme
    );
    const auto currentFile = ctx.settings.themeFile();
    auto themeActions = std::make_unique<Flex>();
    themeActions->setGap(Style::spaceXs * ctx.scale);
    themeActions->addChild(
        ui::button({
            .text = "Import…",
            .onClick = [choose = ctx.chooseFile, mutate = ctx.mutate, status = ctx.showStatus] {
              choose({.extensions = {"json"}, .title = "Import Files theme"}, [mutate, status](const auto& path) {
                if (!nythera::theme::parseUiPaletteFile(path)) {
                  status("The selected file is not a supported Nythera palette", true);
                  return;
                }
                mutate([path](FileManagerSettings& settings) { return settings.setThemeFile(path); });
              });
            },
        })
    );
    themeActions->addChild(
        ui::button({
            .text = "Export…",
            .onClick = [choose = ctx.chooseFile, save = ctx.exportTheme, status = ctx.showStatus] {
              choose(
                  {.mode = FileDialogMode::Save,
                   .extensions = {"json"},
                   .defaultFilename = "Nythera-Files.json",
                   .title = "Export Files theme"},
                  [save, status](const auto& path) {
                    std::string error;
                    const bool ok = save && save(path, error);
                    status(ok ? "Theme exported" : error.empty() ? "Could not export the theme" : error, !ok);
                  }
              );
            },
        })
    );
    preferenceRow(
        theme, ctx, "Custom theme file",
        currentFile.empty() ? "Use a Nythera palette JSON file." : currentFile.filename().string(),
        {"theme", "theme_file"}, std::move(themeActions)
    );
    using Transparency = std::optional<PanelTransparencyMode>;
    choicePreference(
        theme, ctx, "Transparency", {}, {"theme", "transparency_mode"}, {"Inherit / default", "Solid", "Soft", "Glass"},
        {Transparency{}, Transparency(PanelTransparencyMode::Solid), Transparency(PanelTransparencyMode::Soft),
         Transparency(PanelTransparencyMode::Glass)},
        ctx.settings.transparencyModeOverride(), &FileManagerSettings::setTransparencyMode
    );

    auto& colors = *addPreferenceCard(parent, "Colour overrides", "color-swatch", ctx.scale);
    const bool hasColourOverrides = !ctx.settings.themeColorOverrides().empty();
    if (!ctx.modifiedOnly || hasColourOverrides)
      colors.addChild(
          ui::row(
              {.justify = FlexJustify::End},
              ui::button({
                  .text = "Reset all colours",
                  .enabled = hasColourOverrides,
                  .onClick = [mutate = ctx.mutate] {
                    mutate([](FileManagerSettings& settings) { return settings.clearAllThemeColorOverrides(); });
                  },
              })
          )
      );

    bool firstVisibleGroup = true;
    for (const auto& group : kPaletteGroups) {
      if (ctx.modifiedOnly && !std::ranges::any_of(kColorRoleTokens, [&](const ColorRoleToken& entry) {
            return &paletteGroup(entry.token) == &group
                && ctx.settings.hasPreferenceKey({"theme", "colors", std::string(entry.token)});
          }))
        continue;
      // Moving the divider below the heading relocates its old height and card gap.
      // Keep that space above later groups, plus one extra standard gap.
      const float topInset =
          firstVisibleGroup ? 0.F : (2.F * Style::spaceSm + 2.F * Style::spaceXs + Style::borderWidth) * ctx.scale;
      colors.addChild(
          ui::column(
              {.align = FlexAlign::Stretch,
               .gap = Style::spaceXs * ctx.scale,
               .configure = [topInset](Flex& heading) { heading.setPadding(topInset, 0.F, 0.F, 0.F); }},
              ui::label({
                  .text = std::string(group.title),
                  .fontSize = kPreferenceCardTitleSize * ctx.scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
              }),
              ui::separator()
          )
      );
      firstVisibleGroup = false;
      for (const auto& entry : kColorRoleTokens) {
        if (&paletteGroup(entry.token) == &group
            && (!ctx.modifiedOnly || ctx.settings.hasPreferenceKey({"theme", "colors", std::string(entry.token)})))
          addColourRole(colors, ctx, entry);
      }
    }

    auto& interface = *addPreferenceCard(parent, "Interface", "layout", ctx.scale);
    catalogPreference(
        interface, ctx, "Font family", {"files", "font_family"}, nythera::text::discoverFontFamilies(),
        ctx.settings.fontFamilyOverride(), &FileManagerSettings::setFontFamilyOverride
    );
    catalogPreference(
        interface, ctx, "Icon theme", {"files", "icon_theme"}, IconResolver::availableThemes(),
        ctx.settings.iconThemeOverride(), &FileManagerSettings::setIconThemeOverride
    );
    choicePreference(
        interface, ctx, "Branding", {}, {"files", "branding_mode"}, {"Full wordmark", "Compact (N)", "Off"},
        {BrandingMode::Full, BrandingMode::Compact, BrandingMode::Off}, ctx.settings.brandingMode(),
        &FileManagerSettings::setBrandingMode
    );
    togglePreference(
        interface, ctx, "Show menu bar", {}, {"files", "show_menu_bar"}, ctx.settings.showApplicationMenuBar(),
        &FileManagerSettings::setShowApplicationMenuBar
    );

    addViewScalePreference(interface, ctx);

    auto& effects = *addPreferenceCard(parent, "Effects", "sparkles", ctx.scale);
    using Override = std::optional<bool>;
    choicePreference(
        effects, ctx, "Animations", {}, {"files", "animations_enabled"}, {"Inherit / default", "On", "Off"},
        {Override{}, Override(true), Override(false)}, ctx.settings.animationsEnabledOverride(),
        &FileManagerSettings::setAnimationsEnabledOverride
    );
    preferenceRow(
        effects, ctx, "Animation speed", "0.25–4×. Press Enter to save.", {"files", "animation_speed"},
        ui::input({
            .value = ctx.settings.animationSpeedOverride()
                ? std::format("{:.2f}", *ctx.settings.animationSpeedOverride())
                : std::string{},
            .placeholder = std::format("Default ({:.2f}×)", ctx.settings.effectiveAnimationSpeed()),
            .width = 155.F,
            .onSubmit = [mutate = ctx.mutate, status = ctx.showStatus](const std::string& text) {
              if (text.empty()) {
                mutate([](FileManagerSettings& settings) { return settings.setAnimationSpeedOverride({}); });
                return;
              }
              float value = 0.F;
              const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
              if (error != std::errc{} || end != text.data() + text.size() || !(value >= .25F && value <= 4.F)) {
                status("Enter a speed from 0.25 to 4", true);
                return;
              }
              mutate([value](FileManagerSettings& settings) { return settings.setAnimationSpeedOverride(value); });
            },
        })
    );
    choicePreference(
        effects, ctx, "Nythera Haze", "Applies to translucent surfaces.", {"files", "haze_enabled"},
        {"Inherit / default", "On", "Off"}, {Override{}, Override(true), Override(false)},
        ctx.settings.hazeEnabledOverride(), &FileManagerSettings::setHazeEnabledOverride
    );
  }
} // namespace filemanager
