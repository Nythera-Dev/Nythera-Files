#pragma once

#include "filemanager/file_manager_settings.h"
#include "ui/dialogs/file_dialog.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

class Flex;

namespace filemanager {

  struct SettingsPageContext {
    FileManagerSettings& settings;
    float scale = 1.0F;
    bool modifiedOnly = false;
    // Executes a preference mutation against FileManagerSettings. On success,
    // FileManager applies live changes and the settings page rebuilds; on failure,
    // an error banner is shown and controls revert to the unchanged settings state.
    std::function<bool(std::function<bool(FileManagerSettings&)>)> mutate;
    // Executes a search/config.toml mutation against FileManagerSettings.
    std::function<bool(std::function<bool(FileManagerSettings&)>)> mutateSearch;
    std::function<void()> manageCustomSearches;
    std::function<void(std::optional<CustomSearch>)> editCustomSearch;
    std::function<void()> inspectThumbnailCache;
    std::string_view thumbnailCacheUsage;
    std::function<void()> clearThumbnailCache;
    std::function<void(FileDialogOptions, std::function<void(const std::filesystem::path&)>)> chooseFile;
    std::function<bool(const std::filesystem::path&, std::string&)> exportTheme;
    std::function<void(std::string, Color, std::function<void(Color)>)> pickColor;
    std::function<bool()> clearRecentHistory;
    std::function<void(std::string, bool)> showStatus;
  };

  void buildGeneralSettingsPage(Flex& parent, const SettingsPageContext& ctx);
  void buildAppearanceSettingsPage(Flex& parent, const SettingsPageContext& ctx);
  void buildNavigationSettingsPage(Flex& parent, const SettingsPageContext& ctx);
  void addViewScalePreference(Flex& card, const SettingsPageContext& ctx);
  void buildFilesFoldersSettingsPage(Flex& parent, const SettingsPageContext& ctx);
  void buildSearchSettingsPage(Flex& parent, const SettingsPageContext& ctx);
  void buildIntegrationsSettingsPage(Flex& parent, const SettingsPageContext& ctx);
  void buildShortcutsSettingsPage(Flex& parent, const SettingsPageContext& ctx);
  void buildAdvancedSettingsPage(Flex& parent, const SettingsPageContext& ctx);

} // namespace filemanager
