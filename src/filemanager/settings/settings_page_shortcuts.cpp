#include "config/config_service.h"
#include "filemanager/settings/settings_pages.h"
#include "filemanager/settings/settings_presentation.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/style.h"

#include <optional>
#include <string>
#include <vector>

namespace filemanager {

  void buildShortcutsSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    auto* editingCard = addPreferenceCard(parent, "Text Editing Shortcuts", "keyboard", ctx.scale);

    const auto override = ctx.settings.readlineShortcutsOverride();
    const bool shellDefault = ctx.settings.shellConfig().config().shell.readlineShortcuts;
    std::vector<std::string> options{
        std::string("Inherit from Shell (") + (shellDefault ? "enabled" : "disabled") + ")",
        "Enabled",
        "Disabled",
    };
    const std::size_t selectedIndex = !override ? 0 : (*override ? 1 : 2);

    addPreferenceRow(
        *editingCard, ctx,
        {
            .title = "Readline shortcuts in text inputs",
            .subtitle = "Enable Emacs/Readline editing keys (Ctrl+A, Ctrl+E, Ctrl+K, Ctrl+W) in path and search "
                        "inputs.",
            .control = ui::select({
                .options = std::move(options),
                .selectedIndex = selectedIndex,
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 220.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate](std::size_t index, std::string_view) {
                      std::optional<bool> next;
                      if (index == 1)
                        next = true;
                      else if (index == 2)
                        next = false;
                      mutate([next](FileManagerSettings& settings) { return settings.setReadlineShortcuts(next); });
                    },
            }),
            .scale = ctx.scale,
            .showReset = override.has_value(),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "readline_shortcuts"}); });
            },
        }
    );

    if (ctx.modifiedOnly)
      return;

    auto* infoCard = addPreferenceCard(parent, "Application Shortcuts", "keyboard", ctx.scale);
    infoCard->addChild(
        ui::label({
            .text = "Interactive keyboard shortcut customization is planned for a later stage. Use Help → Keyboard "
                    "Shortcuts to view the current bindings.",
            .fontSize = Style::fontSizeCaption * ctx.scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 0,
        })
    );
  }

} // namespace filemanager
