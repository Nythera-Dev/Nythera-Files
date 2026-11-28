#include "filemanager/settings/preference_rows.h"

namespace filemanager {

  void buildGeneralSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    auto& startup = *addPreferenceCard(parent, "Startup", "power", ctx.scale);
    choicePreference(
        startup, ctx, "Startup location", "An explicit directory takes precedence.", {"files", "startup_location"},
        {"Home", "Last visited", "Custom directory"},
        {StartupLocation::Home, StartupLocation::Last, StartupLocation::Custom}, ctx.settings.startupLocation(),
        &FileManagerSettings::setStartupLocation
    );
    if (ctx.settings.startupLocation() == StartupLocation::Custom)
      directoryPreference(
          startup, ctx, {"files", "startup_custom_path"}, ctx.settings.startupCustomPath(),
          &FileManagerSettings::setStartupCustomPath
      );
    togglePreference(
        startup, ctx, "Restore tabs", "Restore directories from the last closed window.", {"files", "restore_tabs"},
        ctx.settings.restoreTabs(), &FileManagerSettings::setRestoreTabs
    );
    togglePreference(
        startup, ctx, "Remember window size", "Also restore the maximised state.",
        {"files", "remember_window_geometry"}, ctx.settings.rememberWindowGeometry(),
        &FileManagerSettings::setRememberWindowGeometry
    );
    choicePreference(
        startup, ctx, "Open external directories", {}, {"files", "external_open_mode"}, {"New tab", "New window"},
        {ExternalOpenMode::NewTab, ExternalOpenMode::NewWindow}, ctx.settings.externalOpenMode(),
        &FileManagerSettings::setExternalOpenMode
    );

    auto& closing = *addPreferenceCard(parent, "Closing", "door-exit", ctx.scale);
    togglePreference(
        closing, ctx, "Confirm closing multiple tabs", {}, {"files", "confirm_close_multiple_tabs"},
        ctx.settings.confirmCloseMultipleTabs(), &FileManagerSettings::setConfirmCloseMultipleTabs
    );
    togglePreference(
        closing, ctx, "Confirm closing during file operations",
        "Without confirmation, work continues in the background.", {"files", "confirm_close_active_operations"},
        ctx.settings.confirmCloseActiveOperations(), &FileManagerSettings::setConfirmCloseActiveOperations
    );
  }
} // namespace filemanager
