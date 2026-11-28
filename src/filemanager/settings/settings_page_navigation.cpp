#include "filemanager/settings/preference_rows.h"

namespace filemanager {

  void buildNavigationSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    auto& tabs = *addPreferenceCard(parent, "Tabs", "browser", ctx.scale);
    togglePreference(
        tabs, ctx, "Always show tab bar", {}, {"files", "always_show_tab_bar"}, ctx.settings.alwaysShowTabBar(),
        &FileManagerSettings::setAlwaysShowTabBar
    );
    togglePreference(
        tabs, ctx, "Open new tabs in background", {}, {"files", "open_tabs_in_background"},
        ctx.settings.openTabsInBackground(), &FileManagerSettings::setOpenTabsInBackground
    );
    choicePreference(
        tabs, ctx, "New tab location", {}, {"files", "new_tab_location"},
        {"Current directory", "Home", "Custom directory", "Follow startup preference"},
        {NewTabLocation::Current, NewTabLocation::Home, NewTabLocation::Custom, NewTabLocation::Startup},
        ctx.settings.newTabLocation(), &FileManagerSettings::setNewTabLocation
    );
    if (ctx.settings.newTabLocation() == NewTabLocation::Custom)
      directoryPreference(
          tabs, ctx, {"files", "new_tab_custom_path"}, ctx.settings.newTabCustomPath(),
          &FileManagerSettings::setNewTabCustomPath
      );
    choicePreference(
        tabs, ctx, "New tab position", {}, {"files", "new_tab_position"}, {"At the end", "After the current tab"},
        {NewTabPosition::End, NewTabPosition::AfterCurrent}, ctx.settings.newTabPosition(),
        &FileManagerSettings::setNewTabPosition
    );
    choicePreference(
        tabs, ctx, "Middle-click a folder", {}, {"files", "middle_click_folder"},
        {"Background tab", "Foreground tab", "New window", "Do nothing"},
        {MiddleClickFolderAction::BackgroundTab, MiddleClickFolderAction::ForegroundTab,
         MiddleClickFolderAction::NewWindow, MiddleClickFolderAction::None},
        ctx.settings.middleClickFolder(), &FileManagerSettings::setMiddleClickFolder
    );

    auto& navigation = *addPreferenceCard(parent, "Navigation", "compass", ctx.scale);
    choicePreference(
        navigation, ctx, "Open items with", {}, {"files", "click_activation"}, {"Double click", "Single click"},
        {ClickActivation::Double, ClickActivation::Single}, ctx.settings.clickActivation(),
        &FileManagerSettings::setClickActivation
    );
    choicePreference(
        navigation, ctx, "Address bar", "Press Tab while editing a path to complete it.", {"files", "address_bar_mode"},
        {"Breadcrumbs", "Full path"}, {AddressBarMode::Breadcrumbs, AddressBarMode::FullPath},
        ctx.settings.addressBarMode(), &FileManagerSettings::setAddressBarMode
    );

    auto& places = *addPreferenceCard(parent, "Places", "map-pin", ctx.scale);
    togglePreference(
        places, ctx, "Show Personal", {}, {"files", "show_places_personal"}, ctx.settings.showPlacesPersonal(),
        &FileManagerSettings::setShowPlacesPersonal
    );
    togglePreference(
        places, ctx, "Show Devices", {}, {"files", "show_places_devices"}, ctx.settings.showPlacesDevices(),
        &FileManagerSettings::setShowPlacesDevices
    );
    togglePreference(
        places, ctx, "Show Network", {}, {"files", "show_places_network"}, ctx.settings.showPlacesNetwork(),
        &FileManagerSettings::setShowPlacesNetwork
    );
    togglePreference(
        places, ctx, "Show capacity indicators", {}, {"files", "show_places_capacity"},
        ctx.settings.showPlacesCapacity(), &FileManagerSettings::setShowPlacesCapacity
    );

    auto& recent = *addPreferenceCard(parent, "Recent activity", "history", ctx.scale);
    const bool recording = ctx.settings.recentActivityEnabled();
    togglePreference(
        recent, ctx, "Record recent activity", "Administrator Mode activity is excluded.",
        {"files", "recent_activity_enabled"}, recording, &FileManagerSettings::setRecentActivityEnabled
    );
    togglePreference(
        recent, ctx, "Show Recent in Places", {}, {"files", "show_recent_sidebar"}, ctx.settings.showRecentSidebar(),
        &FileManagerSettings::setShowRecentSidebar, recording
    );
    togglePreference(
        recent, ctx, "Show frequent locations", {}, {"files", "show_frequent_locations"},
        ctx.settings.showFrequentLocations(), &FileManagerSettings::setShowFrequentLocations, recording
    );
    std::vector<int> sizes;
    std::vector<std::string> sizeLabels;
    for (int i = 1; i <= 20; ++i) {
      sizes.push_back(i);
      sizeLabels.push_back(std::to_string(i));
    }
    choicePreference(
        recent, ctx, "Frequent locations to show", {}, {"files", "frequent_locations_count"}, std::move(sizeLabels),
        std::move(sizes), ctx.settings.frequentLocationsCount(), &FileManagerSettings::setFrequentLocationsCount,
        recording && ctx.settings.showFrequentLocations()
    );
    choicePreference(
        recent, ctx, "Keep history for", {}, {"files", "recent_retention"},
        {"7 days", "30 days", "90 days", "Indefinitely"},
        {RecentRetention::Days7, RecentRetention::Days30, RecentRetention::Days90, RecentRetention::Indefinite},
        ctx.settings.recentRetention(), &FileManagerSettings::setRecentRetention, recording
    );
    addPreferenceRow(
        recent, ctx,
        {
            .title = "Clear history",
            .subtitle = "Remove recent files and visit counts. Pins and exclusions are kept.",
            .control = ui::button(
                {.text = "Clear history",
                 .onClick =
                     [clear = ctx.clearRecentHistory, status = ctx.showStatus] {
                       const bool ok = clear && clear();
                       status(ok ? "Recent history cleared" : "Could not clear recent history", !ok);
                     }}
            ),
            .scale = ctx.scale,
        }
    );
  }
} // namespace filemanager
