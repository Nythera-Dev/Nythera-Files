#include "filemanager/settings/settings_pages.h"
#include "filemanager/settings/settings_presentation.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/stepper.h"
#include "ui/style.h"

#include <algorithm>

namespace filemanager {

  void buildAdvancedSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    auto* adminCard = addPreferenceCard(parent, "Administrator Mode", "shield", ctx.scale);

    const int timeoutMinutes = std::clamp(ctx.settings.adminInactivityTimeoutSeconds() / 60, 1, 60);
    auto timeoutStepper = std::make_unique<Stepper>();
    timeoutStepper->setScale(ctx.scale);
    timeoutStepper->setRange(1, 60);
    timeoutStepper->setStep(1);
    timeoutStepper->setValueSuffix(" min");
    timeoutStepper->setValue(timeoutMinutes);
    timeoutStepper->setOnValueCommitted([mutate = ctx.mutate](int minutes) {
      const int seconds = std::clamp(minutes, 1, 60) * 60;
      mutate([seconds](FileManagerSettings& settings) { return settings.setAdminInactivityTimeoutSeconds(seconds); });
    });

    addPreferenceRow(
        *adminCard, ctx,
        {
            .title = "Inactivity timeout",
            .subtitle = "Automatically end an elevated Administrator Mode session after this period of inactivity (1 "
                        "– 60 minutes).",
            .control = std::move(timeoutStepper),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "admin_inactivity_timeout_seconds"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) {
                return settings.reset({"files", "admin_inactivity_timeout_seconds"});
              });
            },
        }
    );

    auto* cacheCard = addPreferenceCard(parent, "Shared Thumbnail Cache", "photo", ctx.scale);

    auto budgetStepper = std::make_unique<Stepper>();
    budgetStepper->setScale(ctx.scale);
    budgetStepper->setRange(16, 8192);
    budgetStepper->setStep(64);
    budgetStepper->setValueSuffix(" MiB");
    budgetStepper->setValue(ctx.settings.thumbnailCacheMiB());
    budgetStepper->setOnValueCommitted([mutate = ctx.mutate](int mib) {
      mutate([mib](FileManagerSettings& settings) { return settings.setThumbnailCacheMiB(mib); });
    });

    addPreferenceRow(
        *cacheCard, ctx,
        {
            .title = "Shared Freedesktop thumbnail cache budget",
            .subtitle = "Maximum size budget for the shared thumbnail cache used by Nythera Files and NytheraLib (16 "
                        "– 8192 MiB).",
            .control = std::move(budgetStepper),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "thumbnail_cache_mib"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "thumbnail_cache_mib"}); });
            },
        }
    );

    addPreferenceRow(
        *cacheCard, ctx,
        {
            .title = "Inspect or clear shared thumbnail cache",
            .subtitle = "Clearing removes thumbnails written by Nythera Files from the shared Freedesktop cache.",
            .control = ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale},
                ui::button({
                    .text = "Check Usage",
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Default,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick =
                        [inspect = ctx.inspectThumbnailCache] {
                          if (inspect)
                            inspect();
                        },
                }),
                ui::button({
                    .text = "Clear Cache…",
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Destructive,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick =
                        [clear = ctx.clearThumbnailCache] {
                          if (clear)
                            clear();
                        },
                })
            ),
            .scale = ctx.scale,
        }
    );
    if (!ctx.modifiedOnly && !ctx.thumbnailCacheUsage.empty()) {
      cacheCard->addChild(
          ui::label({
              .text = std::string(ctx.thumbnailCacheUsage),
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
  }

} // namespace filemanager
