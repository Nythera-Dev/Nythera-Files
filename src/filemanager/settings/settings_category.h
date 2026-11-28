#pragma once

#include "config/config_sources.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace filemanager {

  enum class SettingsCategory : std::uint8_t {
    General,
    Appearance,
    Navigation,
    FilesAndFolders,
    Search,
    Integrations,
    Shortcuts,
    Advanced,
  };

  struct SettingsCategoryDescriptor {
    SettingsCategory category;
    std::string_view id;
    std::string_view label;
    std::string_view glyph;
  };

  inline constexpr std::array<SettingsCategoryDescriptor, 8> kSettingsCategories{{
      {SettingsCategory::General, "general", "General", "settings"},
      {SettingsCategory::Appearance, "appearance", "Appearance", "palette"},
      {SettingsCategory::Navigation, "navigation", "Navigation", "compass"},
      {SettingsCategory::FilesAndFolders, "files_and_folders", "Files & Folders", "folder"},
      {SettingsCategory::Search, "search", "Search", "search"},
      {SettingsCategory::Integrations, "integrations", "Integrations", "plug"},
      {SettingsCategory::Shortcuts, "shortcuts", "Shortcuts", "keyboard"},
      {SettingsCategory::Advanced, "advanced", "Advanced", "adjustments"},
  }};

  [[nodiscard]] inline const SettingsCategoryDescriptor& categoryDescriptor(SettingsCategory category) noexcept {
    const auto index = static_cast<std::size_t>(category);
    return index < kSettingsCategories.size() ? kSettingsCategories[index] : kSettingsCategories.front();
  }

  [[nodiscard]] inline std::span<const nythera::config::ConfigKeyPath>
  categoryPreferenceKeys(SettingsCategory category) noexcept {
    using nythera::config::ConfigKeyPath;
    switch (category) {
    case SettingsCategory::General: {
      static const std::array<ConfigKeyPath, 7> keys{{
          {"files", "startup_location"},
          {"files", "startup_custom_path"},
          {"files", "restore_tabs"},
          {"files", "remember_window_geometry"},
          {"files", "external_open_mode"},
          {"files", "confirm_close_multiple_tabs"},
          {"files", "confirm_close_active_operations"},
      }};
      return keys;
    }
    case SettingsCategory::Appearance: {
      static const std::array<ConfigKeyPath, 12> keys{{
          {"theme", "inherit_shell"},
          {"theme", "transparency_mode"},
          {"theme", "theme_file"},
          {"theme", "colors"},
          {"files", "font_family"},
          {"files", "icon_theme"},
          {"files", "animations_enabled"},
          {"files", "animation_speed"},
          {"files", "haze_enabled"},
          {"files", "branding_mode"},
          {"files", "show_menu_bar"},
          {"files", "view_scale"},
      }};
      return keys;
    }
    case SettingsCategory::Navigation: {
      static const std::array<ConfigKeyPath, 17> keys{{
          {"files", "always_show_tab_bar"},
          {"files", "open_tabs_in_background"},
          {"files", "new_tab_location"},
          {"files", "new_tab_custom_path"},
          {"files", "new_tab_position"},
          {"files", "middle_click_folder"},
          {"files", "click_activation"},
          {"files", "address_bar_mode"},
          {"files", "show_places_personal"},
          {"files", "show_places_devices"},
          {"files", "show_places_network"},
          {"files", "show_places_capacity"},
          {"files", "recent_activity_enabled"},
          {"files", "show_recent_sidebar"},
          {"files", "show_frequent_locations"},
          {"files", "frequent_locations_count"},
          {"files", "recent_retention"},
      }};
      return keys;
    }
    case SettingsCategory::FilesAndFolders: {
      static const std::array<ConfigKeyPath, 11> keys{{
          {"files", "grid_view"},
          {"files", "sort_field"},
          {"files", "sort_descending"},
          {"files", "folders_first"},
          {"files", "hidden_last"},
          {"files", "show_hidden"},
          {"files", "view_scale"},
          {"files", "list_column_layout"},
          {"files", "list_column_widths"},
          {"files", "preview_mode"},
          {"files", "thumbnail_policy"},
      }};
      return keys;
    }
    case SettingsCategory::Search: {
      static const std::array<ConfigKeyPath, 8> keys{{
          {"files", "fzf_search"},
          {"files", "search_default_mode"},
          {"files", "search_grid_view"},
          {"files", "search_sort_field"},
          {"files", "search_sort_descending"},
          {"files", "search_view_scale"},
          {"files", "search_column_layout"},
          {"files", "search_column_widths"},
      }};
      return keys;
    }
    case SettingsCategory::Integrations: {
      static const std::array<ConfigKeyPath, 1> keys{{
          {"files", "avoid_redundant_extraction_folders"},
      }};
      return keys;
    }
    case SettingsCategory::Shortcuts: {
      static const std::array<ConfigKeyPath, 1> keys{{
          {"files", "readline_shortcuts"},
      }};
      return keys;
    }
    case SettingsCategory::Advanced: {
      static const std::array<ConfigKeyPath, 2> keys{{
          {"files", "admin_inactivity_timeout_seconds"},
          {"files", "thumbnail_cache_mib"},
      }};
      return keys;
    }
    }
    return {};
  }

  // Returns the preference keys managed by the Settings interface (explicitly
  // excluding saved_places, place_order, hidden_mounts, and runtime state).
  // Note: thumbnail_max_file_mib is also included in Files & Folders reset.
  [[nodiscard]] inline std::vector<nythera::config::ConfigKeyPath> categoryResettableKeys(SettingsCategory category) {
    std::vector<nythera::config::ConfigKeyPath> keys;
    const auto base = categoryPreferenceKeys(category);
    keys.assign(base.begin(), base.end());
    if (category == SettingsCategory::FilesAndFolders) {
      keys.push_back({"files", "thumbnail_max_file_mib"});
    } else if (category == SettingsCategory::Appearance) {
      keys.push_back({"files", "view_scale"});
    }
    return keys;
  }

  [[nodiscard]] inline std::vector<nythera::config::ConfigKeyPath> allResettablePreferenceKeys() {
    std::vector<nythera::config::ConfigKeyPath> all;
    for (const auto& descriptor : kSettingsCategories) {
      auto keys = categoryResettableKeys(descriptor.category);
      all.insert(all.end(), keys.begin(), keys.end());
    }
    return all;
  }

} // namespace filemanager
