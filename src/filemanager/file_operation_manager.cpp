#include "filemanager/file_operation_manager.h"

#include "core/files/display_text.h"
#include "filemanager/archive/archive_backend.h"
#include "filemanager/job_view_reporter.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <mutex>
#include <ranges>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace {
  namespace engine = filemanager::file_operation_engine;

  using Kind = FileOperationManager::Kind;
  using filemanager::operationName;

  bool isCopy(Kind kind) { return kind == Kind::Copy || kind == Kind::Duplicate; }
  bool isTransfer(Kind kind) { return isCopy(kind) || kind == Kind::Move; }
  bool usesTransferControls(Kind kind) { return isTransfer(kind) || kind == Kind::TrashToHome; }
  bool isArchive(Kind kind) { return kind == Kind::Compress || kind == Kind::Extract; }
  bool canSuspend(Kind kind) { return usesTransferControls(kind) || isArchive(kind); }
  bool archivePath(const std::string& path) {
    return !path.contains('\0') && std::filesystem::path(path).is_absolute();
  }
  bool isReplay(Kind kind) { return kind == Kind::Undo || kind == Kind::Redo; }
  bool reportsProgress(Kind kind) {
    return isTransfer(kind)
        || isArchive(kind)
        || isReplay(kind)
        || kind == Kind::Trash
        || kind == Kind::TrashToHome
        || kind == Kind::Delete
        || kind == Kind::Restore
        || kind == Kind::EmptyWastebin
        || kind == Kind::Properties;
  }

  using engine::FileTransferPauseState;
  using engine::TransferMeasure;

  void postProgressIdle(FileOperationManager* owner, std::weak_ptr<bool> alive, std::array<std::uint64_t, 6> values) {
    struct ProgressReport {
      FileOperationManager* owner;
      std::weak_ptr<bool> alive;
      std::array<std::uint64_t, 6> values;
    };
    g_idle_add(
        [](gpointer data) -> gboolean {
          std::unique_ptr<ProgressReport> report(static_cast<ProgressReport*>(data));
          const auto locked = report->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          const auto& v = report->values;
          report->owner->updateProgress(v[0], v[1], v[2], v[3], v[4], v[5]);
          return G_SOURCE_REMOVE;
        },
        new ProgressReport{owner, std::move(alive), values}
    );
  }

  std::string basename(const std::string& path) { return std::filesystem::path(path).filename().string(); }

  std::string quotedBasename(const std::string& path) {
    const std::string name = basename(path);
    return name.empty() ? "item" : "\"" + name + "\"";
  }

  std::string displayDestination(const std::string& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return filemanager::sanitizeFilenameForDisplay(error ? path : absolute.native());
  }

  const char* transferVerb(Kind kind) {
    return kind == Kind::Move ? "moved" : kind == Kind::Duplicate ? "duplicated" : "copied";
  }

  const char* transferPreposition(Kind kind) { return kind == Kind::Duplicate ? " in " : " to "; }

  std::string publishedTransfer(const FileOperationManager::Operation& operation) {
    const bool hasFiles = operation.processedFiles != 0;
    const auto count = hasFiles ? operation.processedFiles : operation.completedEntries.size();
    if (count == 0)
      return {};
    const auto unit = hasFiles ? (count == 1 ? " file " : " files ") : (count == 1 ? " item " : " items ");
    return std::to_string(count) + unit + transferVerb(operation.kind);
  }

  std::string completedDescription(const FileOperationManager::Operation& operation) {
    const auto kind = operation.kind;
    const auto count = operation.requestedItemCount;
    const std::string quantity = std::to_string(count) + (count == 1 ? " item" : " items");
    switch (kind) {
    case Kind::Copy:
    case Kind::Duplicate:
    case Kind::Move:
      break;
    case FileOperationManager::Kind::Rename:
      return "Renamed " + quotedBasename(operation.source) + " to " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::CreateFolder:
      return "Created folder " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::CreateFile:
      return "Created file " + quotedBasename(operation.destination);
    case Kind::CreateSymlink:
      return "Created symlink " + quotedBasename(operation.destination);
    case Kind::Undo:
    case Kind::Redo:
      return std::string(operationName(kind)) + " completed for " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::Trash:
      return count == 1 ? "Moved " + quotedBasename(operation.source) + " to Trash" : "Moved " + quantity + " to Trash";
    case FileOperationManager::Kind::TrashToHome:
      return count == 1 ? "Moved " + quotedBasename(operation.source) + " to Home Wastebin"
                        : "Moved " + quantity + " to Home Wastebin";
    case FileOperationManager::Kind::Restore:
      return count == 1 ? "Restored " + quotedBasename(operation.source) : "Restored " + quantity;
    case FileOperationManager::Kind::EmptyWastebin:
      return "Wastebin emptied";
    case FileOperationManager::Kind::Properties:
      return operation.status;
    case FileOperationManager::Kind::Delete:
      return count == 1 ? "Permanently deleted " + quotedBasename(operation.source) : "Permanently deleted " + quantity;
    case Kind::Compress:
    case Kind::Extract:
      break;
    }
    return "File operation completed";
  }

  std::string failedDescription(const FileOperationManager::Operation& operation, std::string detail) {
    if (detail.find("Cannot copy a directory into itself") != std::string::npos)
      return "Cannot copy " + quotedBasename(operation.source) + " into itself";
    const auto replacePath = [&detail](const std::string& path) {
      if (path.empty() || path.size() < 2)
        return;
      std::size_t offset = 0;
      const std::string replacement = quotedBasename(path);
      while ((offset = detail.find(path, offset)) != std::string::npos) {
        detail.replace(offset, path.size(), replacement);
        offset += replacement.size();
      }
    };
    replacePath(operation.source);
    replacePath(operation.destination);
    constexpr std::size_t kMaxErrorDescription = 240;
    if (detail.size() > kMaxErrorDescription)
      detail = detail.substr(0, kMaxErrorDescription - 1) + "…";
    return detail;
  }

} // namespace

struct FileOperationManager::WorkerDrain {
  std::mutex mutex;
  std::condition_variable cv;
  std::size_t active = 0;
};

struct FileOperationManager::Result : engine::Result {
  bool historyBlocked = false;
  bool adminSkipSelected = false;
  std::string customTitle;
  bool outcomeUncertain = false;
  std::optional<std::size_t> requestedCount;
  std::size_t skippedEntries = 0;
  filemanager::PropertyResult metadata;
  std::optional<TrashFallbackOffer> trashFallback;
  std::vector<HistoryEntry> history;
};

struct FileOperationManager::Task {
  FileOperationManager* owner;
  std::uint64_t id;
  Kind kind;
  std::vector<std::string> sources;
  std::string destination;
  TransferMeasure totalMeasure;
  TransferMeasure completedMeasure;
  std::optional<filemanager::PropertyRequest> metadata;
  std::uint32_t uid = 0;
  std::function<void(const filemanager::PropertyResult&)> metadataCompleted;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> renameIdentity;
  std::function<void(bool, const std::string&)> renameCompleted;
  std::shared_ptr<FileTransferPauseState> pause;
  std::shared_ptr<bool> alive;
  std::shared_ptr<WorkerDrain> drain;
  ConflictResolver conflictResolver;
  WastebinBackend wastebinBackend;
  std::function<void(std::uint64_t, std::uint64_t)> boundary;
  bool keepBoth = false;
  std::function<std::vector<filemanager::AdminServerFrame>()> waitForAdminFrames;
  std::function<bool(std::uint64_t, const ConflictDecision&)> replyToAdminConflict;
  std::optional<HistoryEntry> replayEntry;
  std::optional<ArchiveOptions> archiveOptions;
  std::function<std::optional<std::string>(const std::string&, bool, GCancellable*)> archivePassword;
  TransferMeasure archiveProgress;
};

FileOperationManager::FileOperationManager(Changed changed, WastebinBackend wastebinBackend)
    : m_workerDrain(std::make_shared<WorkerDrain>()), m_changed(std::move(changed)),
      m_wastebinBackend(std::move(wastebinBackend)) {
  m_jobReporter = std::make_unique<filemanager::JobViewReporter>(
      [this](bool available) {
        for (const auto& operation : m_operations) {
          if (operation.finished)
            continue;
          if (available) {
            if (!reportsProgress(operation.kind) || m_jobRevealTimers.contains(operation.id))
              continue;
            registerJob(operation);
            publishJob(operation);
          }
          if (m_changed)
            m_changed(operation);
        }
      },
      [this](std::uint64_t id) { cancel(id); }, [this](std::uint64_t id) { pause(id); },
      [this](std::uint64_t id) { resume(id); }
  );
}
void FileOperationManager::shutdown() noexcept {
  if (m_alive)
    *m_alive = false;
  if (m_rateTimer) {
    g_source_remove(m_rateTimer);
    m_rateTimer = 0;
  }
  for (const auto& [id, cancellable] : m_cancellables) {
    (void)id;
    g_cancellable_cancel(cancellable);
  }
  for (const auto& [id, control] : m_adminControls) {
    (void)id;
    if (control.stop)
      control.stop();
  }
  for (const auto& [id, pause] : m_pauseStates) {
    (void)id;
    pause->cancel();
  }
  for (const auto& [id, timer] : m_jobRevealTimers) {
    (void)id;
    g_source_remove(timer);
  }
  m_jobRevealTimers.clear();
  m_jobReporter->shutdown();
  if (m_workerDrain) {
    std::unique_lock lock(m_workerDrain->mutex);
    m_workerDrain->cv.wait(lock, [this] { return m_workerDrain->active == 0; });
  }
  for (auto& [id, cancellable] : m_cancellables) {
    (void)id;
    g_cancellable_cancel(cancellable);
    g_object_unref(cancellable);
  }
  m_cancellables.clear();
  m_adminControls.clear();
  m_pauseStates.clear();
  m_activePaths.clear();
}
FileOperationManager::~FileOperationManager() { shutdown(); }
bool FileOperationManager::active() const noexcept {
  return std::ranges::any_of(m_operations, [](const Operation& op) { return !op.finished; });
}
bool FileOperationManager::usesMount(const std::filesystem::path& mountPath) const {
  if (mountPath.empty())
    return active();
  // Test path components, never string prefixes ("/media/a" is not "/media/ab").
  // Keep both lexical and canonical paths so symlink aliases and paths removed
  // during a move remain protected. Ancestors also cover recursive operations
  // whose directory trees contain another mount.
  auto contains = [](const auto& parent, const auto& child) {
    auto p = parent.begin();
    auto c = child.begin();
    for (; p != parent.end(); ++p, ++c)
      if (c == child.end() || *p != *c)
        return false;
    return true;
  };
  std::error_code error;
  const auto lexical = std::filesystem::absolute(mountPath, error).lexically_normal();
  if (error)
    return active();
  const auto canonical = std::filesystem::weakly_canonical(lexical, error);
  for (const auto& [id, paths] : m_activePaths) {
    (void)id;
    for (const auto& path : paths) {
      if (path.empty() || contains(lexical, path) || contains(path, lexical))
        return true;
      if (!error && (contains(canonical, path) || contains(path, canonical)))
        return true;
    }
  }
  return false;
}
bool FileOperationManager::hasJobService() const noexcept {
  if (!m_jobReporter->available())
    return false;
  return std::ranges::all_of(m_operations, [this](const Operation& operation) {
    return operation.finished
        || !reportsProgress(operation.kind)
        || !operation.progressRevealed
        || m_jobReporter->hasJob(operation.id);
  });
}
FileOperationManager::Operation* FileOperationManager::find(std::uint64_t id) {
  auto it = std::ranges::find(m_operations, id, &Operation::id);
  return it == m_operations.end() ? nullptr : &*it;
}
void FileOperationManager::update(std::uint64_t id, double fraction, std::string status) {
  if (auto* op = find(id); op && !op->finished) {
    op->fraction = fraction;
    op->status = std::move(status);
    publishJob(*op);
    if (m_changed)
      m_changed(*op);
  }
}
bool FileOperationManager::updateTransferMetrics(Operation& operation) {
  const auto found = m_transferRates.find(operation.id);
  if (found == m_transferRates.end())
    return false;
  found->second.observe(std::chrono::steady_clock::now(), operation.processedBytes, operation.paused);
  const auto display = found->second.displaySpeed();
  const auto eta = found->second.etaSpeed();
  const bool changed = display != operation.displaySpeedBytesPerSecond || eta != operation.etaBytesPerSecond;
  operation.displaySpeedBytesPerSecond = display;
  operation.etaBytesPerSecond = eta;
  if (operation.totalBytes > 0) {
    char* processed = g_format_size(operation.processedBytes);
    char* total = g_format_size(operation.totalBytes);
    char* speed = g_format_size(display);
    operation.status = std::string(processed)
        + " / "
        + total
        + " · "
        + std::to_string(operation.processedFiles)
        + " / "
        + std::to_string(operation.totalFiles)
        + (operation.adminOrigin ? " items" : " files")
        + (display > 0 ? std::string(" · ") + speed + "/s" : std::string{});
    g_free(processed);
    g_free(total);
    g_free(speed);
  }
  return changed;
}

void FileOperationManager::updateProgress(
    std::uint64_t id, std::uint64_t bytes, std::uint64_t totalBytes, std::uint64_t files, std::uint64_t totalFiles,
    std::uint64_t speed
) {
  if (auto* op = find(id); op && !op->finished) {
    op->processedBytes = bytes;
    op->totalBytes = totalBytes;
    op->processedFiles = files;
    op->totalFiles = totalFiles;
    op->speedBytesPerSecond = speed;
    if (totalBytes > 0)
      op->fraction = std::clamp(static_cast<double>(bytes) / static_cast<double>(totalBytes), 0.0, 1.0);
    else if (totalFiles > 0)
      op->fraction = std::clamp(static_cast<double>(files) / static_cast<double>(totalFiles), 0.0, 1.0);
    if (isArchive(op->kind)) {
      g_autofree gchar* processed = g_format_size(bytes);
      op->status = std::to_string(files) + (files == 1 ? " entry · " : " entries · ") + processed + " processed";
    } else if (totalBytes > 0) {
      updateTransferMetrics(*op);
    } else
      op->status = std::to_string(files) + " / " + std::to_string(totalFiles) + " items";
    publishJob(*op);
    if (m_changed)
      m_changed(*op);
  }
}
void FileOperationManager::cancel(std::uint64_t id) {
  if (const auto control = m_adminControls.find(id); control != m_adminControls.end() && control->second.cancel)
    (void)control->second.cancel();
  auto it = std::ranges::find(m_cancellables, id, &std::pair<std::uint64_t, GCancellable*>::first);
  if (it != m_cancellables.end())
    g_cancellable_cancel(it->second);
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end())
    state->second->cancel();
}
void FileOperationManager::pause(std::uint64_t id) {
  if (const auto control = m_adminControls.find(id); control != m_adminControls.end() && control->second.setPaused)
    (void)control->second.setPaused(true);
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end())
    state->second->pause();
}
void FileOperationManager::resume(std::uint64_t id) {
  if (const auto control = m_adminControls.find(id); control != m_adminControls.end() && control->second.setPaused)
    (void)control->second.setPaused(false);
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end())
    state->second->resume();
}
void FileOperationManager::updatePaused(std::uint64_t id, bool paused) {
  if (auto* operation = find(id); operation && !operation->finished) {
    operation->paused = paused;
    updateTransferMetrics(*operation);
    publishJob(*operation, true, true);
    if (m_changed)
      m_changed(*operation);
  }
}

bool FileOperationManager::administratorOperationActive(filemanager::TabId tabId) const {
  return std::ranges::any_of(m_operations, [tabId](const auto& operation) {
    return !operation.finished && operation.adminOrigin && operation.adminOrigin->tabId == tabId;
  });
}

std::uint64_t FileOperationManager::copy(
    std::vector<std::string> sources, std::string destination, std::optional<AdminOperationContext> admin
) {
  return start(Kind::Copy, std::move(sources), std::move(destination), {}, 0, {}, {}, {}, false, admin);
}
std::uint64_t FileOperationManager::duplicate(
    std::vector<std::string> sources, std::string destination, std::optional<AdminOperationContext> admin
) {
  return start(Kind::Duplicate, std::move(sources), std::move(destination), {}, 0, {}, {}, {}, false, admin);
}
std::uint64_t FileOperationManager::move(
    std::vector<std::string> sources, std::string destination, std::optional<AdminOperationContext> admin
) {
  return start(Kind::Move, std::move(sources), std::move(destination), {}, 0, {}, {}, {}, false, admin);
}
std::uint64_t FileOperationManager::compress(
    std::vector<std::string> sources, std::string output, filemanager::archive_backend::Format format,
    filemanager::archive_backend::Level level, filemanager::archive_backend::EncryptionOptions options
) {
  if (sources.empty() || !std::ranges::all_of(sources, archivePath) || !archivePath(output))
    return 0;
  return start(
      Kind::Compress, std::move(sources), std::move(output), {}, 0, {}, {}, {}, false, std::nullopt,
      ArchiveOptions{format, level, std::move(options)}
  );
}
std::uint64_t FileOperationManager::extract(
    std::string source, std::string destination, filemanager::archive_backend::Extraction mode
) {
  if (!archivePath(source) || !archivePath(destination))
    return 0;
  return start(
      Kind::Extract, {std::move(source)}, std::move(destination), {}, 0, {}, {}, {}, false, std::nullopt,
      ArchiveOptions{.extraction = mode}
  );
}
std::uint64_t FileOperationManager::extractSelected(
    filemanager::archive_backend::Selection selection, std::string destination, ArchivePassword password,
    std::function<void(const Operation&)> completion
) {
  if (!selection.catalogue || selection.members.empty() || !archivePath(destination))
    return 0;
  auto source = selection.catalogue->source.native();
  if (!archivePath(source))
    return 0;
  return start(
      Kind::Extract, {std::move(source)}, std::move(destination), {}, 0, {}, {}, {}, false, std::nullopt,
      ArchiveOptions{
          .selection = std::move(selection), .password = std::move(password), .completion = std::move(completion)
      }
  );
}
std::uint64_t FileOperationManager::rename(std::string source, std::string destination) {
  return start(Kind::Rename, {std::move(source)}, std::move(destination));
}
std::uint64_t FileOperationManager::renameChecked(
    std::string source, std::string destination, std::uint64_t device, std::uint64_t inode,
    std::function<void(bool, const std::string&)> completed, std::optional<AdminOperationContext> admin
) {
  return start(
      Kind::Rename, {std::move(source)}, std::move(destination), {}, 0, {}, std::pair{device, inode},
      std::move(completed), false, admin
  );
}

std::uint64_t FileOperationManager::createFolder(std::string destination, std::optional<AdminOperationContext> admin) {
  return start(Kind::CreateFolder, {}, std::move(destination), {}, 0, {}, {}, {}, admin.has_value(), admin);
}
std::uint64_t FileOperationManager::createFile(std::string destination, std::optional<AdminOperationContext> admin) {
  return start(Kind::CreateFile, {}, std::move(destination), {}, 0, {}, {}, {}, admin.has_value(), admin);
}

std::uint64_t FileOperationManager::createFileFromTemplate(
    std::string templateSource, std::string destination, std::optional<AdminOperationContext> admin
) {
  return start(
      Kind::CreateFile, {std::move(templateSource)}, std::move(destination), {}, 0, {}, {}, {}, admin.has_value(), admin
  );
}
std::uint64_t FileOperationManager::createSymlink(
    std::string target, std::string destination, bool keepBoth, std::optional<AdminOperationContext> admin
) {
  return start(Kind::CreateSymlink, {std::move(target)}, std::move(destination), {}, 0, {}, {}, {}, keepBoth, admin);
}

bool FileOperationManager::canUndo() const noexcept { return !active() && m_history.canReplay(true); }
bool FileOperationManager::canRedo() const noexcept { return !active() && m_history.canReplay(false); }

std::string FileOperationManager::undoLabel() const { return m_history.label(true); }
std::string FileOperationManager::redoLabel() const { return m_history.label(false); }
std::uint64_t FileOperationManager::undo() {
  if (!canUndo())
    return 0;
  return start(Kind::Undo, {m_history.next(true).source}, m_history.next(true).destination);
}

std::uint64_t FileOperationManager::redo() {
  if (!canRedo())
    return 0;
  return start(Kind::Redo, {m_history.next(false).source}, m_history.next(false).destination);
}
std::uint64_t FileOperationManager::trash(std::vector<std::string> sources) {
  return start(Kind::Trash, std::move(sources), {});
}
std::uint64_t FileOperationManager::trashToHomeWastebin(std::vector<std::string> sources) {
  return start(Kind::TrashToHome, std::move(sources), {});
}
std::uint64_t FileOperationManager::restore(std::vector<std::string> trashUris, std::string destinationDirectory) {
  return start(Kind::Restore, std::move(trashUris), std::move(destinationDirectory));
}
std::uint64_t FileOperationManager::emptyWastebin(bool confirmed) {
  if (!confirmed)
    return 0;
  return start(Kind::EmptyWastebin, {}, {});
}
std::uint64_t
FileOperationManager::removePermanently(std::vector<std::string> sources, std::optional<AdminOperationContext> admin) {
  return start(Kind::Delete, std::move(sources), {}, {}, 0, {}, {}, {}, false, admin);
}

std::uint64_t FileOperationManager::modifyProperties(
    filemanager::PropertyRequest request, std::uint32_t uid,
    std::function<void(const filemanager::PropertyResult&)> completed, std::optional<AdminOperationContext> admin
) {
  const auto identity = std::pair{request.device, request.inode};
  for (const auto& [id, target] : m_metadataTargets) {
    (void)id;
    if (target == identity)
      return 0;
  }
  const auto path = request.path.string();
  const auto id = start(Kind::Properties, {path}, {}, request, uid, std::move(completed), {}, {}, false, admin);
  if (id)
    m_metadataTargets[id] = identity;
  return id;
}

std::uint64_t FileOperationManager::start(
    Kind kind, std::vector<std::string> sources, std::string destination,
    std::optional<filemanager::PropertyRequest> metadata, std::uint32_t uid,
    std::function<void(const filemanager::PropertyResult&)> completedCallback,
    std::optional<std::pair<std::uint64_t, std::uint64_t>> renameIdentity,
    std::function<void(bool, const std::string&)> renameCompleted, bool keepBoth,
    std::optional<AdminOperationContext> admin, std::optional<ArchiveOptions> archiveOptions
) {
#ifndef NYTHERA_FILES_ARCHIVES
  if (isArchive(kind))
    return 0;
#endif
  if (m_history.replaying() || !m_alive || !*m_alive)
    return 0;
  const bool propertyEdit = kind == Kind::Properties && metadata.has_value();
  if (admin
      && (!admin->active
          || administratorOperationActive(admin->origin.tabId)
          || (!propertyEdit && (!admin->canMutate || !filemanager::localOperationKind(kind)))))
    return 0;
  std::optional<HistoryEntry> replayEntry;
  if (isReplay(kind))
    replayEntry = m_history.beginReplay(kind == Kind::Undo);
  const auto id = m_nextId++;
  Operation operation{
      .id = id,
      .kind = kind,
      .source = sources.empty() ? std::string{} : sources.front(),
      .destination = destination,
      .status = "Queued",
      .requestedItemCount = std::max<std::size_t>(1, sources.size())
  };
  if (kind == Kind::Properties)
    operation.status = "Preparing property changes / authorisation…";
  if (admin) {
    operation.status = "Preparing Administrator operation…";
    operation.adminOrigin = admin->origin;
  }
  m_operations.push_back(operation);
  auto& activePaths = m_activePaths[id];
  auto trackPath = [&](const std::string& rawPath) {
    if (rawPath.empty())
      return;
    std::error_code error;
    const auto lexical = std::filesystem::absolute(rawPath, error).lexically_normal();
    if (error) {
      activePaths.emplace_back();
      return;
    }
    activePaths.push_back(lexical);
    if (admin)
      return;
    const auto canonical = std::filesystem::weakly_canonical(lexical, error);
    if (!error && canonical != lexical)
      activePaths.push_back(canonical);
  };
  if (kind != Kind::Restore && kind != Kind::CreateSymlink)
    for (const auto& source : sources)
      trackPath(source);
  trackPath(destination);
  if (admin || kind == Kind::Restore || kind == Kind::EmptyWastebin || kind == Kind::TrashToHome)
    activePaths.emplace_back(); // Paths that the GUI cannot resolve require conservative mount protection.
  if (reportsProgress(kind)) {
    struct RevealRequest {
      FileOperationManager* self;
      std::weak_ptr<bool> alive;
      std::uint64_t operationId;
    };
    const auto timer = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 450,
        [](gpointer data) -> gboolean {
          auto* request = static_cast<RevealRequest*>(data);
          const auto locked = request->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          auto* self = request->self;
          const auto operationId = request->operationId;
          self->m_jobRevealTimers.erase(operationId);
          if (auto* activeOperation = self->find(operationId);
              activeOperation != nullptr && !activeOperation->finished) {
            activeOperation->progressRevealed = true;
            self->registerJob(*activeOperation);
            self->publishJob(*activeOperation);
            if (self->m_changed)
              self->m_changed(*activeOperation);
          }
          return G_SOURCE_REMOVE;
        },
        new RevealRequest{this, m_alive, id}, [](gpointer data) { delete static_cast<RevealRequest*>(data); }
    );
    m_jobRevealTimers.emplace(id, timer);
  }
  const auto alive = m_alive;
  if (m_changed && !admin)
    m_changed(m_operations.back());
  if (!alive || !*alive)
    return 0;
  auto* taskData = new Task{this, id, kind, std::move(sources), std::move(destination)};
  taskData->archiveOptions = std::move(archiveOptions);
  if (isArchive(kind))
    taskData->archivePassword = m_archivePasswordResolver;
  taskData->keepBoth = keepBoth;
  taskData->replayEntry = std::move(replayEntry);
  taskData->alive = m_alive;
  taskData->drain = m_workerDrain;
  taskData->conflictResolver = admin ? m_adminConflictResolver : m_conflictResolver;
  taskData->wastebinBackend = m_wastebinBackend;
  GCancellable* cancellable = g_cancellable_new();
  if (canSuspend(kind) && !admin) {
    taskData->pause = std::make_shared<FileTransferPauseState>();
    taskData->boundary = m_transferBoundary;
    m_pauseStates.emplace(id, taskData->pause);
  }
  if (usesTransferControls(kind)) {
    m_transferRates.emplace(id, filemanager::TransferRates{});
    struct RateTimerData {
      FileOperationManager* self;
      std::weak_ptr<bool> alive;
    };
    if (!m_rateTimer)
      m_rateTimer = g_timeout_add_full(
          G_PRIORITY_DEFAULT, 100,
          [](gpointer value) -> gboolean {
            auto* state = static_cast<RateTimerData*>(value);
            const auto locked = state->alive.lock();
            if (!locked || !*locked)
              return G_SOURCE_REMOVE;
            auto* self = state->self;
            bool active = false;
            for (std::size_t idx = 0; idx < self->m_operations.size(); ++idx) {
              auto& activeTransfer = self->m_operations[idx];
              if (activeTransfer.finished || !self->m_transferRates.contains(activeTransfer.id))
                continue;
              active = true;
              if (activeTransfer.totalBytes > 0 && self->updateTransferMetrics(activeTransfer)) {
                self->publishJob(activeTransfer, true);
                if (self->m_changed)
                  self->m_changed(activeTransfer);
                if (!*locked)
                  return G_SOURCE_REMOVE;
              }
            }
            if (!active)
              self->m_rateTimer = 0;
            return active ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
          },
          new RateTimerData{this, m_alive}, [](gpointer value) { delete static_cast<RateTimerData*>(value); }
      );
  }
  m_cancellables.emplace_back(id, G_CANCELLABLE(g_object_ref(cancellable)));
  taskData->renameIdentity = renameIdentity;
  taskData->renameCompleted = std::move(renameCompleted);
  taskData->metadata = std::move(metadata);
  taskData->uid = uid;
  taskData->metadataCompleted = std::move(completedCallback);
  GTask* task = g_task_new(nullptr, cancellable, completed, this);
  // Keep commit records when cancellation arrives after a filesystem effect.
  g_task_set_check_cancellable(task, false);
  // Completion releases mount protection. Cancellation must not dispatch it
  // early while the worker is still unwinding or cleaning staged copies.
  g_task_set_return_on_cancel(task, false);
  g_task_set_task_data(task, taskData, [](gpointer data) { delete static_cast<Task*>(data); });
  {
    std::lock_guard lock(m_workerDrain->mutex);
    ++m_workerDrain->active;
  }
  if (admin) {
    const auto request = propertyEdit
        ? engine::Request{.kind = engine::Kind::ModifyProperties, .destination = taskData->metadata->path.string(),
                          .properties = taskData->metadata}
        : engine::Request{*filemanager::localOperationKind(kind), taskData->sources, taskData->destination, renameIdentity, keepBoth};
    auto transport = admin->session.runOperation(request, operation.adminOrigin->sessionGeneration, cancellable);
    taskData->waitForAdminFrames = std::move(transport.waitForFrames);
    taskData->replyToAdminConflict = [reply = std::move(transport.sendConflictReply),
                                      cancel = transport.cancel](auto conflict, const auto& decision) {
      if (reply(conflict, decision))
        return true;
      (void)cancel();
      return false;
    };
    m_adminControls.emplace(id, std::move(transport));
  }
  g_task_run_in_thread(task, worker);
  if (admin && m_changed)
    m_changed(m_operations.back());
  g_object_unref(task);
  g_object_unref(cancellable);
  return id;
}

void FileOperationManager::registerJob(const Operation& operation) {
  m_jobReporter->registerJob(operation, canSuspend(operation.kind), m_transferRates.contains(operation.id));
}
void FileOperationManager::publishJob(const Operation& operation, bool metricsOnly, bool suspensionAcknowledgement) {
  m_jobReporter->publishJob(operation, m_transferRates.contains(operation.id), metricsOnly, suspensionAcknowledgement);
}
void FileOperationManager::finishJob(const Operation& operation) {
  if (const auto timer = m_jobRevealTimers.find(operation.id); timer != m_jobRevealTimers.end()) {
    g_source_remove(timer->second);
    m_jobRevealTimers.erase(timer);
  }
  m_jobReporter->finishJob(operation);
}

void FileOperationManager::worker(GTask* task, gpointer, gpointer taskData, GCancellable* cancellable) {
  auto* data = static_cast<Task*>(taskData);
  struct DrainGuard {
    std::shared_ptr<WorkerDrain> drain;
    ~DrainGuard() {
      if (!drain)
        return;
      std::lock_guard lock(drain->mutex);
      if (--drain->active == 0)
        drain->cv.notify_all();
    }
  } drainGuard{data->drain};
  auto* result = new Result{};
  if (data->kind == Kind::Properties && data->metadata && !data->waitForAdminFrames) {
    auto last = std::chrono::steady_clock::now();
    result->metadata =
        filemanager::runPropertyRequest(*data->metadata, data->uid, cancellable, [&](const auto& progress) {
          const auto now = std::chrono::steady_clock::now();
          if (now - last < std::chrono::milliseconds(100))
            return;
          last = now;
          struct Report {
            FileOperationManager* owner;
            std::uint64_t id;
            filemanager::PropertyResult progress;
            std::weak_ptr<bool> alive;
          };
          g_idle_add(
              [](gpointer value) -> gboolean {
                std::unique_ptr<Report> report(static_cast<Report*>(value));
                const auto alive = report->alive.lock();
                if (!alive || !*alive)
                  return G_SOURCE_REMOVE;
                report->owner->update(
                    report->id, 0.0,
                    report->progress.inspected
                        ? "Checking authorisation: " + std::to_string(report->progress.inspected) + " entries inspected"
                        : std::to_string(report->progress.succeeded)
                            + " succeeded, "
                            + std::to_string(report->progress.failed)
                            + " failed, "
                            + std::to_string(report->progress.skipped)
                            + " skipped"
                );
                return G_SOURCE_REMOVE;
              },
              new Report{data->owner, data->id, progress, data->alive}
          );
        });
    result->ok = result->metadata.ok();
    result->cancelled = result->metadata.cancelled;
    result->mutated = result->metadata.succeeded > 0 || result->metadata.ownershipEffects;
    result->message = result->metadata.summary();
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }
  GError* error = nullptr;
  auto fail = [&](const char* fallback) {
    result->ok = false;
    result->message = error == nullptr ? fallback : error->message;
    result->cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
  };
  if (data->replayEntry) {
    result->ok =
        filemanager::OperationHistory::replay(*data->replayEntry, data->kind == Kind::Undo, cancellable, &error);
    if (!result->ok) {
      result->historyBlocked = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
      fail("Could not replay this operation.");
    }
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }

  engine::TransferCallbacks transferCallbacks;
  transferCallbacks.report = [data](auto bytes, auto totalBytes, auto items, auto totalItems, auto speed) {
    postProgressIdle(data->owner, data->alive, {data->id, bytes, totalBytes, items, totalItems, speed});
  };
  if (data->boundary)
    transferCallbacks.boundary = [data](std::uint64_t bytes) { data->boundary(data->id, bytes); };
  transferCallbacks.acknowledgePause = [owner = data->owner, id = data->id,
                                        alive = std::weak_ptr<bool>(data->alive)](bool paused) {
    using Ack = std::tuple<FileOperationManager*, std::uint64_t, std::weak_ptr<bool>, bool>;
    g_idle_add(
        [](gpointer value) -> gboolean {
          std::unique_ptr<Ack> ack(static_cast<Ack*>(value));
          if (const auto locked = std::get<2>(*ack).lock(); locked && *locked)
            std::get<0>(*ack)->updatePaused(std::get<1>(*ack), std::get<3>(*ack));
          return G_SOURCE_REMOVE;
        },
        new Ack{owner, id, alive, paused}
    );
  };

  if (data->waitForAdminFrames) {
    using namespace filemanager;
    for (;;) {
      auto frames = data->waitForAdminFrames();
      for (auto& frame : frames) {
        switch (frame.kind) {
        case AdminServerFrameKind::OpProgress:
          transferCallbacks.report(
              frame.completedBytes, frame.totalBytes, frame.completedItems, frame.totalItems, frame.speed
          );
          data->completedMeasure = {frame.completedBytes, frame.completedItems};
          data->totalMeasure = {frame.totalBytes, frame.totalItems};
          break;
        case AdminServerFrameKind::OpPaused:
          transferCallbacks.acknowledgePause(frame.paused);
          break;
        case AdminServerFrameKind::OpConflict:
          if (!g_cancellable_is_cancelled(cancellable)) {
            const auto decision = data->conflictResolver
                ? data->conflictResolver(
                      frame.sourcePath, frame.destinationPath, frame.hasRemaining, frame.conflictMetadata, cancellable
                  )
                : ConflictDecision{};
            if (!g_cancellable_is_cancelled(cancellable)) {
              const bool replied = data->replyToAdminConflict(frame.conflictId, decision);
              result->adminSkipSelected |= replied && isTransfer(data->kind) && decision.action == ConflictAction::Skip;
            }
          }
          break;
        case AdminServerFrameKind::OpCompletedEntry:
          result->completedEntries.push_back(std::move(frame.completedEntry));
          break;
        case AdminServerFrameKind::OpMovedSource:
          result->movedSources.push_back(std::move(frame.sourcePath));
          break;
        case AdminServerFrameKind::OpResult:
          if (data->kind == Kind::Properties)
            result->metadata = frame.properties.value_or(
                filemanager::PropertyResult{
                    .cancelled = frame.status == AdminStatusCode::Cancelled, .error = frame.detail
                }
            );
          result->ok = frame.status == AdminStatusCode::Ok;
          result->cancelled = frame.status == AdminStatusCode::Cancelled;
          result->mutated = frame.mutated;
          result->message = std::move(frame.detail);
          result->customDescription = std::move(frame.customDescription);
          result->outcomeUncertain = frame.outcomeUncertain;
          if (frame.outcomeUncertain) {
            result->customTitle = std::string(operationName(data->kind)) + " outcome uncertain";
            result->customDescription = result->message;
          } else if (data->kind != Kind::Delete) {
            data->completedMeasure = {frame.completedBytes, frame.completedItems};
            data->totalMeasure = {frame.totalBytes, frame.totalItems};
          }
          if (result->customDescription.empty()) {
            if (data->kind == Kind::Delete
                && data->completedMeasure.items != data->sources.size()
                && (result->ok || data->completedMeasure.items > 0)) {
              result->customDescription =
                  std::format("{} of {} selected items deleted", data->completedMeasure.items, data->sources.size());
              if (!result->message.empty())
                result->customDescription += ". " + result->message;
            } else if (result->cancelled && !isTransfer(data->kind)) {
              result->customDescription = result->message;
            }
          }
          if (result->ok && !frame.destinationPath.empty())
            data->destination = std::move(frame.destinationPath);
          g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
          return;
        default:
          break;
        }
      }
    }
  }

#ifdef NYTHERA_FILES_ARCHIVES
  if (isArchive(data->kind)) {
    namespace archives = filemanager::archive_backend;
    const auto checkpoint = [&](const std::function<void(bool)>& helper) {
      if (!data->pause->checkpoint(cancellable, [&](bool paused) {
            if (helper)
              helper(paused);
            transferCallbacks.acknowledgePause(paused);
          }))
        g_cancellable_cancel(cancellable);
    };
    const archives::Callbacks callbacks{
        .resolveConflict = std::move(data->conflictResolver),
        .checkpoint =
            [&] {
              if (transferCallbacks.boundary)
                transferCallbacks.boundary(data->archiveProgress.bytes);
              checkpoint({});
            },
        .progress =
            [&](std::uint64_t bytes, std::uint64_t items) {
              data->archiveProgress = {.bytes = bytes, .items = items};
              transferCallbacks.report(bytes, 0, items, 0, 0);
            },
        .password = [&]() -> std::function<std::optional<std::string>(bool)> {
          if (data->archiveOptions->password)
            return [&](bool retry) { return data->archiveOptions->password(retry, cancellable); };
          if (data->archivePassword)
            return [&](bool retry) { return data->archivePassword(data->sources.front(), retry, cancellable); };
          return {};
        }(),
        .helperCheckpoint = checkpoint,
    };
    archives::Result archiveResult;
    if (data->kind == Kind::Compress) {
      std::vector<std::filesystem::path> sources(data->sources.begin(), data->sources.end());
      const auto& options = *data->archiveOptions;
      archiveResult = archives::compress(
          sources, data->destination, options.format, options.level, cancellable, callbacks, options.encryption
      );
      explicit_bzero(
          data->archiveOptions->encryption.password.data(), data->archiveOptions->encryption.password.size()
      );
      data->archiveOptions->encryption.password.clear();
      if (!archiveResult.completedEntries.empty())
        data->destination = archiveResult.completedEntries.front().path;
    } else if (data->archiveOptions->selection) {
      archiveResult =
          archives::extractSelected(*data->archiveOptions->selection, data->destination, cancellable, callbacks);
    } else {
      archiveResult = archives::extract(
          data->sources.front(), data->destination, cancellable, callbacks, data->archiveOptions->extraction
      );
    }
    result->skippedEntries = archiveResult.skippedEntries.size();
    data->archiveProgress = archiveResult.processedMeasure;
    static_cast<engine::Result&>(*result) = std::move(archiveResult);
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }
#endif
  if (const auto kind = filemanager::localOperationKind(data->kind)) {
    engine::Request request{
        *kind, std::move(data->sources), std::move(data->destination), data->renameIdentity, data->keepBoth
    };
    const engine::Callbacks callbacks{
        .transfer = std::move(transferCallbacks),
        .resolveConflict = std::move(data->conflictResolver),
        .recordChange = [data, result](
                            const auto& source, const auto& destination, const auto& sourceRevision,
                            const auto& resultRevision, auto sourceParent, auto destinationParent, auto linkTarget,
                            auto content
                        ) {
          filemanager::OperationHistory::record(
              result->history, data->kind, source, destination, sourceRevision, resultRevision, sourceParent,
              destinationParent, std::move(linkTarget), std::move(content)
          );
        },
    };
    static_cast<engine::Result&>(*result) =
        engine::executeLocalOperation(request, cancellable, data->pause.get(), callbacks);
    data->destination = std::move(request.destination);
    data->totalMeasure = result->totalMeasure;
    data->completedMeasure = result->completedMeasure;
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }

  auto wastebin = filemanager::wastebin_operations::execute(
      data->kind, data->sources, data->destination, data->wastebinBackend, data->conflictResolver, transferCallbacks,
      cancellable, data->pause.get()
  );
  data->totalMeasure = wastebin.totalMeasure;
  data->completedMeasure = wastebin.completedMeasure;
  result->customTitle = std::move(wastebin.customTitle);
  result->requestedCount = wastebin.requestedCount;
  result->skippedEntries = wastebin.skippedEntries;
  result->trashFallback = std::move(wastebin.trashFallback);
  static_cast<engine::Result&>(*result) = std::move(wastebin);
  g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
}

void FileOperationManager::completed(GObject*, GAsyncResult* asyncResult, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  GError* error = nullptr;
  std::unique_ptr<Result> result(static_cast<Result*>(g_task_propagate_pointer(G_TASK(asyncResult), &error)));
  auto* data = static_cast<Task*>(g_task_get_task_data(G_TASK(asyncResult)));
  const auto alive = data != nullptr ? data->alive : std::shared_ptr<bool>{};
  if (data != nullptr && alive && *alive) {
    auto* operation = self->find(data->id);
    if (operation != nullptr) {
      const bool clearArchiveSuspension = isArchive(data->kind) && operation->paused;
      operation->finished = true;
      operation->paused = false;
      self->m_pauseStates.erase(data->id);
      self->m_adminControls.erase(data->id);
      self->m_transferRates.erase(data->id);
      self->m_activePaths.erase(data->id);
      operation->succeeded = result && result->ok;
      operation->cancelled =
          result ? result->cancelled : error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      operation->outcomeUncertain = result && result->outcomeUncertain;
      operation->mutated = result && (result->mutated || (isReplay(data->kind) && result->ok));
      if (isReplay(data->kind)) {
        self->m_history.finishReplay(
            data->kind == Kind::Undo, operation->succeeded, result && result->historyBlocked, *data->replayEntry
        );
      } else if (result && result->mutated) {
        self->noteExternalMutation();
        operation->undoUnavailable = result->history.empty()
            && data->kind != Kind::Delete
            && data->kind != Kind::EmptyWastebin
            && data->kind != Kind::Properties;
        self->m_history.append(std::move(result->history));
      }
      if (data->kind == Kind::CreateSymlink || data->kind == Kind::Compress || operation->adminOrigin)
        operation->destination = data->destination;
      if (result) {
        operation->movedSources = std::move(result->movedSources);
        operation->trashFallback = std::move(result->trashFallback);
      }
      if (result && (isTransfer(data->kind) || isArchive(data->kind) || data->kind == Kind::CreateSymlink))
        operation->completedEntries = std::move(result->completedEntries);
      if (usesTransferControls(data->kind) || (operation->adminOrigin && data->kind == Kind::Delete)) {
        operation->processedBytes = data->completedMeasure.bytes;
        operation->totalBytes = data->totalMeasure.bytes;
        operation->processedFiles = data->completedMeasure.items;
        operation->totalFiles = data->totalMeasure.items;
        if (operation->totalBytes > 0)
          operation->fraction = std::clamp(
              static_cast<double>(operation->processedBytes) / static_cast<double>(operation->totalBytes), 0.0, 1.0
          );
      }
      operation->fraction = operation->succeeded ? 1.0 : operation->fraction;
      const std::string verb = operationName(data->kind);
      operation->resultTitle = verb
          + (operation->succeeded       ? " completed"
                 : operation->cancelled ? " cancelled"
                                        : " failed");
      if (result && result->requestedCount)
        operation->requestedItemCount = *result->requestedCount;
      if (result && !result->customTitle.empty())
        operation->resultTitle = result->customTitle;
      if (isTransfer(data->kind) && result && !result->outcomeUncertain) {
        const bool hasSkips = result->skippedTransferEntries != 0 || result->adminSkipSelected;
        const auto published = publishedTransfer(*operation);
        const auto destination = displayDestination(operation->destination);
        const std::string location = transferPreposition(data->kind) + destination;
        if (operation->succeeded) {
          if (hasSkips)
            operation->resultTitle = verb + (published.empty() ? " skipped" : " completed with skips");
          operation->resultDescription = !published.empty() ? published + location
              : hasSkips ? "No items " + std::string(transferVerb(data->kind)) + location
                         : "Transfer to " + destination + " completed";
        } else if (operation->cancelled) {
          operation->resultDescription = "Transfer to " + destination + " was cancelled";
          if (!published.empty())
            operation->resultDescription += " · " + published;
        } else {
          // Retain executor recovery diagnostics verbatim, including any preserved backup paths.
          operation->resultDescription = result->customDescription.empty()
              ? failedDescription(*operation, result->message)
              : result->customDescription;
          operation->resultDescription +=
              published.empty() ? " · Destination: " + destination : " · " + published + location;
        }
        if ((operation->succeeded || operation->cancelled) && !result->customDescription.empty())
          operation->resultDescription += " · " + result->customDescription;
        if (result->skippedTransferEntries != 0)
          operation->resultDescription += " · " + std::to_string(result->skippedTransferEntries) + " skipped";
        else if (result->adminSkipSelected && result->customDescription.empty())
          operation->resultDescription += " · Items skipped";
      } else if (result && !result->customDescription.empty()) {
        operation->resultDescription = result->customDescription;
        if (isTransfer(data->kind))
          operation->resultDescription += " · Destination: " + displayDestination(operation->destination);
      } else if (operation->succeeded)
        operation->resultDescription = completedDescription(*operation);
      else if (operation->cancelled)
        operation->resultDescription = verb
            + " of "
            + quotedBasename(operation->source.empty() ? operation->destination : operation->source)
            + " was cancelled";
      else {
        operation->resultDescription = failedDescription(
            *operation,
            result      ? result->message
                : error ? error->message
                        : "Operation failed"
        );
      }
      if (isArchive(data->kind)) {
        operation->processedBytes = data->archiveProgress.bytes;
        operation->processedFiles = data->archiveProgress.items;
        operation->skippedEntries = result ? result->skippedEntries : 0;
        const auto committed = operation->completedEntries.size();
        if (operation->succeeded) {
          if (operation->skippedEntries > 0)
            operation->resultTitle = verb + " completed with skips";
          operation->resultDescription = data->kind == Kind::Compress && committed > 0
              ? "Created archive " + quotedBasename(operation->destination)
              : std::to_string(committed) + (committed == 1 ? " entry published" : " entries published");
        } else {
          if (committed > 0) {
            operation->resultTitle += " after partial publication";
            operation->resultDescription +=
                " · " + std::to_string(committed) + (committed == 1 ? " entry published" : " entries published");
          }
        }
        if (operation->skippedEntries > 0)
          operation->resultDescription += " · " + std::to_string(operation->skippedEntries) + " skipped";
        operation->resultDescription = filemanager::sanitizeFilenameForDisplay(operation->resultDescription);
      }
      if (data->kind == Kind::Properties) {
        self->m_metadataTargets.erase(data->id);
        operation->resultDescription = result ? result->message : "Properties operation failed.";
      }
      operation->status = operation->resultDescription;
      if (operation->undoUnavailable)
        operation->status += " · Undo unavailable for this operation";
      if (!operation->succeeded && !operation->cancelled && !self->m_jobReporter->hasJob(operation->id))
        self->registerJob(*operation);
      self->publishJob(*operation, false, clearArchiveSuspension);
      self->finishJob(*operation);
      auto it = std::ranges::find(self->m_cancellables, data->id, &std::pair<std::uint64_t, GCancellable*>::first);
      if (it != self->m_cancellables.end()) {
        g_object_unref(it->second);
        self->m_cancellables.erase(it);
      }
      const auto operationId = data->id;
      const auto renameCompleted = data->renameCompleted;
      const auto renameSucceeded = operation->succeeded;
      const auto renameDescription = operation->resultDescription;
      if (data->kind == Kind::Properties && data->metadataCompleted && result) {
        data->metadataCompleted(result->metadata);
        if (!*alive) {
          g_clear_error(&error);
          return;
        }
      }
      if (renameCompleted) {
        renameCompleted(renameSucceeded, renameDescription);
        if (!*alive) {
          g_clear_error(&error);
          return;
        }
      }
      if (data->archiveOptions && data->archiveOptions->completion) {
        data->archiveOptions->completion(*operation);
        if (!*alive) {
          g_clear_error(&error);
          return;
        }
      }
      if (auto* currentOperation = self->find(operationId); currentOperation != nullptr && self->m_changed)
        self->m_changed(*currentOperation);
    } else {
      auto it = std::ranges::find(self->m_cancellables, data->id, &std::pair<std::uint64_t, GCancellable*>::first);
      if (it != self->m_cancellables.end()) {
        g_object_unref(it->second);
        self->m_cancellables.erase(it);
      }
    }
  }
  g_clear_error(&error);
}
