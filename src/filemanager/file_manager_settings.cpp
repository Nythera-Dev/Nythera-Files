#include "filemanager/file_manager_settings.h"

#include "config/application_paths.h"
#include "config/config_service.h"
#include "config/files_config_validator.h"
#include "config/toml_persistence.h"
#include "core/log.h"
#include "filemanager/file_column_serialization.h"
#include "filemanager/runtime_file_lock.h"
#include "render/core/color.h"
#include "ui/palette.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>

namespace filemanager {

  namespace {
    namespace fs = std::filesystem;
    using namespace nythera::config;

    constexpr Logger kLog("files-config");
    constexpr std::string_view kOwner = "files";

    fs::path statePath() {
      const auto dir = FileUtils::applicationStateDir(FileUtils::Application::Files);
      return dir.empty() ? fs::path{} : fs::path(dir) / "state.toml";
    }

    template <typename T> std::string literal(T value) {
      std::ostringstream stream;
      stream << toml::value<T>(std::move(value));
      return stream.str();
    }

    std::string literal(toml::array array) {
      std::ostringstream stream;
      stream << array;
      return stream.str();
    }

    std::string arrayLiteral(const std::vector<std::string>& values) {
      toml::array array;
      for (const auto& value : values)
        array.push_back(value);
      return literal(std::move(array));
    }

    float loadClampedFloat(
        const StateStore& state, std::string_view key, float defaultValue, float minValue, float maxValue
    ) {
      if (const auto stored = state.stringValue(kOwner, key)) {
        try {
          const float value = std::stof(*stored);
          if (std::isfinite(value))
            return std::clamp(value, minValue, maxValue);
        } catch (const std::exception&) {
        }
      }
      return defaultValue;
    }

    std::vector<std::string> readStrings(toml::node_view<const toml::node> node) {
      std::vector<std::string> result;
      if (const auto* values = node.as_array()) {
        for (const auto& value : *values)
          result.push_back(*value.value_exact<std::string>());
      }
      return result;
    }

    const toml::node* find(const toml::table& table, const ConfigKeyPath& path) {
      const toml::node* node = &table;
      for (const auto& key : path) {
        const auto* parent = node->as_table();
        if (!parent || !(node = parent->get(key)))
          return nullptr;
      }
      return node;
    }

    bool equal(const toml::node* left, const toml::node* right) {
      return toml::node_view(left) == toml::node_view(right);
    }

    const toml::node* preferenceDefault(const ConfigKeyPath& path) {
      // Optional Shell overrides have no stored default. An explicit value can
      // match Shell today and still override future changes.
      static const auto defaults = [] {
        auto values = toml::parse(R"(
          [files]
          fzf_search = true
          search_default_mode = "contiguous"
          search_grid_view = false
          grid_view = false
          show_hidden = false
          folders_first = true
          hidden_last = true
          search_sort_field = 0
          sort_field = 0
          search_sort_descending = false
          sort_descending = false
          show_menu_bar = true
          always_show_tab_bar = false
          open_tabs_in_background = false
          admin_inactivity_timeout_seconds = 600
          view_scale = 1.0
          search_view_scale = 1.0
          preview_mode = "full"
          avoid_redundant_extraction_folders = true
          thumbnail_policy = "local"
          thumbnail_max_file_mib = 64
          thumbnail_cache_mib = 1024
          startup_location = "home"
          startup_custom_path = ""
          restore_tabs = false
          remember_window_geometry = true
          external_open_mode = "new_tab"
          confirm_close_multiple_tabs = true
          confirm_close_active_operations = true
          branding_mode = "full"
          new_tab_location = "home"
          new_tab_custom_path = ""
          new_tab_position = "end"
          middle_click_folder = "background_tab"
          click_activation = "double"
          address_bar_mode = "breadcrumbs"
          show_places_personal = true
          show_places_devices = true
          show_places_network = true
          show_places_capacity = true
          recent_activity_enabled = true
          show_recent_sidebar = true
          show_frequent_locations = true
          frequent_locations_count = 5
          recent_retention = "30d"

          [theme]
          inherit_shell = true
          theme_file = ""
        )");
        auto& files = *values["files"].as_table();
        files.insert("list_column_layout", serializeFileColumnLayout(FileColumnLayout{}));
        files.insert("search_column_layout", serializeSearchFileColumnLayout(kDefaultSearchColumnLayout));
        return values;
      }();
      return find(defaults, path);
    }

    bool matchesDefault(const toml::node& value, const toml::node& defaultValue) {
      if (defaultValue.is_floating_point() && value.is_number())
        return value.value<double>() == defaultValue.value<double>();
      return equal(&value, &defaultValue);
    }

    std::optional<PanelTransparencyMode> transparency(toml::node_view<const toml::node> node) {
      const auto name = node.value_exact<std::string_view>();
      for (const auto& option : kPanelTransparencyModes) {
        if (name && *name == option.key)
          return option.value;
      }
      return std::nullopt;
    }
  } // namespace

  FileManagerSettings::FileManagerSettings(const ConfigService& shellConfig)
      : FileManagerSettings(shellConfig, FileUtils::applicationConfigDir(FileUtils::Application::Files), statePath()) {}

  FileManagerSettings::FileManagerSettings(
      const ConfigService& shellConfig, fs::path configDir, fs::path runtimeStatePath
  )
      : m_shellConfig(shellConfig), m_configDir(configDir.empty() ? fs::path{} : fs::absolute(configDir)),
        m_state(std::move(runtimeStatePath)) {
    m_state.load();
    if (!m_configDir.empty()) {
      m_monitor.emplace(
          nythera::config::ApplicationConfigMonitor::Options{
              .appId = std::string(kOwner),
              .displayName = "Files",
              .configDir = m_configDir,
              .validator = validateFilesConfiguration,
              .additionalWatchFiles = {searchSettingsPath()},
          }
      );
      m_monitor->setProblemCallback([this](const ConfigurationProblem& problem, const fs::path&) {
        reportProblem(problem);
      });
      m_monitor->setReloadCallback([this](const SourcedMergeResult& loaded, bool manualReload) {
        accept(loaded, true);
        if (reloadSearchSettings() && manualReload && m_onManualReload)
          m_onManualReload();
      });
    }
    reload();
  }

  bool FileManagerSettings::readlineShortcuts() const {
    return m_preferences["files"]["readline_shortcuts"].value_or(m_shellConfig.config().shell.readlineShortcuts);
  }

  std::optional<bool> FileManagerSettings::readlineShortcutsOverride() const {
    return m_preferences["files"]["readline_shortcuts"].value_exact<bool>();
  }

  bool FileManagerSettings::setReadlineShortcuts(std::optional<bool> enabled) {
    return save({{{"files", "readline_shortcuts"}, enabled ? std::optional(literal(*enabled)) : std::nullopt}});
  }

  PanelTransparencyMode FileManagerSettings::transparencyMode() const {
    return transparency(m_preferences["theme"]["transparency_mode"])
        .value_or(
            inheritShellTheme() ? m_shellConfig.config().shell.panel.transparencyMode : PanelTransparencyMode::Solid
        );
  }

  std::optional<PanelTransparencyMode> FileManagerSettings::transparencyModeOverride() const {
    return transparency(m_preferences["theme"]["transparency_mode"]);
  }

  bool FileManagerSettings::setTransparencyMode(std::optional<PanelTransparencyMode> mode) {
    if (!mode) {
      return reset({"theme", "transparency_mode"});
    }
    for (const auto& option : kPanelTransparencyModes) {
      if (option.value == *mode) {
        return save({{{"theme", "transparency_mode"}, literal(std::string(option.key))}});
      }
    }
    return false;
  }

  bool FileManagerSettings::hasPreferenceKey(const ConfigKeyPath& path) const {
    const auto* value = find(m_preferences, path);
    const auto* defaultValue = preferenceDefault(path);
    return value && (!defaultValue || !matchesDefault(*value, *defaultValue));
  }

  void FileManagerSettings::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_monitor)
      m_monitor->addPollFds(fds);
  }

  void FileManagerSettings::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (m_monitor)
      m_monitor->dispatch(fds, startIdx);
  }

  void FileManagerSettings::recordSelfWrite(const fs::path& path, std::string content) {
    if (m_monitor)
      m_monitor->recordInProcessSelfWrite(path, std::move(content));
  }

  void FileManagerSettings::accept(SourcedMergeResult loaded, bool notify) {
    m_problem.reset();
    if (loaded.config.merged == m_preferences)
      return;
    auto previous = std::exchange(m_preferences, std::move(loaded.config.merged));
    if (notify && m_onReload)
      m_onReload(previous);
  }

  void FileManagerSettings::setProblemCallback(ProblemCallback callback) {
    m_onProblem = std::move(callback);
    if (m_problem && m_onProblem)
      m_onProblem(*m_problem, m_configDir);
  }

  void FileManagerSettings::reportProblem(ConfigurationProblem problem) {
    if (m_problem == problem)
      return;
    m_problem = std::move(problem);
    if (m_onProblem)
      m_onProblem(*m_problem, m_configDir);
  }

  bool FileManagerSettings::reload() {
    const bool ok = m_monitor ? m_monitor->reload(/*notifyReload=*/false) : true;
    const bool searchOk = reloadSearchSettings();
    return ok && searchOk;
  }

  bool FileManagerSettings::save(std::vector<Change> changes) {
    m_lastSaveError.clear();
    if (changes.empty())
      return true;
    if (m_configDir.empty()) {
      m_lastSaveError = "Configuration directory is unavailable";
      return false;
    }
    auto current = mergeConfigWithSources(m_configDir.string());
    if (auto error = validateFilesConfiguration(current)) {
      m_lastSaveError = error->origin.prefixed(error->message);
      kLog.warn("Cannot save Files configuration: {}", m_lastSaveError);
      reportProblem(std::move(*error));
      return false;
    }
    const bool hadExternalChanges = current.config.merged != m_preferences;

    std::vector<TomlFileEdits> files;
    const auto append = [&](const fs::path& file, const Change& change) {
      const auto found = std::ranges::find(files, file, &TomlFileEdits::path);
      if (found == files.end())
        files.push_back({file, {{change.first, change.second}}});
      else
        found->edits.push_back({change.first, change.second});
    };
    for (auto& change : changes) {
      auto& [key, value] = change;
      if (key.size() < 2 || (key.front() != "files" && key.front() != "theme")) {
        m_lastSaveError = "Invalid configuration key path";
        return false;
      }
      if (value) {
        if (const auto* defaultValue = preferenceDefault(key)) {
          const auto proposed = toml::parse("value = " + *value);
          if (const auto* node = proposed.get("value"); node && matchesDefault(*node, *defaultValue))
            value.reset();
        }
      }
      if (!value) {
        for (const auto& source : current.sources.definitions(key))
          append(source.origin.file, change);
        continue;
      }
      if (const auto* definition = current.sources.winner(key)) {
        append(definition->origin.file, change);
        continue;
      }
      fs::path destination = m_configDir / (key.front() == "theme" ? "theme.toml" : "config.toml");
      ConfigKeyPath parent(key.begin(), key.end() - 1);
      for (; !parent.empty(); parent.pop_back()) {
        const auto* source = current.sources.winner(parent);
        if (source && source->kind == ConfigDefinitionKind::Table) {
          destination = source->origin.file;
          break;
        }
      }
      append(destination, change);
    }

    if (files.empty()) {
      accept(std::move(current), hadExternalChanges);
      return true;
    }

    std::optional<SourcedMergeResult> candidate;
    std::map<fs::path, std::string> expectedWrites;
    const auto result =
        persistTomlEdits(files, [&](std::span<const StagedTomlFile> staged) -> std::optional<std::string> {
          candidate = mergeConfigWithSources(m_configDir.string(), staged);
          if (auto error = validateFilesConfiguration(*candidate))
            return error->origin.prefixed(error->message);
          for (const auto& [path, value] : changes) {
            const auto* actual = find(candidate->config.merged, path);
            if (!value && !actual)
              continue;
            if (value) {
              const auto expected = toml::parse("value = " + *value);
              if (equal(actual, expected.get("value")))
                continue;
            }
            return std::format("The edit to '{}' would not take effect in the merged configuration", path.back());
          }
          expectedWrites.clear();
          std::vector<RecordedConfigWrite> recorded;
          recorded.reserve(staged.size() * 2);
          for (const auto& file : staged) {
            expectedWrites.emplace(file.path, file.content);
            recorded.push_back({file.path, file.content});
            for (const auto& alias : file.aliases) {
              recorded.push_back({alias, file.content});
            }
          }
          (void)recordValidatedApplicationConfigWrites(kOwner, recorded, m_configDir, *candidate);
          return std::nullopt;
        });
    if (!result.ok()) {
      clearApplicationConfigWrites(kOwner);
      m_lastSaveError = result.error;
      kLog.warn("Cannot save Files configuration: {}", result.error);
      for (const auto& path : result.rollbackFailures)
        kLog.error("Could not restore '{}'", path.string());
      return false;
    }
    for (auto& [path, content] : expectedWrites)
      recordSelfWrite(path, std::move(content));
    accept(candidate ? std::move(*candidate) : std::move(current), hadExternalChanges);
    return true;
  }

  bool FileManagerSettings::reset(const ConfigKeyPath& path) {
    if (path.size() == 2 && path[0] == "theme" && path[1] == "colors") {
      return clearAllThemeColorOverrides();
    }
    return save({{path, std::nullopt}});
  }

  bool FileManagerSettings::resetKeys(std::span<const ConfigKeyPath> paths) {
    std::vector<Change> changes;
    changes.reserve(paths.size());
    for (const auto& path : paths) {
      if (path.size() == 2 && path[0] == "theme" && path[1] == "colors") {
        if (const auto* colors = m_preferences["theme"]["colors"].as_table()) {
          for (const auto& [key, _] : *colors) {
            changes.emplace_back(ConfigKeyPath{"theme", "colors", std::string(key.str())}, std::nullopt);
          }
        }
      }
      changes.emplace_back(path, std::nullopt);
    }
    return save(std::move(changes));
  }

  bool FileManagerSettings::fzfSearchEnabled() const { return m_preferences["files"]["fzf_search"].value_or(true); }

  bool FileManagerSettings::setFzfSearchEnabled(bool enabled) {
    return save({{{"files", "fzf_search"}, literal(enabled)}});
  }

  SearchMode FileManagerSettings::searchDefaultMode() const {
    return searchModeFromConfig(m_preferences["files"]["search_default_mode"].value_or(std::string_view{}));
  }

  bool FileManagerSettings::setSearchDefaultMode(SearchMode mode) {
    return save({{{"files", "search_default_mode"}, literal(std::string(searchModeToConfig(mode)))}});
  }

  FileViewSettings FileManagerSettings::loadFileView(bool search) const {
    const auto files = m_preferences["files"];
    return {
        .gridMode = files[search ? "search_grid_view" : "grid_view"].value_or(false),
        .showHidden = files["show_hidden"].value_or(false),
        .foldersFirst = files["folders_first"].value_or(true),
        .hiddenLast = files["hidden_last"].value_or(true),
        .sortField = static_cast<FileDialogSortField>(
            files[search ? "search_sort_field" : "sort_field"].value_or(std::int64_t{0})
        ),
        .sortOrder = files[search ? "search_sort_descending" : "sort_descending"].value_or(false)
            ? FileDialogSortOrder::Descending
            : FileDialogSortOrder::Ascending,
    };
  }

  bool
  FileManagerSettings::saveFileView(const FileViewSettings& settings, const FileViewSettings& previous, bool search) {
    std::vector<Change> changes;
    const auto add = [&]<typename T>(std::string key, T next, T old) {
      if (next != old)
        changes.emplace_back(ConfigKeyPath{"files", std::move(key)}, literal(next));
    };
    add(search ? "search_grid_view" : "grid_view", settings.gridMode, previous.gridMode);
    add("show_hidden", settings.showHidden, previous.showHidden);
    add("folders_first", settings.foldersFirst, previous.foldersFirst);
    add("hidden_last", settings.hiddenLast, previous.hiddenLast);
    add(search ? "search_sort_field" : "sort_field", static_cast<std::int64_t>(settings.sortField),
        static_cast<std::int64_t>(previous.sortField));
    add(search ? "search_sort_descending" : "sort_descending", settings.sortOrder == FileDialogSortOrder::Descending,
        previous.sortOrder == FileDialogSortOrder::Descending);
    return save(std::move(changes));
  }

  bool FileManagerSettings::showApplicationMenuBar() const {
    return m_preferences["files"]["show_menu_bar"].value_or(true);
  }

  bool FileManagerSettings::setShowApplicationMenuBar(bool show) {
    return save({{{"files", "show_menu_bar"}, literal(show)}});
  }

  bool FileManagerSettings::alwaysShowTabBar() const {
    return m_preferences["files"]["always_show_tab_bar"].value_or(false);
  }

  bool FileManagerSettings::setAlwaysShowTabBar(bool show) {
    return save({{{"files", "always_show_tab_bar"}, literal(show)}});
  }

  bool FileManagerSettings::openTabsInBackground() const {
    return m_preferences["files"]["open_tabs_in_background"].value_or(false);
  }

  bool FileManagerSettings::setOpenTabsInBackground(bool background) {
    return save({{{"files", "open_tabs_in_background"}, literal(background)}});
  }

  int FileManagerSettings::adminInactivityTimeoutSeconds() const {
    return static_cast<int>(m_preferences["files"]["admin_inactivity_timeout_seconds"].value_or(std::int64_t{600}));
  }

  bool FileManagerSettings::setAdminInactivityTimeoutSeconds(int seconds) {
    if (seconds < 60 || seconds > 3600)
      return false;
    return save({{{"files", "admin_inactivity_timeout_seconds"}, literal(static_cast<std::int64_t>(seconds))}});
  }

  void FileManagerSettings::loadListColumnWidths(FileColumns& out, bool search) const {
    if (const auto widths = m_preferences["files"][search ? "search_column_widths" : "list_column_widths"]
                                .value_exact<std::string_view>())
      parseFileColumns(*widths, out);
  }

  bool FileManagerSettings::saveListColumnWidths(const FileColumns& columns, bool search) {
    return save(
        {{{"files", search ? "search_column_widths" : "list_column_widths"}, literal(serializeFileColumns(columns))}}
    );
  }

  void FileManagerSettings::loadListColumnLayout(FileColumnLayout& out, bool search) const {
    const auto columns =
        m_preferences["files"][search ? "search_column_layout" : "list_column_layout"].value_exact<std::string_view>();
    if (!columns)
      return;
    if (search)
      parseSearchFileColumnLayout(*columns, out);
    else
      parseFileColumnLayout(*columns, out);
  }

  bool FileManagerSettings::saveListColumnLayout(const FileColumnLayout& layout, bool search) {
    const auto text = search ? serializeSearchFileColumnLayout(layout) : serializeFileColumnLayout(layout);
    return save({{{"files", search ? "search_column_layout" : "list_column_layout"}, literal(text)}});
  }

  float FileManagerSettings::loadViewScale(float defaultScale, bool search) const {
    return static_cast<float>(
        m_preferences["files"][search ? "search_view_scale" : "view_scale"].value_or(static_cast<double>(defaultScale))
    );
  }

  bool FileManagerSettings::saveViewScale(float scale, bool search) {
    return save({{{"files", search ? "search_view_scale" : "view_scale"}, std::format("{:.6f}", scale)}});
  }

  PreviewMode FileManagerSettings::previewMode() const {
    const auto mode = m_preferences["files"]["preview_mode"].value_or<std::string_view>("full");
    if (mode == "hidden")
      return PreviewMode::Hidden;
    if (mode == "information")
      return PreviewMode::Information;
    return PreviewMode::Full;
  }

  bool FileManagerSettings::setPreviewMode(PreviewMode mode) {
    const std::string_view value = mode == PreviewMode::Information ? "information"
        : mode == PreviewMode::Hidden                               ? "hidden"
                                                                    : "full";
    return save({{{"files", "preview_mode"}, literal(std::string(value))}});
  }

  bool FileManagerSettings::avoidRedundantExtractionFolders() const {
    return m_preferences["files"]["avoid_redundant_extraction_folders"].value_or(true);
  }

  bool FileManagerSettings::setAvoidRedundantExtractionFolders(bool enabled) {
    return save({{{"files", "avoid_redundant_extraction_folders"}, literal(enabled)}});
  }

  ThumbnailPolicy FileManagerSettings::thumbnailPolicy() const {
    const auto policy = m_preferences["files"]["thumbnail_policy"].value_or<std::string_view>("local");
    if (policy == "never")
      return ThumbnailPolicy::Never;
    if (policy == "all")
      return ThumbnailPolicy::All;
    return ThumbnailPolicy::Local;
  }

  bool FileManagerSettings::setThumbnailPolicy(ThumbnailPolicy policy) {
    const std::string value = policy == ThumbnailPolicy::Never ? "never"
        : policy == ThumbnailPolicy::All                       ? "all"
                                                               : "local";
    return save({{{"files", "thumbnail_policy"}, literal(value)}});
  }

  int FileManagerSettings::thumbnailMaxFileMiB() const {
    return m_preferences["files"]["thumbnail_max_file_mib"].value_or(64);
  }

  bool FileManagerSettings::setThumbnailMaxFileMiB(int mib) {
    if (mib < 1 || mib > 64) {
      m_lastSaveError = "Thumbnail max file size must be between 1 and 64 MiB";
      return false;
    }
    return save({{{"files", "thumbnail_max_file_mib"}, literal(static_cast<std::int64_t>(mib))}});
  }

  int FileManagerSettings::thumbnailCacheMiB() const {
    return m_preferences["files"]["thumbnail_cache_mib"].value_or(1024);
  }

  bool FileManagerSettings::setThumbnailCacheMiB(int mib) {
    if (mib < 16 || mib > 8192) {
      m_lastSaveError = "Thumbnail cache budget must be between 16 and 8192 MiB";
      return false;
    }
    return save({{{"files", "thumbnail_cache_mib"}, literal(static_cast<std::int64_t>(mib))}});
  }

  float FileManagerSettings::loadPreviewWidth() const {
    return loadClampedFloat(m_state, "preview_width", 320.0F, 208.0F, 800.0F);
  }

  bool FileManagerSettings::savePreviewWidth(float width) {
    return updateRuntimeState([&] { return m_state.setString(kOwner, "preview_width", std::to_string(width)); });
  }

  float FileManagerSettings::loadSidebarWidth(float defaultWidth, float minWidth, float maxWidth) const {
    return loadClampedFloat(m_state, "sidebar_width", defaultWidth, minWidth, maxWidth);
  }

  bool FileManagerSettings::saveSidebarWidth(float width) {
    return updateRuntimeState([&] { return m_state.setString(kOwner, "sidebar_width", std::to_string(width)); });
  }

  float FileManagerSettings::loadStatusHeight(float defaultHeight, float minHeight, float maxHeight) const {
    return loadClampedFloat(m_state, "status_height", defaultHeight, minHeight, maxHeight);
  }

  bool FileManagerSettings::saveStatusHeight(float height) {
    return updateRuntimeState([&] { return m_state.setString(kOwner, "status_height", std::to_string(height)); });
  }

  std::vector<SavedPlace> FileManagerSettings::loadSavedPlaces() const {
    std::vector<SavedPlace> result;
    const auto* places = m_preferences["files"]["saved_places"].as_table();
    if (!places)
      return result;
    for (const auto& [id, place] : *places) {
      const auto* record = place.as_table();
      result.push_back(
          {std::string(id.str()), *(*record)["path"].value_exact<std::string>(),
           *(*record)["label"].value_exact<std::string>()}
      );
    }
    std::ranges::stable_sort(result, {}, [&](const auto& place) {
      return (*places)[place.id]["order"].value_or(std::int64_t{0});
    });
    return result;
  }

  bool
  FileManagerSettings::saveSavedPlaces(const std::vector<SavedPlace>& places, const std::vector<std::string>* order) {
    std::vector<Change> changes;
    const auto previous = loadSavedPlaces();
    for (const auto& place : previous) {
      if (std::ranges::find(places, place.id, &SavedPlace::id) == places.end())
        changes.push_back({{"files", "saved_places", place.id}, std::nullopt});
    }
    std::int64_t lastOrder = 0;
    if (const auto* stored = m_preferences["files"]["saved_places"].as_table()) {
      for (const auto& [id, entry] : *stored) {
        lastOrder = std::max(lastOrder, (*entry.as_table())["order"].value_or(std::int64_t{0}));
      }
    }
    for (const auto& place : places) {
      const auto old = std::ranges::find(previous, place.id, &SavedPlace::id);
      if (old == previous.end()) {
        if (lastOrder == std::numeric_limits<std::int64_t>::max())
          return false;
        changes.push_back({{"files", "saved_places", place.id, "order"}, literal(++lastOrder)});
      }
      if (old == previous.end() || old->path != place.path)
        changes.push_back({{"files", "saved_places", place.id, "path"}, literal(place.path.string())});
      if (old == previous.end() || old->label != place.label)
        changes.push_back({{"files", "saved_places", place.id, "label"}, literal(place.label)});
    }
    if (order)
      changes.push_back({{"files", "place_order"}, arrayLiteral(*order)});
    return save(std::move(changes));
  }

  std::vector<std::string> FileManagerSettings::loadPlaceOrder() const {
    return readStrings(m_preferences["files"]["place_order"]);
  }

  bool FileManagerSettings::savePlaceOrder(const std::vector<std::string>& order) {
    return save({{{"files", "place_order"}, arrayLiteral(order)}});
  }

  std::vector<std::string> FileManagerSettings::loadHiddenMounts() const {
    return readStrings(m_preferences["files"]["hidden_mounts"]);
  }

  bool FileManagerSettings::saveHiddenMounts(const std::vector<std::string>& identities) {
    return save({{{"files", "hidden_mounts"}, arrayLiteral(identities)}});
  }

  bool FileManagerSettings::placesSectionExpanded(std::string_view key) const {
    return m_state.boolValue(kOwner, key).value_or(true);
  }

  bool FileManagerSettings::setPlacesSectionExpanded(std::string_view key, bool expanded) {
    return updateRuntimeState([&] { return m_state.setBool(kOwner, key, expanded); });
  }

  std::string FileManagerSettings::dialogSizeStateKey(std::string_view title, bool conflictDialog) {
    if (conflictDialog)
      return "conflict_dialog_size";
    std::string key = "dialog_size_";
    bool separator = false;
    for (const unsigned char character : title) {
      if ((character >= 'A' && character <= 'Z')
          || (character >= 'a' && character <= 'z')
          || (character >= '0' && character <= '9')) {
        const auto normalized = character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        key.push_back(static_cast<char>(normalized));
        separator = false;
      } else if (!separator && key.back() != '_') {
        key.push_back('_');
        separator = true;
      }
    }
    while (!key.empty() && key.back() == '_')
      key.pop_back();
    return key;
  }

  std::optional<std::pair<std::uint32_t, std::uint32_t>>
  FileManagerSettings::loadWindowSize(std::string_view key, std::uint32_t minWidth, std::uint32_t minHeight) const {
    const auto saved = m_state.stringValue(kOwner, key);
    if (!saved)
      return std::nullopt;
    const auto separator = saved->find('x');
    if (separator == std::string::npos)
      return std::nullopt;
    std::uint32_t savedWidth = 0;
    std::uint32_t savedHeight = 0;
    const auto [widthEnd, widthError] = std::from_chars(saved->data(), saved->data() + separator, savedWidth);
    const auto [heightEnd, heightError] =
        std::from_chars(saved->data() + separator + 1, saved->data() + saved->size(), savedHeight);
    if (widthError == std::errc{}
        && widthEnd == saved->data() + separator
        && heightError == std::errc{}
        && heightEnd == saved->data() + saved->size()
        && savedWidth >= minWidth
        && savedHeight >= minHeight) {
      return std::pair{savedWidth, savedHeight};
    }
    return std::nullopt;
  }

  bool FileManagerSettings::saveDialogSize(std::string_view key, std::uint32_t width, std::uint32_t height) {
    const auto size = std::to_string(width) + "x" + std::to_string(height);
    return updateRuntimeState([&] { return m_state.setString(kOwner, key, size); });
  }

  std::optional<std::pair<std::uint32_t, std::uint32_t>>
  FileManagerSettings::loadDialogSize(std::string_view key) const {
    return loadWindowSize(key, 360, 180);
  }

  std::optional<std::pair<std::uint32_t, std::uint32_t>> FileManagerSettings::loadPropertiesSize() const {
    return loadWindowSize("properties_window_size", 440, 400);
  }

  bool FileManagerSettings::savePropertiesSize(std::uint32_t width, std::uint32_t height) {
    return saveDialogSize("properties_window_size", width, height);
  }

  std::optional<std::pair<std::uint32_t, std::uint32_t>> FileManagerSettings::loadSettingsWindowSize() const {
    return loadWindowSize("settings_window_size", 640, 420);
  }

  bool FileManagerSettings::saveSettingsWindowSize(std::uint32_t width, std::uint32_t height) {
    return saveDialogSize("settings_window_size", width, height);
  }

  // Stage 2: General preferences

  StartupLocation FileManagerSettings::startupLocation() const {
    const auto value = m_preferences["files"]["startup_location"].value_or<std::string_view>("home");
    if (value == "last")
      return StartupLocation::Last;
    if (value == "custom")
      return StartupLocation::Custom;
    return StartupLocation::Home;
  }

  bool FileManagerSettings::setStartupLocation(StartupLocation location) {
    const std::string_view key = location == StartupLocation::Last ? "last"
        : location == StartupLocation::Custom                      ? "custom"
                                                                   : "home";
    return save({{{"files", "startup_location"}, literal(std::string(key))}});
  }

  fs::path FileManagerSettings::startupCustomPath() const {
    return fs::path(m_preferences["files"]["startup_custom_path"].value_or<std::string_view>(""));
  }

  bool FileManagerSettings::setStartupCustomPath(const fs::path& path) {
    return save({{{"files", "startup_custom_path"}, literal(path.string())}});
  }

  bool FileManagerSettings::restoreTabs() const { return m_preferences["files"]["restore_tabs"].value_or(false); }

  bool FileManagerSettings::setRestoreTabs(bool restore) {
    return save({{{"files", "restore_tabs"}, literal(restore)}});
  }

  bool FileManagerSettings::rememberWindowGeometry() const {
    return m_preferences["files"]["remember_window_geometry"].value_or(true);
  }

  bool FileManagerSettings::setRememberWindowGeometry(bool remember) {
    return save({{{"files", "remember_window_geometry"}, literal(remember)}});
  }

  ExternalOpenMode FileManagerSettings::externalOpenMode() const {
    const auto value = m_preferences["files"]["external_open_mode"].value_or<std::string_view>("new_tab");
    return value == "new_window" ? ExternalOpenMode::NewWindow : ExternalOpenMode::NewTab;
  }

  bool FileManagerSettings::setExternalOpenMode(ExternalOpenMode mode) {
    return save(
        {{{"files", "external_open_mode"},
          literal(std::string(mode == ExternalOpenMode::NewWindow ? "new_window" : "new_tab"))}}
    );
  }

  bool FileManagerSettings::confirmCloseMultipleTabs() const {
    return m_preferences["files"]["confirm_close_multiple_tabs"].value_or(true);
  }

  bool FileManagerSettings::setConfirmCloseMultipleTabs(bool confirm) {
    return save({{{"files", "confirm_close_multiple_tabs"}, literal(confirm)}});
  }

  bool FileManagerSettings::confirmCloseActiveOperations() const {
    return m_preferences["files"]["confirm_close_active_operations"].value_or(true);
  }

  bool FileManagerSettings::setConfirmCloseActiveOperations(bool confirm) {
    return save({{{"files", "confirm_close_active_operations"}, literal(confirm)}});
  }

  // Stage 2: Appearance preferences

  bool FileManagerSettings::inheritShellTheme() const { return m_preferences["theme"]["inherit_shell"].value_or(true); }

  bool FileManagerSettings::setInheritShellTheme(bool inherit) {
    return save({{{"theme", "inherit_shell"}, literal(inherit)}});
  }

  fs::path FileManagerSettings::themeFile() const {
    return fs::path(m_preferences["theme"]["theme_file"].value_or<std::string_view>(""));
  }

  bool FileManagerSettings::setThemeFile(const fs::path& path) {
    if (path.empty())
      return reset({"theme", "theme_file"});
    return save({{{"theme", "theme_file"}, literal(path.string())}});
  }

  std::map<ColorRole, Color> FileManagerSettings::themeColorOverrides() const {
    std::map<ColorRole, Color> overrides;
    const auto* colors = m_preferences["theme"]["colors"].as_table();
    if (colors == nullptr)
      return overrides;
    for (const auto& [key, value] : *colors) {
      const auto role = colorRoleFromToken(key.str());
      const auto hexText = value.value_exact<std::string_view>();
      if (!role || !hexText)
        continue;
      Color parsed;
      if (tryParseHexColor(*hexText, parsed))
        overrides.emplace(*role, parsed);
    }
    return overrides;
  }

  std::optional<Color> FileManagerSettings::themeColorOverride(ColorRole role) const {
    const auto hexText = m_preferences["theme"]["colors"][colorRoleToken(role)].value_exact<std::string_view>();
    if (!hexText)
      return std::nullopt;
    Color parsed;
    if (tryParseHexColor(*hexText, parsed))
      return parsed;
    return std::nullopt;
  }

  bool FileManagerSettings::setThemeColorOverride(ColorRole role, std::optional<Color> color) {
    const ConfigKeyPath key{"theme", "colors", std::string(colorRoleToken(role))};
    if (!color)
      return save({{key, std::nullopt}});
    const std::string hexText = color->a < 0.999F ? formatRgbaHex(*color) : formatRgbHex(*color);
    return save({{key, literal(hexText)}});
  }

  bool FileManagerSettings::clearAllThemeColorOverrides() {
    std::vector<Change> changes;
    if (const auto* colors = m_preferences["theme"]["colors"].as_table()) {
      for (const auto& [key, _] : *colors)
        changes.emplace_back(ConfigKeyPath{"theme", "colors", std::string(key.str())}, std::nullopt);
    }
    changes.emplace_back(ConfigKeyPath{"theme", "colors"}, std::nullopt);
    return save(std::move(changes));
  }

  std::optional<std::string> FileManagerSettings::fontFamilyOverride() const {
    if (const auto value = m_preferences["files"]["font_family"].value_exact<std::string_view>()) {
      const auto trimmed = StringUtils::trim(*value);
      if (!trimmed.empty())
        return trimmed;
    }
    return std::nullopt;
  }

  std::string FileManagerSettings::effectiveFontFamily() const {
    if (auto ovr = fontFamilyOverride())
      return *ovr;
    if (inheritShellTheme() && !m_shellConfig.config().shell.fontFamily.empty())
      return m_shellConfig.config().shell.fontFamily;
    return "sans-serif";
  }

  bool FileManagerSettings::setFontFamilyOverride(std::optional<std::string> family) {
    if (!family || StringUtils::trim(*family).empty())
      return reset({"files", "font_family"});
    return save({{{"files", "font_family"}, literal(StringUtils::trim(*family))}});
  }

  std::optional<std::string> FileManagerSettings::iconThemeOverride() const {
    if (const auto value = m_preferences["files"]["icon_theme"].value_exact<std::string_view>()) {
      const auto trimmed = StringUtils::trim(*value);
      if (!trimmed.empty())
        return trimmed;
    }
    return std::nullopt;
  }

  bool FileManagerSettings::setIconThemeOverride(std::optional<std::string> theme) {
    if (!theme || StringUtils::trim(*theme).empty())
      return reset({"files", "icon_theme"});
    return save({{{"files", "icon_theme"}, literal(StringUtils::trim(*theme))}});
  }

  std::optional<bool> FileManagerSettings::animationsEnabledOverride() const {
    return m_preferences["files"]["animations_enabled"].value_exact<bool>();
  }

  bool FileManagerSettings::effectiveAnimationsEnabled() const {
    if (const auto ovr = animationsEnabledOverride())
      return *ovr;
    if (inheritShellTheme())
      return m_shellConfig.config().shell.animation.enabled;
    return true;
  }

  bool FileManagerSettings::setAnimationsEnabledOverride(std::optional<bool> enabled) {
    if (!enabled)
      return reset({"files", "animations_enabled"});
    return save({{{"files", "animations_enabled"}, literal(*enabled)}});
  }

  std::optional<float> FileManagerSettings::animationSpeedOverride() const {
    if (const auto value = m_preferences["files"]["animation_speed"].value<double>())
      return static_cast<float>(std::clamp(*value, 0.25, 4.0));
    return std::nullopt;
  }

  float FileManagerSettings::effectiveAnimationSpeed() const {
    if (const auto ovr = animationSpeedOverride())
      return *ovr;
    if (inheritShellTheme())
      return std::clamp(m_shellConfig.config().shell.animation.speed, 0.25F, 4.0F);
    return 1.0F;
  }

  bool FileManagerSettings::setAnimationSpeedOverride(std::optional<float> speed) {
    if (!speed)
      return reset({"files", "animation_speed"});
    const float clamped = std::clamp(*speed, 0.25F, 4.0F);
    return save({{{"files", "animation_speed"}, std::format("{:.2f}", clamped)}});
  }

  std::optional<bool> FileManagerSettings::hazeEnabledOverride() const {
    return m_preferences["files"]["haze_enabled"].value_exact<bool>();
  }

  bool FileManagerSettings::effectiveHazeEnabled() const {
    if (const auto ovr = hazeEnabledOverride())
      return *ovr;
    if (inheritShellTheme())
      return m_shellConfig.config().shell.panel.hazeEnabled;
    return false;
  }

  bool FileManagerSettings::setHazeEnabledOverride(std::optional<bool> enabled) {
    if (!enabled)
      return reset({"files", "haze_enabled"});
    return save({{{"files", "haze_enabled"}, literal(*enabled)}});
  }

  BrandingMode FileManagerSettings::brandingMode() const {
    const auto value = m_preferences["files"]["branding_mode"].value_or<std::string_view>("full");
    if (value == "glyph")
      return BrandingMode::Compact;
    if (value == "off")
      return BrandingMode::Off;
    return BrandingMode::Full;
  }

  bool FileManagerSettings::setBrandingMode(BrandingMode mode) {
    const std::string_view key = mode == BrandingMode::Compact ? "glyph" : mode == BrandingMode::Off ? "off" : "full";
    return save({{{"files", "branding_mode"}, literal(std::string(key))}});
  }

  // Stage 2: Navigation preferences

  NewTabLocation FileManagerSettings::newTabLocation() const {
    const auto value = m_preferences["files"]["new_tab_location"].value_or<std::string_view>("home");
    if (value == "current")
      return NewTabLocation::Current;
    if (value == "custom")
      return NewTabLocation::Custom;
    if (value == "startup")
      return NewTabLocation::Startup;
    return NewTabLocation::Home;
  }

  bool FileManagerSettings::setNewTabLocation(NewTabLocation location) {
    const std::string_view key = location == NewTabLocation::Current ? "current"
        : location == NewTabLocation::Custom                         ? "custom"
        : location == NewTabLocation::Startup                        ? "startup"
                                                                     : "home";
    return save({{{"files", "new_tab_location"}, literal(std::string(key))}});
  }

  fs::path FileManagerSettings::newTabCustomPath() const {
    return fs::path(m_preferences["files"]["new_tab_custom_path"].value_or<std::string_view>(""));
  }

  bool FileManagerSettings::setNewTabCustomPath(const fs::path& path) {
    return save({{{"files", "new_tab_custom_path"}, literal(path.string())}});
  }

  NewTabPosition FileManagerSettings::newTabPosition() const {
    const auto value = m_preferences["files"]["new_tab_position"].value_or<std::string_view>("end");
    return value == "after_current" ? NewTabPosition::AfterCurrent : NewTabPosition::End;
  }

  bool FileManagerSettings::setNewTabPosition(NewTabPosition position) {
    return save(
        {{{"files", "new_tab_position"},
          literal(std::string(position == NewTabPosition::AfterCurrent ? "after_current" : "end"))}}
    );
  }

  MiddleClickFolderAction FileManagerSettings::middleClickFolder() const {
    const auto value = m_preferences["files"]["middle_click_folder"].value_or<std::string_view>("background_tab");
    if (value == "foreground_tab")
      return MiddleClickFolderAction::ForegroundTab;
    if (value == "new_window")
      return MiddleClickFolderAction::NewWindow;
    if (value == "none")
      return MiddleClickFolderAction::None;
    return MiddleClickFolderAction::BackgroundTab;
  }

  bool FileManagerSettings::setMiddleClickFolder(MiddleClickFolderAction action) {
    const std::string_view key = action == MiddleClickFolderAction::ForegroundTab ? "foreground_tab"
        : action == MiddleClickFolderAction::NewWindow                            ? "new_window"
        : action == MiddleClickFolderAction::None                                 ? "none"
                                                                                  : "background_tab";
    return save({{{"files", "middle_click_folder"}, literal(std::string(key))}});
  }

  ClickActivation FileManagerSettings::clickActivation() const {
    const auto value = m_preferences["files"]["click_activation"].value_or<std::string_view>("double");
    return value == "single" ? ClickActivation::Single : ClickActivation::Double;
  }

  bool FileManagerSettings::setClickActivation(ClickActivation activation) {
    return save(
        {{{"files", "click_activation"},
          literal(std::string(activation == ClickActivation::Single ? "single" : "double"))}}
    );
  }

  AddressBarMode FileManagerSettings::addressBarMode() const {
    const auto value = m_preferences["files"]["address_bar_mode"].value_or<std::string_view>("breadcrumbs");
    return value == "full_path" ? AddressBarMode::FullPath : AddressBarMode::Breadcrumbs;
  }

  bool FileManagerSettings::setAddressBarMode(AddressBarMode mode) {
    return save(
        {{{"files", "address_bar_mode"},
          literal(std::string(mode == AddressBarMode::FullPath ? "full_path" : "breadcrumbs"))}}
    );
  }

  bool FileManagerSettings::showPlacesPersonal() const {
    return m_preferences["files"]["show_places_personal"].value_or(true);
  }

  bool FileManagerSettings::setShowPlacesPersonal(bool show) {
    return save({{{"files", "show_places_personal"}, literal(show)}});
  }

  bool FileManagerSettings::showPlacesDevices() const {
    return m_preferences["files"]["show_places_devices"].value_or(true);
  }

  bool FileManagerSettings::setShowPlacesDevices(bool show) {
    return save({{{"files", "show_places_devices"}, literal(show)}});
  }

  bool FileManagerSettings::showPlacesNetwork() const {
    return m_preferences["files"]["show_places_network"].value_or(true);
  }

  bool FileManagerSettings::setShowPlacesNetwork(bool show) {
    return save({{{"files", "show_places_network"}, literal(show)}});
  }

  bool FileManagerSettings::showPlacesCapacity() const {
    return m_preferences["files"]["show_places_capacity"].value_or(true);
  }

  bool FileManagerSettings::setShowPlacesCapacity(bool show) {
    return save({{{"files", "show_places_capacity"}, literal(show)}});
  }

  bool FileManagerSettings::recentActivityEnabled() const {
    return m_preferences["files"]["recent_activity_enabled"].value_or(true);
  }

  bool FileManagerSettings::setRecentActivityEnabled(bool enabled) {
    return save({{{"files", "recent_activity_enabled"}, literal(enabled)}});
  }

  bool FileManagerSettings::showRecentSidebar() const {
    return m_preferences["files"]["show_recent_sidebar"].value_or(true);
  }

  bool FileManagerSettings::setShowRecentSidebar(bool show) {
    return save({{{"files", "show_recent_sidebar"}, literal(show)}});
  }

  bool FileManagerSettings::showFrequentLocations() const {
    return m_preferences["files"]["show_frequent_locations"].value_or(true);
  }

  bool FileManagerSettings::setShowFrequentLocations(bool show) {
    return save({{{"files", "show_frequent_locations"}, literal(show)}});
  }

  int FileManagerSettings::frequentLocationsCount() const {
    return static_cast<int>(
        std::clamp<std::int64_t>(m_preferences["files"]["frequent_locations_count"].value_or(std::int64_t{5}), 1, 20)
    );
  }

  bool FileManagerSettings::setFrequentLocationsCount(int count) {
    if (count < 1 || count > 20) {
      m_lastSaveError = "Frequent locations count must be between 1 and 20";
      return false;
    }
    return save({{{"files", "frequent_locations_count"}, literal(static_cast<std::int64_t>(count))}});
  }

  RecentRetention FileManagerSettings::recentRetention() const {
    const auto value = m_preferences["files"]["recent_retention"].value_or<std::string_view>("30d");
    if (value == "7d")
      return RecentRetention::Days7;
    if (value == "90d")
      return RecentRetention::Days90;
    if (value == "indefinite")
      return RecentRetention::Indefinite;
    return RecentRetention::Days30;
  }

  bool FileManagerSettings::setRecentRetention(RecentRetention retention) {
    const std::string_view key = retention == RecentRetention::Days7 ? "7d"
        : retention == RecentRetention::Days90                       ? "90d"
        : retention == RecentRetention::Indefinite                   ? "indefinite"
                                                                     : "30d";
    return save({{{"files", "recent_retention"}, literal(std::string(key))}});
  }

  // Stage 2: Runtime state persistence

  fs::path FileManagerSettings::stateDir() const {
    if (!m_state.path().empty())
      return m_state.path().parent_path();
    const auto dir = FileUtils::applicationStateDir(FileUtils::Application::Files);
    return dir.empty() ? fs::path{} : fs::path(dir);
  }

  bool FileManagerSettings::updateRuntimeState(const std::function<bool()>& update) {
    const RuntimeFileLock lock(m_state.path());
    if (!lock)
      return false;
    m_state.loadReadOnly();
    return m_state.parseError().empty() && update();
  }

  std::optional<WindowGeometryState> FileManagerSettings::loadMainWindowGeometry() const {
    const auto size = loadWindowSize("main_window_size", 560, 380);
    if (!size)
      return std::nullopt;
    return WindowGeometryState{
        .width = size->first,
        .height = size->second,
        .maximized = m_state.boolValue(kOwner, "main_window_maximized").value_or(false),
    };
  }

  bool FileManagerSettings::saveMainWindowGeometry(const WindowGeometryState& geometry) {
    return updateRuntimeState([&] {
      const auto size = std::format("{}x{}", geometry.width, geometry.height);
      return m_state.setString(kOwner, "main_window_size", size)
          && m_state.setBool(kOwner, "main_window_maximized", geometry.maximized);
    });
  }

  std::optional<fs::path> FileManagerSettings::loadLastVisitedLocation() const {
    const auto stored = m_state.stringValue(kOwner, "last_visited_location");
    if (!stored || stored->empty())
      return std::nullopt;
    fs::path p(*stored);
    return p.is_absolute() ? std::optional(p) : std::nullopt;
  }

  bool FileManagerSettings::saveLastVisitedLocation(const fs::path& path) {
    if (path.empty() || !path.is_absolute())
      return false;
    return updateRuntimeState([&] { return m_state.setString(kOwner, "last_visited_location", path.string()); });
  }

  SavedTabsState FileManagerSettings::loadLastWindowTabs() const {
    SavedTabsState result;
    const auto stored = m_state.stringValue(kOwner, "last_window_tabs");
    if (!stored || stored->empty())
      return result;
    try {
      const auto root = nlohmann::json::parse(*stored);
      if (!root.is_object())
        return result;
      if (const auto it = root.find("paths"); it != root.end() && it->is_array()) {
        for (const auto& entry : *it) {
          if (!entry.is_string())
            continue;
          const fs::path p(entry.get<std::string>());
          if (p.is_absolute())
            result.paths.push_back(p);
        }
      }
      result.activeIndex = root.value("active", std::size_t{0});
      if (!result.paths.empty() && result.activeIndex >= result.paths.size())
        result.activeIndex = 0;
    } catch (const std::exception&) {
    }
    return result;
  }

  bool FileManagerSettings::saveLastWindowTabs(const SavedTabsState& tabs) {
    try {
      nlohmann::json pathsJson = nlohmann::json::array();
      for (const auto& p : tabs.paths) {
        if (!p.empty() && p.is_absolute())
          pathsJson.push_back(p.string());
      }
      const nlohmann::json root = {
          {"paths", std::move(pathsJson)},
          {"active", tabs.activeIndex},
      };
      return updateRuntimeState([&] { return m_state.setString(kOwner, "last_window_tabs", root.dump()); });
    } catch (const std::exception&) {
      return false;
    }
  }
} // namespace filemanager
