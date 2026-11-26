#include "filemanager/file_manager_settings.h"

#include "config/application_paths.h"
#include "config/config_service.h"
#include "config/files_config_validator.h"
#include "config/toml_persistence.h"
#include "core/log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
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

    std::optional<PanelTransparencyMode> transparency(toml::node_view<const toml::node> node) {
      const auto name = node.value_exact<std::string_view>();
      for (const auto& option : kPanelTransparencyModes) {
        if (name && *name == option.key)
          return option.value;
      }
      return std::nullopt;
    }

    fs::path canonicalPath(const fs::path& path) {
      std::error_code ec;
      auto resolved = fs::weakly_canonical(path, ec);
      return ec ? path : resolved;
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
    reload();
  }

  bool FileManagerSettings::readlineShortcuts() const {
    return m_preferences["files"]["readline_shortcuts"].value_or(m_shellConfig.config().shell.readlineShortcuts);
  }

  PanelTransparencyMode FileManagerSettings::transparencyMode() const {
    return transparency(m_preferences["theme"]["transparency_mode"])
        .value_or(m_shellConfig.config().shell.panel.transparencyMode);
  }

  void FileManagerSettings::updateWatches(const SourcedMergeResult& loaded) {
    if (m_configDir.empty())
      return;
    std::map<fs::path, Watch> desired;
    auto add = [&](fs::path path, bool contents) {
      std::error_code error;
      fs::path name;
      if (!contents) {
        name = path.filename();
        path = path.parent_path();
      }
      while (!path.empty() && !fs::is_directory(path, error)) {
        name = path.filename();
        const auto parent = path.parent_path();
        if (parent == path)
          return;
        path = parent;
      }
      if (path.empty())
        return;
      const auto resolved = canonicalPath(path);
      auto& watch = desired[resolved];
      watch.directory = resolved;
      if (name.empty()) {
        watch.scanToml = true;
      } else {
        watch.names.insert(name.string());
      }
    };
    add(m_configDir, true);
    add(m_configDir, false);
    for (const auto& dir : loaded.config.includeDirs)
      add(dir, true);
    for (const auto& path : loaded.watchPaths) {
      add(path, false);
      const auto target = canonicalPath(path);
      if (target != path)
        add(target, false);
      // weakly_canonical cannot follow a dangling link. Watch its named target
      // so recreating an external included file also recovers the configuration.
      std::error_code error;
      const auto link = fs::read_symlink(path, error);
      if (!error)
        add(link.is_absolute() ? link : path.parent_path() / link, false);
    }
    for (const auto& path : loaded.config.loadedFiles)
      add(path, false);

    constexpr auto mask = IN_CLOSE_WRITE
        | IN_MOVED_TO
        | IN_MOVED_FROM
        | IN_DELETE
        | IN_CREATE
        | IN_DELETE_SELF
        | IN_MOVE_SELF
        | IN_ATTRIB;
    std::map<int, Watch> watches;
    for (auto& [path, watch] : desired) {
      if (auto descriptor = m_inotify.watch(path, mask))
        watches.emplace(*descriptor, std::move(watch));
    }
    for (const auto& [descriptor, watch] : m_watches) {
      if (!watches.contains(descriptor))
        m_inotify.unwatch(descriptor);
    }
    m_watches = std::move(watches);
  }

  void FileManagerSettings::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_inotify.fd() >= 0)
      fds.push_back({.fd = m_inotify.fd(), .events = POLLIN});
  }

  void FileManagerSettings::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (m_inotify.fd() < 0 || (fds[startIdx].revents & POLLIN) == 0)
      return;
    bool changed = false;
    m_inotify.drain([&](const inotify_event* event) {
      if (event->mask & IN_Q_OVERFLOW) {
        changed = true;
        return;
      }
      const auto watch = m_watches.find(event->wd);
      if (watch == m_watches.end())
        return;
      if (!event->len) {
        changed = true;
        return;
      }
      const std::string_view name(event->name);
      if (!watch->second.names.contains(std::string(name)) && !(watch->second.scanToml && name.ends_with(".toml")))
        return;
      // An ordinary new regular file is read after its writer closes it.
      if ((event->mask & IN_CREATE) && !(event->mask & IN_ISDIR)) {
        std::error_code ec;
        if (!fs::is_symlink(watch->second.directory / name, ec))
          return;
      }
      const auto write = m_selfWrites.find(watch->second.directory / name);
      if (write != m_selfWrites.end() && (event->mask & IN_MOVED_TO)) {
        std::ifstream input(write->first, std::ios::binary);
        if (input && std::string(std::istreambuf_iterator<char>(input), {}) == write->second)
          return;
      }
      changed = true;
    });
    m_selfWrites.clear();
    if (changed && reload() && m_onManualReload)
      m_onManualReload();
  }

  void FileManagerSettings::accept(SourcedMergeResult loaded, bool notify) {
    updateWatches(loaded);
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
    auto loaded = mergeConfigWithSources(m_configDir.string());
    if (auto error = validateFilesConfiguration(loaded)) {
      updateWatches(loaded);
      kLog.warn("Retaining the previous Files configuration: {}", error->origin.prefixed(error->message));
      reportProblem(std::move(*error));
      return false;
    }
    accept(std::move(loaded), true);
    return true;
  }

  bool FileManagerSettings::save(std::vector<Change> changes) {
    if (changes.empty())
      return true;
    if (m_configDir.empty())
      return false;
    auto current = mergeConfigWithSources(m_configDir.string());
    if (auto error = validateFilesConfiguration(current)) {
      kLog.warn("Cannot save Files configuration: {}", error->origin.prefixed(error->message));
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
    for (const auto& change : changes) {
      const auto& [key, value] = change;
      if (key.size() < 2 || (key.front() != "files" && key.front() != "theme"))
        return false;
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
          (void)recordApplicationConfigWrites(kOwner, recorded);
          return std::nullopt;
        });
    if (!result.ok()) {
      clearApplicationConfigWrites(kOwner);
      kLog.warn("Cannot save Files configuration: {}", result.error);
      for (const auto& path : result.rollbackFailures)
        kLog.error("Could not restore '{}'", path.string());
      return false;
    }
    for (auto& [path, content] : expectedWrites)
      m_selfWrites.insert_or_assign(path, std::move(content));
    accept(candidate ? std::move(*candidate) : std::move(current), hadExternalChanges);
    return true;
  }

  bool FileManagerSettings::reset(const ConfigKeyPath& path) { return save({{path, std::nullopt}}); }

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

  float FileManagerSettings::loadSidebarWidth(float defaultWidth, float minWidth, float maxWidth) const {
    return loadClampedFloat(m_state, "sidebar_width", defaultWidth, minWidth, maxWidth);
  }

  bool FileManagerSettings::saveSidebarWidth(float width) {
    return m_state.setString(kOwner, "sidebar_width", std::to_string(width));
  }

  float FileManagerSettings::loadStatusHeight(float defaultHeight, float minHeight, float maxHeight) const {
    return loadClampedFloat(m_state, "status_height", defaultHeight, minHeight, maxHeight);
  }

  bool FileManagerSettings::saveStatusHeight(float height) {
    return m_state.setString(kOwner, "status_height", std::to_string(height));
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
    return m_state.setBool(kOwner, key, expanded);
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
    return m_state.setString(kOwner, key, size);
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
} // namespace filemanager
