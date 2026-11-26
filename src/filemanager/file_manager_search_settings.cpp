#include "config/toml_persistence.h"
#include "core/log.h"
#include "filemanager/file_manager_settings.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace filemanager {
  namespace {
    namespace fs = std::filesystem;
    using nythera::config::persistTomlEdits;
    using nythera::config::StagedTomlFile;
    using nythera::config::TomlFileEdits;
    constexpr Logger kLog("files-search-config");

    std::optional<SearchLocations> readLocations(toml::node_view<const toml::node> node) {
      SearchLocations paths;
      if (!node)
        return paths;
      const auto* array = node.as_array();
      if (!array)
        return std::nullopt;
      for (const auto& item : *array) {
        auto path = item.value_exact<std::string>();
        if (!path || !validSearchLocation(*path))
          return std::nullopt;
        paths.emplace_back(std::move(*path));
      }
      return paths;
    }

    std::optional<SearchOptions> readOptions(const toml::table& table) {
      const auto hidden = table["show_hidden"];
      const auto links = table["follow_symlinks"];
      if ((hidden && !hidden.is_boolean()) || (links && !links.is_boolean()))
        return std::nullopt;
      return SearchOptions{hidden.value_or(false), links.value_or(false)};
    }

    std::optional<SearchSettings> parseSearchSettings(const toml::table& table) {
      if (std::ranges::any_of(table, [](const auto& item) {
            return item.first != "exclusions" && item.first != "profiles" && item.first != "options";
          }))
        return std::nullopt;
      auto global = readLocations(table["exclusions"]);
      if (!global)
        return std::nullopt;
      SearchSettings settings{.exclusions = std::move(*global)};
      if (const auto preferences = table["options"]) {
        const auto* options = preferences.as_table();
        if (!options || std::ranges::any_of(*options, [](const auto& item) {
              return item.first != "show_hidden" && item.first != "follow_symlinks";
            }))
          return std::nullopt;
        const auto parsed = readOptions(*options);
        if (!parsed)
          return std::nullopt;
        settings.standardOptions = *parsed;
      }
      const auto profiles = table["profiles"];
      if (!profiles)
        return settings;
      const auto* values = profiles.as_array();
      if (!values)
        return std::nullopt;
      std::unordered_set<std::string> names;
      for (const auto& item : *values) {
        const auto* profile = item.as_table();
        if (!profile || std::ranges::any_of(*profile, [](const auto& field) {
              return field.first != "name"
                  && field.first != "included"
                  && field.first != "excluded"
                  && field.first != "include_mounts"
                  && field.first != "show_hidden"
                  && field.first != "follow_symlinks";
            }))
          return std::nullopt;
        auto name = (*profile)["name"].value_exact<std::string>();
        auto included = readLocations((*profile)["included"]);
        auto excluded = readLocations((*profile)["excluded"]);
        const auto mounts = (*profile)["include_mounts"];
        const auto options = readOptions(*profile);
        if (!name
            || !validCustomSearchName(*name)
            || !names.insert(*name).second
            || !included
            || included->empty()
            || !excluded
            || !options
            || (mounts && !mounts.is_boolean()))
          return std::nullopt;
        const bool includeMounts = explicitlyIncludesRoot(*included) && mounts.value_or(false);
        settings.profiles.push_back(
            {std::move(*name), std::move(*included), std::move(*excluded), includeMounts, *options}
        );
      }
      return settings;
    }

    std::optional<SearchSettings> readSearchSettings(const fs::path& path) {
      std::error_code error;
      if (!fs::exists(path, error) && !error)
        return SearchSettings{};
      try {
        return parseSearchSettings(toml::parse_file(path.string()));
      } catch (const toml::parse_error&) {
        return std::nullopt;
      }
    }

    toml::array writeLocations(const SearchLocations& paths) {
      toml::array values;
      for (const auto& path : paths)
        values.push_back(path.native());
      return values;
    }

    template <typename T> std::string printToml(const T& value) {
      std::ostringstream output;
      output << value;
      return output.str();
    }

    template <typename T>
    bool saveSetting(
        const fs::path& path, std::string key, const T& value,
        const std::vector<CustomSearch>* expectedProfiles = nullptr
    ) {
      const TomlFileEdits edits{path, {{{std::move(key)}, printToml(value)}}};
      const auto result = persistTomlEdits(
          std::span(&edits, 1), [&](std::span<const StagedTomlFile> staged) -> std::optional<std::string> {
            if (expectedProfiles) {
              const auto current = readSearchSettings(path);
              if (!current || current->profiles != *expectedProfiles)
                return "Custom searches changed while saving";
            }
            if (staged.size() == 1 && parseSearchSettings(toml::parse(staged.front().content)))
              return std::nullopt;
            return "Invalid Files search settings";
          }
      );
      if (!result.ok())
        kLog.warn("Cannot save search settings: {}", result.error);
      return result.ok();
    }
  } // namespace

  fs::path FileManagerSettings::searchSettingsPath() const { return m_configDir / "search" / "config.toml"; }

  bool FileManagerSettings::reloadSearchSettings() {
    if (m_configDir.empty())
      return true;
    const auto path = searchSettingsPath();
    auto settings = readSearchSettings(path);
    if (!settings) {
      reportProblem({{path.string()}, "Invalid Files search settings"});
      return false;
    }
    if (*settings != m_searchSettings) {
      auto previous = std::exchange(m_searchSettings, std::move(*settings));
      if (m_onSearchReload)
        m_onSearchReload(previous);
    }
    return true;
  }

  bool FileManagerSettings::saveStandardSearchOptions(SearchOptions options) {
    toml::table fields{{"show_hidden", options.showHidden}, {"follow_symlinks", options.followSymlinks}};
    fields.is_inline(true);
    return !m_configDir.empty() && saveSetting(searchSettingsPath(), "options", fields) && reloadSearchSettings();
  }

  bool FileManagerSettings::saveSearchExclusions(const SearchLocations& exclusions) {
    return !m_configDir.empty()
        && std::ranges::all_of(exclusions, validSearchLocation)
        && saveSetting(searchSettingsPath(), "exclusions", writeLocations(exclusions))
        && reloadSearchSettings();
  }

  bool FileManagerSettings::editCustomSearch(
      const std::optional<CustomSearch>& previous, const std::optional<CustomSearch>& replacement
  ) {
    if (m_configDir.empty())
      return false;
    const auto path = searchSettingsPath();
    auto settings = readSearchSettings(path);
    if (!settings || (!previous && !replacement))
      return false;
    auto profiles = settings->profiles;
    auto position = profiles.end();
    if (previous) {
      position = std::ranges::find(profiles, previous->name, &CustomSearch::name);
      if (position == profiles.end() || *position != *previous)
        return false;
    }
    if (replacement) {
      if (const auto other = std::ranges::find(profiles, replacement->name, &CustomSearch::name);
          other != profiles.end() && other != position)
        return false;
      if (position == profiles.end())
        profiles.push_back(*replacement);
      else
        *position = *replacement;
    } else {
      profiles.erase(position);
    }
    toml::array values;
    for (const auto& profile : profiles) {
      toml::table value{
          {"name", profile.name},
          {"included", writeLocations(profile.included)},
          {"excluded", writeLocations(profile.excluded)},
          {"include_mounts", explicitlyIncludesRoot(profile.included) && profile.includeMounts},
          {"show_hidden", profile.options.showHidden},
          {"follow_symlinks", profile.options.followSymlinks}
      };
      value.is_inline(true);
      values.push_back(std::move(value));
    }
    return saveSetting(path, "profiles", values, &settings->profiles) && reloadSearchSettings();
  }

} // namespace filemanager
