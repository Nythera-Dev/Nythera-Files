#include "filemanager/settings/settings_pages.h"
#include "filemanager/settings/settings_presentation.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/toggle.h"

namespace filemanager {

  void buildIntegrationsSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    auto* archivesCard = addPreferenceCard(parent, "Archive Handling", "archive", ctx.scale);

    addPreferenceRow(
        *archivesCard, ctx,
        {
            .title = "Avoid redundant extraction folders",
            .subtitle = "When extracting an archive into a new folder, avoid creating an extra wrapper folder if the "
                        "archive already has a single top-level directory.",
            .control = ui::toggle({
                .checked = ctx.settings.avoidRedundantExtractionFolders(),
                .scale = ctx.scale,
                .onChange =
                    [mutate = ctx.mutate](bool checked) {
                      mutate([checked](FileManagerSettings& settings) {
                        return settings.setAvoidRedundantExtractionFolders(checked);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "avoid_redundant_extraction_folders"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) {
                return settings.reset({"files", "avoid_redundant_extraction_folders"});
              });
            },
        }
    );
  }

} // namespace filemanager
