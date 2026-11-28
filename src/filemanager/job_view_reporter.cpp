#include "filemanager/job_view_reporter.h"

#include "core/files/display_text.h"

namespace filemanager {
  namespace {
    using Kind = FileOperationManager::Kind;
    bool isTransfer(Kind kind) { return kind == Kind::Copy || kind == Kind::Move || kind == Kind::Duplicate; }
    bool isArchive(Kind kind) { return kind == Kind::Compress || kind == Kind::Extract; }
    std::string quotedBasename(const std::string& path) {
      const std::string name = std::filesystem::path(path).filename().string();
      return name.empty() ? "item" : "\"" + name + "\"";
    }
  } // namespace

  JobViewReporter::JobViewReporter(
      std::function<void(bool)> serviceChanged, Control cancel, Control pause, Control resume
  )
      : m_serviceChanged(std::move(serviceChanged)), m_cancel(std::move(cancel)), m_pause(std::move(pause)),
        m_resume(std::move(resume)) {
    GError* error = nullptr;
    m_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    g_clear_error(&error);
    if (m_bus != nullptr)
      m_serviceWatch = g_bus_watch_name_on_connection(
          m_bus, "org.kde.JobViewServer", G_BUS_NAME_WATCHER_FLAGS_NONE, serviceAppeared, serviceVanished, this, nullptr
      );
  }

  JobViewReporter::~JobViewReporter() { shutdown(); }

  void JobViewReporter::shutdown() noexcept {
    if (m_serviceWatch != 0) {
      g_bus_unwatch_name(m_serviceWatch);
      m_serviceWatch = 0;
    }
    if (m_bus != nullptr) {
      for (const auto& [id, subscription] : m_signalSubscriptions) {
        (void)id;
        g_dbus_connection_signal_unsubscribe(m_bus, subscription);
      }
    }
    m_signalSubscriptions.clear();
    if (m_bus != nullptr) {
      g_object_unref(m_bus);
      m_bus = nullptr;
    }
  }

  void JobViewReporter::serviceAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer userData) {
    auto* self = static_cast<JobViewReporter*>(userData);
    self->m_serviceAvailable = true;
    self->m_serviceChanged(true);
  }
  void JobViewReporter::serviceVanished(GDBusConnection*, const gchar*, gpointer userData) {
    auto* self = static_cast<JobViewReporter*>(userData);
    self->m_serviceAvailable = false;
    for (const auto& [id, subscription] : self->m_signalSubscriptions) {
      (void)id;
      g_dbus_connection_signal_unsubscribe(self->m_bus, subscription);
    }
    self->m_signalSubscriptions.clear();
    self->m_jobPaths.clear();
    self->m_serviceChanged(false);
  }
  void JobViewReporter::jobSignal(
      GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar* signal, GVariant*, gpointer userData
  ) {
    auto* self = static_cast<JobViewReporter*>(userData);
    for (const auto& [id, jobPath] : self->m_jobPaths)
      if (jobPath == path) {
        if (g_str_equal(signal, "cancelRequested"))
          self->m_cancel(id);
        else if (g_str_equal(signal, "suspendRequested"))
          self->m_pause(id);
        else if (g_str_equal(signal, "resumeRequested"))
          self->m_resume(id);
        return;
      }
  }
  void JobViewReporter::registerJob(const Operation& operation, bool suspendable, bool hasRates) {
    if (m_bus == nullptr || !m_serviceAvailable || m_jobPaths.contains(operation.id))
      return;
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "title", g_variant_new_string("File operation"));
    GError* error = nullptr;
    GVariant* response = g_dbus_connection_call_sync(
        m_bus, "org.kde.JobViewServer", "/JobViewServer", "org.kde.JobViewServerV2", "requestView",
        g_variant_new("(si@a{sv})", "org.nythera.FileManager", suspendable ? 3 : 1, g_variant_builder_end(&hints)),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error
    );
    if (response == nullptr) {
      g_clear_error(&error);
      return;
    }
    const gchar* objectPath = nullptr;
    g_variant_get(response, "(&o)", &objectPath);
    const std::string ownedPath(objectPath);
    g_variant_unref(response);
    m_jobPaths.emplace(operation.id, ownedPath);
    const guint sub = g_dbus_connection_signal_subscribe(
        m_bus, "org.kde.JobViewServer", "org.kde.JobViewV3", nullptr, ownedPath.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, jobSignal, this, nullptr
    );
    m_signalSubscriptions.emplace(operation.id, sub);
    if (operation.paused)
      publishJob(operation, hasRates, true, true);
  }
  void JobViewReporter::publishJob(
      const Operation& operation, bool hasRates, bool metricsOnly, bool suspensionAcknowledgement
  ) {
    const auto path = m_jobPaths.find(operation.id);
    if (m_bus == nullptr || !m_serviceAvailable || path == m_jobPaths.end())
      return;
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    if (hasRates) {
      g_variant_builder_add(
          &properties, "{sv}", "displaySpeed", g_variant_new_uint64(operation.displaySpeedBytesPerSecond)
      );
      g_variant_builder_add(&properties, "{sv}", "etaSpeed", g_variant_new_uint64(operation.etaBytesPerSecond));
    }
    if (suspensionAcknowledgement)
      g_variant_builder_add(&properties, "{sv}", "suspended", g_variant_new_boolean(operation.paused));
    if (!metricsOnly) {
      const char* title = operation.finished      ? operation.resultTitle.c_str()
          : operation.kind == Kind::Duplicate     ? "Duplicating items"
          : operation.kind == Kind::Copy          ? "Copying files"
          : operation.kind == Kind::Move          ? "Moving files"
          : operation.kind == Kind::Rename        ? "Renaming item"
          : operation.kind == Kind::Trash         ? "Moving to Trash"
          : operation.kind == Kind::TrashToHome   ? "Moving to Home Wastebin"
          : operation.kind == Kind::Delete        ? "Deleting items"
          : operation.kind == Kind::Restore       ? "Restoring items"
          : operation.kind == Kind::EmptyWastebin ? "Emptying Wastebin"
          : operation.kind == Kind::Properties    ? "Changing properties"
          : operation.kind == Kind::Undo          ? "Undoing file operation"
          : operation.kind == Kind::Redo          ? "Redoing file operation"
          : operation.kind == Kind::Compress      ? "Compressing archive"
          : operation.kind == Kind::Extract       ? "Extracting archive"
                                                  : "Creating item";
      const auto namedTitle = isArchive(operation.kind) && !operation.finished
          ? std::string(title)
              + " "
              + filemanager::sanitizeFilenameForDisplay(
                  quotedBasename(operation.kind == Kind::Compress ? operation.destination : operation.source)
              )
          : std::string(title);
      g_variant_builder_add(&properties, "{sv}", "title", g_variant_new_string(namedTitle.c_str()));
      g_variant_builder_add(
          &properties, "{sv}", "infoMessage",
          g_variant_new_string(operation.finished ? operation.resultDescription.c_str() : operation.status.c_str())
      );
      if (!isArchive(operation.kind))
        g_variant_builder_add(
            &properties, "{sv}", "percent", g_variant_new_uint32(static_cast<guint32>(operation.fraction * 100.0))
        );
      if (isArchive(operation.kind)) {
        g_variant_builder_add(&properties, "{sv}", "processedBytes", g_variant_new_uint64(operation.processedBytes));
        g_variant_builder_add(&properties, "{sv}", "processedItems", g_variant_new_uint64(operation.processedFiles));
      }
      if (operation.totalBytes > 0) {
        g_variant_builder_add(&properties, "{sv}", "processedBytes", g_variant_new_uint64(operation.processedBytes));
        g_variant_builder_add(&properties, "{sv}", "totalBytes", g_variant_new_uint64(operation.totalBytes));
        g_variant_builder_add(&properties, "{sv}", "speed", g_variant_new_uint64(operation.speedBytesPerSecond));
      }
      if (operation.totalFiles > 0) {
        g_variant_builder_add(&properties, "{sv}", "processedFiles", g_variant_new_uint64(operation.processedFiles));
        g_variant_builder_add(&properties, "{sv}", "totalFiles", g_variant_new_uint64(operation.totalFiles));
        g_variant_builder_add(&properties, "{sv}", "processedItems", g_variant_new_uint64(operation.processedFiles));
        g_variant_builder_add(&properties, "{sv}", "totalItems", g_variant_new_uint64(operation.totalFiles));
      }
      if (!operation.finished && isTransfer(operation.kind) && !operation.destination.empty()) {
        const auto destination = filemanager::sanitizeFilenameForDisplay(operation.destination);
        g_variant_builder_add(&properties, "{sv}", "descriptionLabel2", g_variant_new_string("Destination"));
        g_variant_builder_add(&properties, "{sv}", "descriptionValue2", g_variant_new_string(destination.c_str()));
      }
      if (!operation.destination.empty()) {
        char* uri = g_filename_to_uri(operation.destination.c_str(), nullptr, nullptr);
        if (uri != nullptr) {
          g_variant_builder_add(&properties, "{sv}", "destUrl", g_variant_new_string(uri));
          g_free(uri);
        }
      }
    } else {
      g_variant_builder_add(&properties, "{sv}", "infoMessage", g_variant_new_string(operation.status.c_str()));
    }
    g_dbus_connection_call(
        m_bus, "org.kde.JobViewServer", path->second.c_str(), "org.kde.JobViewV3", "update",
        g_variant_new("(@a{sv})", g_variant_builder_end(&properties)), nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
          GError* callError = nullptr;
          GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &callError);
          if (reply != nullptr)
            g_variant_unref(reply);
          g_clear_error(&callError);
        },
        nullptr
    );
  }
  void JobViewReporter::finishJob(const Operation& operation) {
    const auto path = m_jobPaths.find(operation.id);
    if (m_bus != nullptr && m_serviceAvailable && path != m_jobPaths.end()) {
      const guint subscription = m_signalSubscriptions[operation.id];
      if (subscription != 0)
        g_dbus_connection_signal_unsubscribe(m_bus, subscription);
      m_signalSubscriptions.erase(operation.id);
      GVariantBuilder hints;
      g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
      GError* error = nullptr;
      GVariant* reply = g_dbus_connection_call_sync(
          m_bus, "org.kde.JobViewServer", path->second.c_str(), "org.kde.JobViewV3", "terminate",
          g_variant_new(
              "(us@a{sv})",
              operation.succeeded       ? 0U
                  : operation.cancelled ? 1U
                                        : 2U,
              operation.status.c_str(), g_variant_builder_end(&hints)
          ),
          nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error
      );
      if (reply != nullptr)
        g_variant_unref(reply);
      g_clear_error(&error);
      m_jobPaths.erase(path);
    }
  }

} // namespace filemanager
