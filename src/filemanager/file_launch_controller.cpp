#include "filemanager/file_launch_controller.h"

#include "config/config_service.h"
#include "filemanager/file_interaction.h"
#include "filemanager/property_operations.h"
#include "render/render_context.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <gio/gdesktopappinfo.h>
#include <unordered_set>

namespace filemanager {

  namespace {

    ContextMenuControlEntry applicationMenuEntry(std::int32_t id, GAppInfo* app) {
      ContextMenuControlEntry entry{.id = id, .label = FileLaunchController::applicationDisplayName(app)};
      entry.iconNames = FileLaunchController::applicationIconNames(app);
      return entry;
    }

  } // namespace

  FileLaunchController::FileLaunchController(
      WaylandConnection& wayland, RenderContext& renderContext, TabManager& tabs, ProtectedFileManager& protectedFiles,
      Callbacks callbacks
  )
      : m_wayland(wayland), m_renderContext(renderContext), m_tabs(tabs), m_protectedFiles(protectedFiles),
        m_callbacks(std::move(callbacks)) {}

  FileLaunchController::~FileLaunchController() {
    if (m_alive)
      *m_alive = false;
    closeApplicationPicker();
  }

  std::string FileLaunchController::applicationDisplayName(GAppInfo* app) {
    const char* name = app ? g_app_info_get_display_name(app) : nullptr;
    if (!name || !*name)
      name = app ? g_app_info_get_name(app) : nullptr;
    return name ? name : "Unknown application";
  }

  std::vector<std::string> FileLaunchController::applicationIconNames(GAppInfo* app) {
    std::vector<std::string> names;
    GIcon* icon = app ? g_app_info_get_icon(app) : nullptr;
    if (icon && G_IS_THEMED_ICON(icon)) {
      const char* const* themedNames = g_themed_icon_get_names(G_THEMED_ICON(icon));
      for (std::size_t i = 0; themedNames && themedNames[i]; ++i)
        names.emplace_back(themedNames[i]);
    }
    return names;
  }

  void FileLaunchController::initializePicker(ConfigService& configService, LayerPopupHostRegistry& pickerHosts) {
    m_applicationPicker = std::make_unique<SearchPickerPopup>();
    m_applicationPicker->initialize(m_wayland, configService, m_renderContext, pickerHosts);
  }

  void FileLaunchController::closeApplicationPicker() {
    if (auto request = std::exchange(m_applicationMimeRequest, {}))
      g_cancellable_cancel(request.get());
    if (m_applicationPicker)
      m_applicationPicker->close();
  }

  bool FileLaunchController::isApplicationPickerOpen() const {
    return m_applicationPicker && m_applicationPicker->isOpen();
  }

  bool FileLaunchController::handleAppPickerPointer(const PointerEvent& event) {
    return m_applicationPicker && m_applicationPicker->onPointerEvent(event);
  }

  bool FileLaunchController::handleKeyboard(const KeyboardEvent& event) {
    if (m_applicationPicker
        && m_applicationPicker->isOpen()
        && m_wayland.lastKeyboardSurface() == m_applicationPicker->wlSurface()) {
      m_applicationPicker->onKeyboardEvent(event);
      return true;
    }
    return false;
  }

  void FileLaunchController::clearOpenWithApplications() { m_openWithApplications.clear(); }

  void
  FileLaunchController::appendOpenWithEntries(const FileEntry& target, std::vector<ContextMenuControlEntry>& entries) {
    std::vector<ContextMenuControlEntry> alternatives;
    std::string mimeStorage = target.type;
    if (mimeStorage.empty()) {
      if (gchar* guessed = g_content_type_guess(target.absPath.filename().c_str(), nullptr, 0, nullptr)) {
        mimeStorage = guessed;
        g_free(guessed);
      }
    }
    if (!mimeStorage.empty()) {
      const char* mime = mimeStorage.c_str();
      std::shared_ptr<GAppInfo> defaultApp(g_app_info_get_default_for_type(mime, FALSE), g_object_unref);
      if (defaultApp) {
        entries.front() = applicationMenuEntry(menu_id::kOpen, defaultApp.get());
        entries.front().label = "Open with " + applicationDisplayName(defaultApp.get());
        entries.front().gutterIconNames = std::move(entries.front().iconNames);
        entries.front().iconNames.clear();
      }

      GList* compatible = g_app_info_get_all_for_type(mime);
      for (GList* item = compatible; item; item = item->next) {
        auto* app = G_APP_INFO(item->data);
        if (!g_app_info_get_id(app) || (defaultApp && g_app_info_equal(defaultApp.get(), app))) {
          g_object_unref(app);
          continue;
        }
        const auto appId = static_cast<std::int32_t>(menu_id::kFirstOpenWithApp + m_openWithApplications.size());
        alternatives.push_back(applicationMenuEntry(appId, app));
        m_openWithApplications.emplace_back(app, g_object_unref);
      }
      g_list_free(compatible);
    }
    ContextMenuControlEntry openWith{.id = menu_id::kOpenWithSubmenu, .label = "Open With", .hasSubmenu = true};
    openWith.submenuEntries = std::move(alternatives);
    openWith.submenuEntries.push_back({.id = menu_id::kOpenWithOther, .label = "Other Application…"});
    entries.insert(entries.begin() + 1, std::move(openWith));
  }

  void FileLaunchController::openApplicationChooser(const std::filesystem::path& target) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    if (!m_applicationPicker || target.empty() || session.browsingTrash() || !tab.capabilities().canLaunchApps)
      return;
    const auto targetEntry = std::ranges::find(session.entries(), target, &FileEntry::absPath);
    std::string mime = targetEntry != session.entries().end() ? targetEntry->type : std::string{};
    if (mime.empty()) {
      g_autofree char* guessed = g_content_type_guess(target.filename().c_str(), nullptr, 0, nullptr);
      if (guessed)
        mime = guessed;
    }
    closeApplicationPicker();
    if (!session.search().active()) {
      showApplicationChooser(std::move(mime), target);
      return;
    }

    // The chooser uses the actual MIME type for recommendations and for setting the default app.
    struct Request {
      FileLaunchController* owner;
      std::weak_ptr<bool> alive;
      std::shared_ptr<GCancellable> cancel;
      TabId tabId;
      std::uint64_t navigation;
      std::string query;
      std::filesystem::path path;
    };
    m_applicationMimeRequest = std::shared_ptr<GCancellable>(g_cancellable_new(), g_object_unref);
    g_autoptr(GFile) file = g_file_new_for_path(target.c_str());
    g_file_query_info_async(
        file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
        m_applicationMimeRequest.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), result, nullptr);
          if (g_cancellable_is_cancelled(request->cancel.get()))
            return;
          const auto locked = request->alive.lock();
          if (!locked || !*locked)
            return;
          auto& self = *request->owner;
          self.m_applicationMimeRequest.reset();
          const auto& active = self.m_tabs.activeTab();
          if (active.id != request->tabId
              || active.session.navigationGeneration() != request->navigation
              || !active.session.search().active()
              || active.session.search().query() != request->query
              || !self.m_applicationPicker
              || !active.capabilities().canLaunchApps)
            return;
          const auto* detectedType =
              info ? g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE) : nullptr;
          self.showApplicationChooser(detectedType ? detectedType : "", request->path);
        },
        new Request{
            this, m_alive, m_applicationMimeRequest, tab.id, session.navigationGeneration(), session.search().query(),
            target
        }
    );
  }

  void FileLaunchController::showApplicationChooser(std::string mime, const std::filesystem::path& target) {
    const auto& tab = m_tabs.activeTab();
    m_applicationChooserMime = std::move(mime);
    m_applicationChooserApps.clear();
    std::vector<SearchPickerOption> options;
    std::unordered_set<std::string> recommendedIds;
    std::unordered_set<std::string> associatedIds;
    if (!m_applicationChooserMime.empty()) {
      GList* recommended = g_app_info_get_recommended_for_type(m_applicationChooserMime.c_str());
      for (GList* item = recommended; item; item = item->next) {
        const char* id = g_app_info_get_id(G_APP_INFO(item->data));
        if (id && *id)
          recommendedIds.emplace(id);
      }
      g_list_free_full(recommended, g_object_unref);
      associatedIds = recommendedIds;
      GList* associated = g_app_info_get_all_for_type(m_applicationChooserMime.c_str());
      for (GList* item = associated; item; item = item->next) {
        const char* id = g_app_info_get_id(G_APP_INFO(item->data));
        if (id && *id)
          associatedIds.emplace(id);
      }
      g_list_free_full(associated, g_object_unref);
    }

    std::unordered_set<std::string> seen;
    auto appendApps = [&](GList* apps) {
      for (GList* item = apps; item; item = item->next) {
        auto* app = G_APP_INFO(item->data);
        const char* id = g_app_info_get_id(app);
        if (!id
            || !*id
            || !g_app_info_should_show(app)
            || (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app))
            || !seen.emplace(id).second) {
          g_object_unref(app);
          continue;
        }
        const bool compatible = associatedIds.contains(id);
        const std::size_t index = m_applicationChooserApps.size();
        options.push_back(
            SearchPickerOption{
                .value = std::to_string(index),
                .label = applicationDisplayName(app),
                .description = compatible ? "Compatible with this file" : "Installed application",
                .iconNames = applicationIconNames(app),
                .compatible = compatible
            }
        );
        m_applicationChooserApps.emplace_back(app, g_object_unref);
      }
      g_list_free(apps);
    };

    if (!m_applicationChooserMime.empty()) {
      GAppInfo* defaultApp = g_app_info_get_default_for_type(m_applicationChooserMime.c_str(), FALSE);
      if (defaultApp != nullptr) {
        const char* id = g_app_info_get_id(defaultApp);
        if (id && *id) {
          associatedIds.emplace(id);
          GList* defaultList = nullptr;
          defaultList = g_list_append(defaultList, defaultApp);
          appendApps(defaultList);
        } else {
          g_object_unref(defaultApp);
        }
      }
    }

    if (!m_applicationChooserMime.empty()) {
      appendApps(g_app_info_get_recommended_for_type(m_applicationChooserMime.c_str()));
      appendApps(g_app_info_get_all_for_type(m_applicationChooserMime.c_str()));
    }
    appendApps(g_app_info_get_all());
    m_applicationPicker->setOnSelectWithOption([this, tabId = tab.id,
                                                target](const std::string& value, bool setAsDefault) {
      const auto* origin = m_tabs.findTab(tabId);
      if (!origin || !origin->capabilities().canLaunchApps)
        return;
      std::size_t index = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), index);
      if (error != std::errc{} || end != value.data() + value.size() || index >= m_applicationChooserApps.size()) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("The selected application is no longer available");
        return;
      }
      GAppInfo* app = m_applicationChooserApps[index].get();
      if (setAsDefault) {
        GError* defaultError = nullptr;
        if (!filemanager::savePropertyDefaultApplication(app, m_applicationChooserMime, &defaultError)) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus(defaultError ? defaultError->message : "Could not save the default application");
          g_clear_error(&defaultError);
        }
      }
      if (launchProtectedApplication(*origin, app, target))
        return;
      GError* launchError = nullptr;
      if (!launchApplicationForFile(app, target, &launchError)) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus(
              launchError ? launchError->message : "Could not open this file with the selected application"
          );
      } else if (!setAsDefault) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("");
      }
      g_clear_error(&launchError);
    });
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    SearchPickerPopupRequest request{
        .title = "Open With",
        .options = std::move(options),
        .placeholder = "Search applications…",
        .emptyText = "No matching applications",
        .optionToggleChecked = false,
        .showCancelButton = true,
        .explicitConfirmation = true,
        .showAllApplicationsToggle = true,
        .preferredWidth = 500.0F,
        .preferredHeight = 500.0F,
        .nativeWindow = true,
        .nativeAppId = "dev.nythera.FileManager.OpenWith",
        .nativeParent = surface ? surface->xdgToplevel() : nullptr,
        .nativeParentSurface = surface ? surface->wlSurface() : nullptr
    };
    if (!m_applicationChooserMime.empty())
      request.optionToggleLabel = "Set as default for this file type";
    m_applicationPicker->open(std::move(request));
  }

  bool FileLaunchController::launchOpenWithApp(std::size_t index, const std::filesystem::path& target) {
    if (!m_tabs.activeTab().capabilities().canLaunchApps || index >= m_openWithApplications.size() || target.empty())
      return false;
    if (launchProtectedApplication(m_tabs.activeTab(), m_openWithApplications[index].get(), target))
      return true;
    GError* error = nullptr;
    if (!launchApplicationForFile(m_openWithApplications[index].get(), target, &error)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus(error ? error->message : "Could not open this file with the selected application");
    } else if (m_callbacks.onStatus) {
      m_callbacks.onStatus("");
    }
    g_clear_error(&error);
    return true;
  }

  std::vector<std::string> FileLaunchController::externalToolTargets(external_tools::Tool tool) const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    if (session.browsingTrash() || !tab.capabilities().canRunExternalTools || session.loading())
      return {};
    if (tool == external_tools::Tool::Compare) {
      if (!m_callbacks.selectedPaths || !m_callbacks.selectedPathsOrdered)
        return {};
      const auto& selectedPaths = m_callbacks.selectedPaths();
      if (selectedPaths.size() != 2)
        return {};
      auto paths = m_callbacks.selectedPathsOrdered();
      if (tab.hasActiveAdminSession() && std::ranges::any_of(paths, [&](const std::string& path) {
            if (userCanReadPath(path)) {
              std::error_code error;
              return !std::filesystem::is_regular_file(path, error);
            }
            const auto entry = std::ranges::find(session.entries(), std::filesystem::path(path), &FileEntry::absPath);
            return entry == session.entries().end()
                || entry->isDir
                || (!entry->isSymlink && entry->type.starts_with("inode/"));
          }))
        return {};
      return paths;
    }
    return !tab.administratorBrowsing || userCanReadPath(session.path(), true) ? std::vector{session.path().string()}
                                                                               : std::vector<std::string>{};
  }

  std::vector<ContextMenuControlEntry> FileLaunchController::externalToolMenuRows() {
    namespace tools = external_tools;
    const auto& tab = m_tabs.activeTab();
    const auto caps = tab.capabilities();
    const auto applications = tools::discover();
    constexpr std::array labels{"Open Terminal Here", "Analyse Disk Usage…", "Compare Files…"};
    constexpr std::array fallbackGlyphs{"terminal-2", "chart-pie", "file-diff"};
    const auto iconsFor = [](GDesktopAppInfo* app) {
      auto names = applicationIconNames(G_APP_INFO(app));
      if (GIcon* icon = g_app_info_get_icon(G_APP_INFO(app)); G_IS_FILE_ICON(icon)) {
        if (char* path = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)))) {
          names.emplace_back(path);
          g_free(path);
        }
      }
      return names;
    };

    m_externalTools.clear();
    std::vector<ContextMenuControlEntry> rows;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      const auto tool = static_cast<tools::Tool>(i);
      const auto& apps = applications[i];
      const auto targets = externalToolTargets(tool);
      ContextMenuControlEntry entry{
          .id = menu_id::kFirstToolMenuRow + static_cast<int>(i),
          .label = labels[i],
          .enabled = caps.canRunExternalTools
              && !apps.choices.empty()
              && (tools::validTargets(tool, targets)
                  || (tool == tools::Tool::Compare && tab.hasActiveAdminSession() && targets.size() == 2)),
          .hasSubmenu = true,
          .attachSubmenuContent = true,
          .splitSubmenu = true,
          .gutterFallbackGlyph = fallbackGlyphs[i],
      };
      if (apps.preferred)
        entry.gutterIconNames = iconsFor(apps.preferred.get());
      if (entry.enabled) {
        for (const auto& choice : apps.choices) {
          const auto id = menu_id::kFirstExternalToolApp + static_cast<int>(m_externalTools.size());
          m_externalTools.emplace_back(tool, choice);
          if (choice.app == apps.preferred) {
            entry.id = id;
            entry.activateMainSection = true;
          }
          entry.submenuEntries.push_back({
              .id = id,
              .label = applicationDisplayName(G_APP_INFO(choice.app.get())),
              .gutterIconNames = iconsFor(choice.app.get()),
              .gutterFallbackGlyph = fallbackGlyphs[i],
          });
        }
      }
      rows.push_back(std::move(entry));
    }
    return rows;
  }

  bool FileLaunchController::launchExternalTool(std::size_t index) {
    if (!m_tabs.activeTab().capabilities().canRunExternalTools || index >= m_externalTools.size())
      return false;
    const auto [tool, choice] = m_externalTools[index];
    const auto targets = externalToolTargets(tool);
    const auto& tab = m_tabs.activeTab();
    if (tab.hasActiveAdminSession()
        && tool == external_tools::Tool::Compare
        && targets.size() == 2
        && std::ranges::any_of(targets, [](const auto& path) { return !userCanReadPath(path); })) {
      auto prepare = [this, id = tab.id](
                         const std::filesystem::path& path, std::function<void(const std::filesystem::path&)> ready
                     ) {
        if (userCanReadPath(path))
          ready(path);
        else
          m_protectedFiles.openFile(id, path, std::move(ready));
      };
      prepare(
          targets.front(),
          [prepare, second = targets.back(), choice, status = m_callbacks.onStatus](const auto& first) {
            prepare(second, [first, choice, status](const auto& last) {
              const std::array paths{first.string(), last.string()};
              g_autoptr(GError) error = nullptr;
              const bool launched = external_tools::launch(choice, external_tools::Tool::Compare, paths, &error);
              if (status)
                status(
                    launched ? "Opened comparison. Use Save Changes Back for edited temporary copies."
                             : (error ? error->message : "Could not open the comparison.")
                );
            });
          }
      );
      return true;
    }
    GError* error = nullptr;
    if (external_tools::launch(choice, tool, targets, &error)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("");
    } else if (m_callbacks.onStatus) {
      m_callbacks.onStatus(error ? error->message : "Could not launch the selected tool");
    }
    g_clear_error(&error);
    return true;
  }

  bool FileLaunchController::launchProtectedApplication(
      const BrowserTab& tab, GAppInfo* app, const std::filesystem::path& path
  ) {
    if (!tab.administratorBrowsing || userCanReadPath(path))
      return false;
    auto application = std::shared_ptr<GAppInfo>(G_APP_INFO(g_object_ref(app)), g_object_unref);
    m_protectedFiles.openFile(tab.id, path, [application, status = m_callbacks.onStatus](const auto& temporary) {
      g_autoptr(GError) error = nullptr;
      const bool opened = launchApplicationForFile(application.get(), temporary, &error);
      if (status)
        status(
            opened ? "Opened a temporary copy. Use Save Changes Back to update the original."
                   : (error ? error->message : "Could not open the temporary copy.")
        );
    });
    return true;
  }

  void FileLaunchController::openProtectedFileWithDefault(TabId tabId, const std::filesystem::path& path) {
    m_protectedFiles.openFile(tabId, path, [this, tabId](const auto& temporary) {
      g_autoptr(GFile) file = g_file_new_for_path(temporary.c_str());
      launchFileUri(tabId, file, "Opened a temporary copy. Use Save Changes Back to update the original.");
    });
  }

  void FileLaunchController::launchFileUri(TabId tabId, GFile* file, std::string success) {
    char* uri = g_file_get_uri(file);
    struct LaunchRequest {
      FileLaunchController* controller;
      std::weak_ptr<bool> alive;
      TabId tabId;
      std::string success;
    };
    g_app_info_launch_default_for_uri_async(
        uri, nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer data) {
          std::unique_ptr<LaunchRequest> request(static_cast<LaunchRequest*>(data));
          GError* error = nullptr;
          const bool launched = g_app_info_launch_default_for_uri_finish(result, &error);
          if (const auto locked = request->alive.lock(); locked && *locked) {
            std::string message = !launched ? (error == nullptr ? "Could not open this file" : error->message)
                                            : std::move(request->success);
            if (request->tabId == request->controller->m_tabs.activeTabId()) {
              if (request->controller->m_callbacks.onStatus)
                request->controller->m_callbacks.onStatus(std::move(message));
            } else if (auto* originTab = request->controller->m_tabs.findTab(request->tabId)) {
              originTab->lastStatusMessage = std::move(message);
            }
          }
          g_clear_error(&error);
        },
        new LaunchRequest{this, m_alive, tabId, std::move(success)}
    );
    g_free(uri);
  }

  bool
  FileLaunchController::launchApplicationForFile(GAppInfo* app, const std::filesystem::path& path, GError** error) {
    if (!app || path.empty()) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No application or file was selected.");
      return false;
    }
    GFile* file = g_file_new_for_path(path.c_str());
    GList files{file, nullptr, nullptr};
    char* uri = g_file_get_uri(file);
    GList uris{uri, nullptr, nullptr};
    const gboolean launched = g_app_info_supports_files(app) ? g_app_info_launch(app, &files, nullptr, error)
                                                             : g_app_info_launch_uris(app, &uris, nullptr, error);
    g_free(uri);
    g_object_unref(file);
    return launched == TRUE;
  }

} // namespace filemanager
