#pragma once

#include "filemanager/settings/settings_pages.h"
#include "filemanager/settings/settings_presentation.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"

#include <algorithm>

namespace filemanager {
  using PreferencePath = nythera::config::ConfigKeyPath;

  inline void preferenceRow(
      Flex& card, const SettingsPageContext& ctx, std::string title, std::string subtitle, const PreferencePath& key,
      std::unique_ptr<Node> control, bool enabled = true, float labelIndent = 0.F
  ) {
    const bool modified = ctx.settings.hasPreferenceKey(key);
    addPreferenceRow(
        card, ctx,
        {
            .title = std::move(title),
            .subtitle = std::move(subtitle),
            .control = std::move(control),
            .scale = ctx.scale,
            .showReset = enabled && modified,
            .onReset = [mutate = ctx.mutate,
                        key] { mutate([key](FileManagerSettings& settings) { return settings.reset(key); }); },
        },
        labelIndent, modified
    );
  }

  inline void togglePreference(
      Flex& card, const SettingsPageContext& ctx, std::string title, std::string subtitle, const PreferencePath& key,
      bool checked, bool (FileManagerSettings::*set)(bool), bool enabled = true
  ) {
    preferenceRow(
        card, ctx, std::move(title), std::move(subtitle), key,
        ui::toggle({
            .checked = checked,
            .enabled = enabled,
            .scale = ctx.scale,
            .onChange = [mutate = ctx.mutate, set](
                            bool value
                        ) { mutate([set, value](FileManagerSettings& settings) { return (settings.*set)(value); }); },
        }),
        enabled
    );
  }

  template <typename Value>
  void choicePreference(
      Flex& card, const SettingsPageContext& ctx, std::string title, std::string subtitle, const PreferencePath& key,
      std::vector<std::string> labels, std::vector<Value> values, Value current,
      bool (FileManagerSettings::*set)(Value), bool enabled = true
  ) {
    const auto currentIndex = std::ranges::find(values, current) - values.begin();
    preferenceRow(
        card, ctx, std::move(title), std::move(subtitle), key,
        ui::select({
            .options = std::move(labels),
            .selectedIndex = static_cast<std::size_t>(currentIndex),
            .fontSize = Style::fontSizeCaption * ctx.scale,
            .controlHeight = Style::controlHeightSm * ctx.scale,
            .enabled = enabled,
            .width = 195.F * ctx.scale,
            .onSelectionChanged =
                [mutate = ctx.mutate, set, values = std::move(values)](std::size_t i, std::string_view) {
                  const auto value = values.at(i);
                  mutate([set, value](FileManagerSettings& settings) { return (settings.*set)(value); });
                },
        }),
        enabled
    );
  }

  inline void directoryPreference(
      Flex& card, const SettingsPageContext& ctx, const PreferencePath& key, const std::filesystem::path& current,
      bool (FileManagerSettings::*set)(const std::filesystem::path&)
  ) {
    auto control = std::make_unique<Flex>();
    control->setGap(Style::spaceXs * ctx.scale);
    control->addChild(
        ui::input({
            .value = current.string(),
            .placeholder = "Absolute path",
            .width = 160.F * ctx.scale,
            .onSubmit = [mutate = ctx.mutate, set](const std::string& value) {
              mutate([set, value](FileManagerSettings& settings) { return (settings.*set)(value); });
            },
        })
    );
    control->addChild(
        ui::button({
            .glyph = "folder-open",
            .tooltip = "Choose directory",
            .onClick = [pick = ctx.chooseFile, mutate = ctx.mutate, current, set] {
              pick(
                  {.mode = FileDialogMode::SelectFolder, .startDirectory = current, .title = "Choose directory"},
                  [mutate, set](const auto& path) {
                    mutate([set, path](FileManagerSettings& settings) { return (settings.*set)(path); });
                  }
              );
            },
        })
    );
    preferenceRow(card, ctx, "Custom directory", {}, key, std::move(control));
  }
} // namespace filemanager
