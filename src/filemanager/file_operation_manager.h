#pragma once

#include "filemanager/admin_session_client.h"
#include "filemanager/archive/archive_backend.h"
#include "filemanager/file_operation_engine.h"
#include "filemanager/operation_history.h"
#include "filemanager/property_operations.h"
#include "filemanager/tab_presentation.h"
#include "filemanager/transfer_rates.h"
#include "filemanager/wastebin_operations.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Local file operations run on GTask workers; callbacks return on the caller's
// GLib context. The manager deliberately owns operation state independently of
// the file browser's transient row indices.
class FileOperationManagerTestAccess;

namespace filemanager {
  class JobViewReporter;
}

class FileOperationManager {
public:
  using Kind = filemanager::FileOperationKind;
  using ConflictAction = filemanager::file_operation_engine::ConflictAction;
  using TrashFallbackOffer = filemanager::wastebin_operations::FallbackOffer;
  using WastebinBackend = filemanager::wastebin_operations::Backend;
  using ConflictDecision = filemanager::file_operation_engine::ConflictDecision;
  using CompletedEntry = filemanager::file_operation_engine::CompletedEntry;
  struct AdminOperationOrigin {
    filemanager::TabId tabId;
    std::uint64_t sessionGeneration;
    std::uint64_t navigation;
    std::filesystem::path directory;
  };
  struct AdminOperationContext {
    AdminOperationOrigin origin;
    filemanager::AdminSessionClient& session;
    bool active;
    bool canMutate;
  };
  struct Operation {
    std::uint64_t id = 0;
    Kind kind = Kind::Copy;
    std::string source;
    std::string destination;
    std::string status;
    std::string resultTitle;
    std::string resultDescription;
    std::vector<CompletedEntry> completedEntries;
    std::vector<std::string> movedSources;
    std::optional<TrashFallbackOffer> trashFallback;
    std::optional<AdminOperationOrigin> adminOrigin;
    std::size_t requestedItemCount = 0;
    std::size_t skippedEntries = 0;
    double fraction = 0.0;
    std::uint64_t processedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t processedFiles = 0;
    std::uint64_t totalFiles = 0;
    std::uint64_t speedBytesPerSecond = 0;
    std::uint64_t displaySpeedBytesPerSecond = 0;
    std::uint64_t etaBytesPerSecond = 0;
    bool paused = false;
    bool finished = false;
    bool succeeded = false;
    bool cancelled = false;
    bool mutated = false;
    bool outcomeUncertain = false;
    bool progressRevealed = false;
    bool undoUnavailable = false;
  };
  using Changed = std::function<void(const Operation&)>;
  using ConflictResolver = filemanager::file_operation_engine::ConflictResolver;
  explicit FileOperationManager(Changed changed = {}, WastebinBackend wastebinBackend = {});
  ~FileOperationManager();
  void shutdown() noexcept;
  FileOperationManager(const FileOperationManager&) = delete;
  FileOperationManager& operator=(const FileOperationManager&) = delete;

  std::uint64_t copy(
      std::vector<std::string> sources, std::string destinationDirectory,
      std::optional<AdminOperationContext> admin = std::nullopt
  );
  std::uint64_t duplicate(
      std::vector<std::string> sources, std::string destinationDirectory,
      std::optional<AdminOperationContext> admin = std::nullopt
  );
  std::uint64_t move(
      std::vector<std::string> sources, std::string destinationDirectory,
      std::optional<AdminOperationContext> admin = std::nullopt
  );
  std::uint64_t compress(
      std::vector<std::string> sources, std::string output, filemanager::archive_backend::Format format,
      filemanager::archive_backend::Level level, filemanager::archive_backend::EncryptionOptions options = {}
  );
  std::uint64_t extract(
      std::string source, std::string destinationDirectory,
      filemanager::archive_backend::Extraction mode = filemanager::archive_backend::Extraction::Direct
  );
  using ArchivePassword = std::function<std::optional<std::string>(bool, GCancellable*)>;
  std::uint64_t extractSelected(
      filemanager::archive_backend::Selection selection, std::string destination, ArchivePassword password,
      std::function<void(const Operation&)> completion = {}
  );
  void setArchivePasswordResolver(
      std::function<std::optional<std::string>(const std::string&, bool, GCancellable*)> resolver
  ) {
    m_archivePasswordResolver = std::move(resolver);
  }
  std::uint64_t rename(std::string source, std::string destination);
  std::uint64_t renameChecked(
      std::string source, std::string destination, std::uint64_t device, std::uint64_t inode,
      std::function<void(bool, const std::string&)> completed, std::optional<AdminOperationContext> admin = std::nullopt
  );
  std::uint64_t createFolder(std::string destination, std::optional<AdminOperationContext> admin = std::nullopt);
  std::uint64_t createFile(std::string destination, std::optional<AdminOperationContext> admin = std::nullopt);
  std::uint64_t createFileFromTemplate(
      std::string templateSource, std::string destination, std::optional<AdminOperationContext> admin = std::nullopt
  );
  std::uint64_t createSymlink(
      std::string target, std::string destination, bool keepBoth = false,
      std::optional<AdminOperationContext> admin = std::nullopt
  );
  std::uint64_t trash(std::vector<std::string> sources);
  std::uint64_t trashToHomeWastebin(std::vector<std::string> sources);
  std::uint64_t restore(std::vector<std::string> trashUris, std::string destinationDirectory = {});
  std::uint64_t emptyWastebin(bool confirmed);
  std::uint64_t
  removePermanently(std::vector<std::string> sources, std::optional<AdminOperationContext> admin = std::nullopt);
  std::uint64_t modifyProperties(
      filemanager::PropertyRequest request, std::uint32_t uid,
      std::function<void(const filemanager::PropertyResult&)> completed,
      std::optional<AdminOperationContext> admin = std::nullopt
  );
  [[nodiscard]] bool administratorOperationActive(filemanager::TabId tabId) const;
  void cancel(std::uint64_t id);
  void pause(std::uint64_t id);
  void resume(std::uint64_t id);
  void updatePaused(std::uint64_t id, bool paused);
  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] bool canRedo() const noexcept;
  [[nodiscard]] bool replaying() const noexcept { return m_history.replaying(); }
  [[nodiscard]] std::string undoLabel() const;
  [[nodiscard]] std::string redoLabel() const;
  std::uint64_t undo();
  std::uint64_t redo();
  void noteExternalMutation() { m_history.clearRedo(); }
  void setConflictResolver(ConflictResolver resolver) { m_conflictResolver = std::move(resolver); }
  void setAdminConflictResolver(ConflictResolver resolver) { m_adminConflictResolver = std::move(resolver); }
  [[nodiscard]] const std::vector<Operation>& operations() const noexcept { return m_operations; }
  [[nodiscard]] bool active() const noexcept;
  // Remains true through cancellation until the worker has actually returned.
  [[nodiscard]] bool usesMount(const std::filesystem::path& mountPath) const;
  [[nodiscard]] bool hasJobService() const noexcept;
  void updateProgress(
      std::uint64_t id, std::uint64_t bytes, std::uint64_t totalBytes, std::uint64_t files, std::uint64_t totalFiles,
      std::uint64_t speed
  );

private:
  friend class FileOperationManagerTestAccess;
  struct WorkerDrain;
  struct Task;
  struct Result;
  struct ArchiveOptions {
    filemanager::archive_backend::Format format = filemanager::archive_backend::Format::TarGzip;
    filemanager::archive_backend::Level level = filemanager::archive_backend::Level::Balanced;
    filemanager::archive_backend::EncryptionOptions encryption;
    filemanager::archive_backend::Extraction extraction = filemanager::archive_backend::Extraction::Direct;
    std::optional<filemanager::archive_backend::Selection> selection;
    ArchivePassword password;
    std::function<void(const Operation&)> completion;
  };
  using HistoryEntry = filemanager::OperationHistory::Entry;
  std::uint64_t start(
      Kind kind, std::vector<std::string> sources, std::string destination,
      std::optional<filemanager::PropertyRequest> metadata = {}, std::uint32_t uid = 0,
      std::function<void(const filemanager::PropertyResult&)> completed = {},
      std::optional<std::pair<std::uint64_t, std::uint64_t>> renameIdentity = {},
      std::function<void(bool, const std::string&)> renameCompleted = {}, bool keepBoth = false,
      std::optional<AdminOperationContext> admin = std::nullopt, std::optional<ArchiveOptions> archiveOptions = {}
  );
  static void worker(GTask* task, gpointer source, gpointer taskData, GCancellable* cancellable);
  static void completed(GObject* source, GAsyncResult* result, gpointer userData);
  void registerJob(const Operation& operation);
  void publishJob(const Operation& operation, bool metricsOnly = false, bool suspensionAcknowledgement = false);
  bool updateTransferMetrics(Operation& operation);
  void finishJob(const Operation& operation);
  void update(std::uint64_t id, double fraction, std::string status);
  Operation* find(std::uint64_t id);

  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  std::shared_ptr<WorkerDrain> m_workerDrain;
  Changed m_changed;
  ConflictResolver m_conflictResolver;
  ConflictResolver m_adminConflictResolver;
  std::function<std::optional<std::string>(const std::string&, bool, GCancellable*)> m_archivePasswordResolver;
  WastebinBackend m_wastebinBackend;
  std::vector<Operation> m_operations;
  filemanager::OperationHistory m_history;
  std::unordered_map<std::uint64_t, std::vector<std::filesystem::path>> m_activePaths;
  std::vector<std::pair<std::uint64_t, GCancellable*>> m_cancellables;
  std::uint64_t m_nextId = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<filemanager::file_operation_engine::FileTransferPauseState>> m_pauseStates;
  std::unordered_map<std::uint64_t, filemanager::TransferRates> m_transferRates;
  std::unordered_map<std::uint64_t, filemanager::AdminSessionClient::OperationTransport> m_adminControls;
  guint m_rateTimer = 0;
  // Isolated tests coordinate a real GIO copy at an I/O boundary.
  std::function<void(std::uint64_t, std::uint64_t)> m_transferBoundary;
  std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> m_metadataTargets;
  std::unordered_map<std::uint64_t, guint> m_jobRevealTimers;
  std::unique_ptr<filemanager::JobViewReporter> m_jobReporter;
};
