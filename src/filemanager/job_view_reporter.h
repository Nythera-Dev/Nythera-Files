#pragma once

#include "filemanager/file_operation_manager.h"

namespace filemanager {
  class JobViewReporter {
  public:
    using Operation = FileOperationManager::Operation;
    using Control = std::function<void(std::uint64_t)>;
    JobViewReporter(std::function<void(bool)> serviceChanged, Control cancel, Control pause, Control resume);
    ~JobViewReporter();
    JobViewReporter(const JobViewReporter&) = delete;
    JobViewReporter& operator=(const JobViewReporter&) = delete;

    void shutdown() noexcept;
    [[nodiscard]] bool available() const noexcept { return m_serviceAvailable; }
    [[nodiscard]] bool hasJob(std::uint64_t id) const { return m_jobPaths.contains(id); }
    void registerJob(const Operation& operation, bool suspendable, bool hasRates);
    void publishJob(
        const Operation& operation, bool hasRates, bool metricsOnly = false, bool suspensionAcknowledgement = false
    );
    void finishJob(const Operation& operation);

  private:
    friend class ::FileOperationManagerTestAccess;
    static void serviceAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer);
    static void serviceVanished(GDBusConnection*, const gchar*, gpointer);
    static void
    jobSignal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer);

    std::function<void(bool)> m_serviceChanged;
    Control m_cancel;
    Control m_pause;
    Control m_resume;
    GDBusConnection* m_bus = nullptr;
    guint m_serviceWatch = 0;
    bool m_serviceAvailable = false;
    std::unordered_map<std::uint64_t, std::string> m_jobPaths;
    std::unordered_map<std::uint64_t, guint> m_signalSubscriptions;
  };
} // namespace filemanager
