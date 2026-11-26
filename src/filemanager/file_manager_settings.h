#pragma once

#include "app/poll_source.h"
#include "config/config_merge.h"
#include "config/state_store.h"
#include "core/files/directory_scanner.h"
#include "core/inotify/inotify.h"
#include "filemanager/configuration_notice.h"
#include "filemanager/file_columns.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ConfigService;
enum class PanelTransparencyMode : std::uint8_t;

namespace filemanager {

  struct SavedPlace {
    std::string id;
    std::filesystem::path path;
    std::string label;
  };

  struct FileViewSettings {
    bool gridMode = false;
    bool showHidden = false;
    bool foldersFirst = true;
    bool hiddenLast = true;
    FileDialogSortField sortField = FileDialogSortField::Name;
    FileDialogSortOrder sortOrder = FileDialogSortOrder::Ascending;
  };

  class FileManagerSettings final : public PollSource {
  public:
    using ProblemCallback = std::function<void(const ConfigurationProblem&, const std::filesystem::path&)>;
    using ReloadCallback = std::function<void(const toml::table& previous)>;

    explicit FileManagerSettings(const ConfigService& shellConfig);
    explicit FileManagerSettings(
        const ConfigService& shellConfig, std::filesystem::path configDir, std::filesystem::path statePath
    );
    void setReloadCallback(ReloadCallback callback) { m_onReload = std::move(callback); }
    void setManualReloadCallback(std::function<void()> callback) { m_onManualReload = std::move(callback); }
    void setProblemCallback(ProblemCallback callback);
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;
    bool reload();
    [[nodiscard]] const toml::table& preferences() const { return m_preferences; }
    [[nodiscard]] bool readlineShortcuts() const;
    [[nodiscard]] PanelTransparencyMode transparencyMode() const;
    [[nodiscard]] bool reset(const nythera::config::ConfigKeyPath& path);

    [[nodiscard]] FileViewSettings loadFileView(bool search = false) const;
    [[nodiscard]] bool
    saveFileView(const FileViewSettings& settings, const FileViewSettings& previous, bool search = false);

    [[nodiscard]] bool showApplicationMenuBar() const;
    [[nodiscard]] bool setShowApplicationMenuBar(bool show);

    [[nodiscard]] bool alwaysShowTabBar() const;
    [[nodiscard]] bool setAlwaysShowTabBar(bool show);

    [[nodiscard]] bool openTabsInBackground() const;
    [[nodiscard]] bool setOpenTabsInBackground(bool background);

    [[nodiscard]] int adminInactivityTimeoutSeconds() const;
    [[nodiscard]] bool setAdminInactivityTimeoutSeconds(int seconds);

    void loadListColumnWidths(FileColumns& out, bool search = false) const;
    [[nodiscard]] bool saveListColumnWidths(const FileColumns& columns, bool search = false);

    void loadListColumnLayout(FileColumnLayout& out, bool search = false) const;
    [[nodiscard]] bool saveListColumnLayout(const FileColumnLayout& layout, bool search = false);

    [[nodiscard]] float loadViewScale(float defaultScale = 1.0F, bool search = false) const;
    [[nodiscard]] bool saveViewScale(float scale, bool search = false);

    [[nodiscard]] float loadSidebarWidth(float defaultWidth, float minWidth, float maxWidth) const;
    [[nodiscard]] bool saveSidebarWidth(float width);

    [[nodiscard]] float loadStatusHeight(float defaultHeight, float minHeight, float maxHeight) const;
    [[nodiscard]] bool saveStatusHeight(float height);

    [[nodiscard]] std::vector<SavedPlace> loadSavedPlaces() const;
    [[nodiscard]] bool
    saveSavedPlaces(const std::vector<SavedPlace>& places, const std::vector<std::string>* order = nullptr);

    [[nodiscard]] std::vector<std::string> loadPlaceOrder() const;
    [[nodiscard]] bool savePlaceOrder(const std::vector<std::string>& order);

    [[nodiscard]] std::vector<std::string> loadHiddenMounts() const;
    [[nodiscard]] bool saveHiddenMounts(const std::vector<std::string>& identities);

    [[nodiscard]] bool placesSectionExpanded(std::string_view key) const;
    [[nodiscard]] bool setPlacesSectionExpanded(std::string_view key, bool expanded);

    [[nodiscard]] static std::string dialogSizeStateKey(std::string_view title, bool conflictDialog);
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> loadDialogSize(std::string_view key) const;
    [[nodiscard]] bool saveDialogSize(std::string_view key, std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> loadPropertiesSize() const;
    [[nodiscard]] bool savePropertiesSize(std::uint32_t width, std::uint32_t height);

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    using Change = std::pair<nythera::config::ConfigKeyPath, std::optional<std::string>>;
    bool save(std::vector<Change> changes);
    void reportProblem(ConfigurationProblem problem);
    void updateWatches(const nythera::config::SourcedMergeResult& loaded);
    void accept(nythera::config::SourcedMergeResult loaded, bool notify);
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
    loadWindowSize(std::string_view key, std::uint32_t minWidth, std::uint32_t minHeight) const;

    const ConfigService& m_shellConfig;
    const std::filesystem::path m_configDir;
    StateStore m_state;
    toml::table m_preferences;
    ReloadCallback m_onReload;
    std::function<void()> m_onManualReload;
    ProblemCallback m_onProblem;
    std::optional<ConfigurationProblem> m_problem;
    Inotify m_inotify;
    struct Watch {
      std::filesystem::path directory;
      std::set<std::string> names;
      bool scanToml = false;
    };
    std::map<int, Watch> m_watches;
    std::map<std::filesystem::path, std::string> m_selfWrites;
  };

} // namespace filemanager
