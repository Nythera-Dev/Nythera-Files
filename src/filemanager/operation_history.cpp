#include "filemanager/operation_history.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace filemanager {
  namespace {
    namespace engine = file_operation_engine;
    using namespace engine;
    using Kind = FileOperationKind;
    using Revision = engine::Revision;
    std::string entryBasename(const std::string& path) { return std::filesystem::path(path).filename().string(); }
    bool isCopy(Kind kind) { return kind == Kind::Copy || kind == Kind::Duplicate; }
    bool isTransfer(Kind kind) { return isCopy(kind) || kind == Kind::Move; }
    // Replay is keyed to the recorded directory as well as the entry. Pin it
    // before validation so a concurrent rename or retargeted parent symlink
    // cannot redirect a later filesystem call to a different directory.
    struct ReplayParent {
      int fd = -1;
      std::filesystem::path path;
      std::filesystem::path logicalDirectory;
      explicit ReplayParent(const std::string& entry, FileIdentity expected) {
        if (expected == FileIdentity{})
          return;
        const auto directory = std::filesystem::path(entry).parent_path();
        fd = open(directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
          return;
        struct stat state{};
        if (fstat(fd, &state) == 0 && FileIdentity{state.st_dev, state.st_ino} == expected) {
          path = "/proc/self/fd/" + std::to_string(fd);
          logicalDirectory = directory;
        }
      }
      ReplayParent(const ReplayParent&) = delete;
      ReplayParent& operator=(const ReplayParent&) = delete;
      ~ReplayParent() {
        if (fd >= 0)
          close(fd);
      }
    };

    bool changedEntry(GError** error, const char* message) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, message);
      return false;
    }

    bool emptyDirectory(const std::string& path) {
      std::error_code error;
      return std::filesystem::is_empty(path, error) && !error;
    }

  } // namespace
  using namespace file_operation_engine;

  bool OperationHistory::replay(Entry& entry, bool undoing, GCancellable* cancellable, GError** error) {
    const bool moving = entry.kind == Kind::Move || entry.kind == Kind::Rename;
    const bool copying = isCopy(entry.kind);
    const bool readsSource = moving || copying;
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
      return false;
    const ReplayParent destinationParent(entry.destination, entry.destinationParent);
    const ReplayParent sourceParent(entry.source, readsSource ? entry.sourceParent : FileIdentity{});
    if (destinationParent.path.empty() || (readsSource && sourceParent.path.empty()))
      return reject(error, "An original folder is missing or was replaced.");
    const auto destinationLeaf = entryBasename(entry.destination);
    const auto destinationPath = (destinationParent.path / destinationLeaf).string();
    const auto destinationLogicalPath = (destinationParent.logicalDirectory / destinationLeaf).string();
    const auto sourceLeaf = readsSource ? entryBasename(entry.source) : std::string{};
    const auto sourcePath = readsSource ? (sourceParent.path / sourceLeaf).string() : std::string{};
    const auto sourceLogicalPath = readsSource ? (sourceParent.logicalDirectory / sourceLeaf).string() : std::string{};

    // Moving updates ctime. Require the entire fingerprint before moving and
    // recheck everything except that timestamp after the no-overwrite rename.
    const auto afterMove = [](std::optional<Revision> actual, Revision expected) {
      if (actual)
        expected.changed = actual->changed;
      return actual == expected;
    };
    const auto expectedAt = [](const std::string& path, const Revision& expected) {
      return revision(path) == expected && (!S_ISDIR(expected.mode) || emptyDirectory(path));
    };
    const auto restoreMovedItem = [&](const std::string& from, const std::string& to, const std::string& location,
                                      const std::string& fromHook, const std::string& toHook, int fromFd, int toFd) {
      const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
          from.c_str(), to.c_str(), false, std::nullopt, true, fromHook.c_str(), toHook.c_str(), fromFd, toFd
      );
      if (outcome.ok)
        return changedEntry(error, "The item changed during the operation and was put back.");
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, "The item could not be restored and remains at %s.",
          location.c_str()
      );
      return false;
    };
    if (moving) {
      const auto& from = undoing ? destinationPath : sourcePath;
      const auto& to = undoing ? sourcePath : destinationPath;
      const auto& fromHook = undoing ? destinationLogicalPath : sourceLogicalPath;
      const auto& toHook = undoing ? sourceLogicalPath : destinationLogicalPath;
      const int fromFd = undoing ? destinationParent.fd : sourceParent.fd;
      const int toFd = undoing ? sourceParent.fd : destinationParent.fd;
      const auto& expected = undoing ? entry.resultRevision : *entry.sourceRevision;
      if (!expectedAt(from, expected))
        return changedEntry(error, "The item is missing, replaced, or has been modified.");
      if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return false;
      const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
          from.c_str(), to.c_str(), false, toCompatIdentity(expected), S_ISLNK(expected.mode), fromHook.c_str(),
          toHook.c_str(), fromFd, toFd
      );
      if (outcome.splitState)
        return changedEntry(error, outcome.message.c_str());
      if (!outcome.ok) {
        setErrorFromPublishOutcome(outcome, error);
        return false;
      }
      const auto actual = revision(to);
      if (!afterMove(actual, expected) || (S_ISDIR(expected.mode) && !emptyDirectory(to)))
        return restoreMovedItem(to, from, undoing ? entry.source : entry.destination, toHook, fromHook, toFd, fromFd);
      if (undoing)
        entry.sourceRevision = actual;
      else
        entry.resultRevision = *actual;
      return true;
    }

    if (undoing && !expectedAt(destinationPath, entry.resultRevision))
      return changedEntry(error, "The result is missing, replaced, or has been modified.");
    if (copying && !expectedAt(sourcePath, *entry.sourceRevision))
      return changedEntry(error, "The original source is missing, replaced, or has been modified.");

    if (!undoing && entry.kind == Kind::CreateFolder) {
      const auto caps = fs_compat::probeCapabilities(destinationParent.logicalDirectory);
      if (caps.renameNoReplace == fs_compat::CapabilitySupport::Unsupported) {
        int mkdirErr = 0;
        if (!fs_compat::createDirectoryExclusiveAt(
                destinationParent.fd, destinationLeaf.c_str(), entry.resultRevision.mode & 07777, mkdirErr, error
            )) {
          return false;
        }
        const auto created = revision(destinationPath);
        if (!created || created->mode != entry.resultRevision.mode) {
          unlinkat(destinationParent.fd, destinationLeaf.c_str(), AT_REMOVEDIR);
          return reject(error, "The recreated item's permissions differ from the original result.");
        }
        entry.resultRevision = *created;
        return true;
      }
    }

    // Isolate an output before deleting it. If the directory entry changed
    // between validation and rename, recover it without overwriting anything.
    // The private directory also gives redo a no-overwrite publication point.
    PrivateStaging staging(destinationPath);
    if (staging.directory.empty())
      return reject(error, "Could not create a private staging directory beside the destination.");
    const std::string stagingFolder = entryBasename(staging.directory);
    const std::string staged = staging.directory + "/item";
    const std::string stagedHook = (destinationParent.logicalDirectory / stagingFolder / "item").string();
    if (undoing) {
      const auto recovery = (std::filesystem::path(entry.destination).parent_path() / stagingFolder / "item").string();
      if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return false;
      bool isolated = false;
      int renameErr = 0;
      GError* noReplaceErr = nullptr;
      if (fs_compat::renameNoReplaceRaw(destinationLogicalPath.c_str(), stagedHook.c_str(), renameErr, &noReplaceErr)) {
        isolated = true;
      } else if (fs_compat::isNoReplaceUnsupportedErrno(renameErr)) {
        g_clear_error(&noReplaceErr);
        // staging.directory was created exclusively with mkdtemp (0700), so "item" is absent.
        const int stagingFd = open(staging.directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (stagingFd >= 0) {
          if (renameat(destinationParent.fd, destinationLeaf.c_str(), stagingFd, "item") == 0)
            isolated = true;
          else
            g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(errno), g_strerror(errno));
          close(stagingFd);
        } else {
          g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(errno), g_strerror(errno));
        }
      } else {
        g_propagate_error(error, noReplaceErr);
      }
      if (!isolated)
        return false;
      const auto actual = revision(staged);
      if (!afterMove(actual, entry.resultRevision) || (copying && !expectedAt(sourcePath, *entry.sourceRevision)))
        return restoreMovedItem(
            staged, destinationPath, recovery, stagedHook, destinationLogicalPath, -1, destinationParent.fd
        );
      const int removed = S_ISDIR(entry.resultRevision.mode) ? rmdir(staged.c_str()) : unlink(staged.c_str());
      return removed == 0
          || restoreMovedItem(
                 staged, destinationPath, recovery, stagedHook, destinationLogicalPath, -1, destinationParent.fd
          );
    }

    GFile* target = g_file_new_for_path(staged.c_str());
    bool ok = false;
    bool destinationCommitted = false;
    if (copying) {
      GFile* source = g_file_new_for_path(sourcePath.c_str());
      ok = copyTree(source, target, cancellable, error, nullptr);
      g_object_unref(source);
      if (ok && !expectedAt(sourcePath, *entry.sourceRevision))
        ok = changedEntry(error, "The source changed during copying.");
    } else {
      bool created = false;
      ok = engine::createItem(
          *localOperationKind(entry.kind), target, entry.linkTarget, entry.createdContent, {}, cancellable, error,
          created
      );
    }
    if (ok) {
      const auto created = revision(staged);
      if (!created || created->mode != entry.resultRevision.mode)
        ok = reject(error, "The recreated item's permissions differ from the original result.");
      else if (!g_cancellable_set_error_if_cancelled(cancellable, error)) {
        const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
            staged.c_str(), destinationPath.c_str(), true, toCompatIdentity(*created), true, stagedHook.c_str(),
            destinationLogicalPath.c_str(), -1, destinationParent.fd
        );
        ok = outcome.ok;
        destinationCommitted = outcome.destinationCommitted;
        if (!ok) {
          if (outcome.splitState)
            reject(error, outcome.message.c_str());
          else
            setErrorFromPublishOutcome(outcome, error);
        }
      } else {
        ok = false;
      }
      if (ok) {
        const auto actual = revision(destinationPath);
        entry.resultRevision = afterMove(actual, *created) ? *actual : Revision{};
      }
    }
    if (!destinationCommitted)
      g_file_delete(target, nullptr, nullptr);
    g_object_unref(target);
    return ok;
  }

  bool OperationHistory::canReplay(bool undoing) const noexcept {
    const auto& stack = undoing ? m_undo : m_redo;
    return !m_replaying && !stack.empty() && !stack.back().blocked;
  }

  const OperationHistory::Entry& OperationHistory::next(bool undoing) const {
    return (undoing ? m_undo : m_redo).back();
  }

  std::string OperationHistory::label(bool undoing) const {
    const auto& stack = undoing ? m_undo : m_redo;
    const std::string action = undoing ? "Undo" : "Redo";
    if (stack.empty() || stack.back().blocked)
      return action;
    const auto& entry = stack.back();
    const auto name = entryBasename(entry.destination);
    const auto target = name.empty() ? std::string("item") : "\"" + name + "\"";
    return action + " " + operationName(entry.kind) + " " + target;
  }

  OperationHistory::Entry OperationHistory::beginReplay(bool undoing) {
    m_replaying = true;
    return next(undoing);
  }

  void OperationHistory::finishReplay(bool undoing, bool succeeded, bool blocked, Entry& after) {
    m_replaying = false;
    auto& from = undoing ? m_undo : m_redo;
    auto& to = undoing ? m_redo : m_undo;
    if (succeeded) {
      auto before = std::move(from.back());
      from.pop_back();
      // Rebase only exact matches to the state this replay replaced.
      // An independently modified item keeps its stale fingerprint.
      const auto rebase = [&](const std::string& path, const Revision& oldState, const Revision& newState) {
        for (auto* stack : {&m_undo, &m_redo}) {
          for (auto& previous : *stack) {
            if (previous.source == path && previous.sourceRevision == oldState)
              previous.sourceRevision = newState;
            if (previous.destination == path && previous.resultRevision == oldState)
              previous.resultRevision = newState;
          }
        }
      };
      if (undoing && (after.kind == Kind::Move || after.kind == Kind::Rename))
        rebase(after.source, *before.sourceRevision, *after.sourceRevision);
      if (!undoing && after.resultRevision.inode)
        rebase(after.destination, before.resultRevision, after.resultRevision);
      if (after.resultRevision.inode)
        to.push_back(std::move(after));
    } else if (blocked) {
      from.back().blocked = true;
    }
  }

  void OperationHistory::append(std::vector<Entry> entries) {
    constexpr std::size_t historyLimit = 32;
    for (auto& entry : entries) {
      if (m_undo.size() == historyLimit)
        m_undo.erase(m_undo.begin());
      m_undo.push_back(std::move(entry));
    }
  }

  void OperationHistory::record(
      std::vector<Entry>& entries, Kind kind, const std::string& source, const std::string& destination,
      const std::optional<Revision>& sourceRevision, const std::optional<Revision>& resultRevision,
      FileIdentity sourceParent, FileIdentity destinationParent, std::string linkTarget,
      std::optional<std::vector<std::uint8_t>> createdContent
  ) {
    if (!resultRevision
        || destinationParent == FileIdentity{}
        || parentIdentity(destination) != destinationParent
        || (!source.empty()
            && (sourceParent == FileIdentity{} || !sourceRevision || parentIdentity(source) != sourceParent)))
      return;
    const auto& state = *resultRevision;
    if ((!S_ISREG(state.mode) && !S_ISLNK(state.mode) && !S_ISDIR(state.mode))
        || state.links > (S_ISDIR(state.mode) ? 2U : 1U))
      return;
    if (S_ISDIR(state.mode) && (isTransfer(kind) || !emptyDirectory(destination)))
      return;
    if ((kind == Kind::Rename || kind == Kind::Move) && state.device != sourceParent.first)
      return;
    entries.push_back(
        {kind, source, destination, std::move(linkTarget), std::move(createdContent), sourceRevision, state,
         sourceParent, destinationParent}
    );
  }
} // namespace filemanager
