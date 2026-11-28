#include "filemanager/protected_file_operations.h"

#include "filemanager/checksum.h"
#include "filemanager/folder_size.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace filemanager::file_operation_engine {
  namespace {
    struct ScopedFd {
      int fd = -1;
      ~ScopedFd() {
        if (fd >= 0)
          close(fd);
      }
    };

    int openProtectedPath(int parent, const char* path, std::uint64_t flags) {
      const open_how how{.flags = flags | O_CLOEXEC, .resolve = RESOLVE_NO_MAGICLINKS};
      return static_cast<int>(syscall(SYS_openat2, parent, path, &how, sizeof(how)));
    }

    std::string fdPath(int fd) { return "/proc/self/fd/" + std::to_string(fd); }

    std::string contentRevision(const struct stat& state) {
      return std::to_string(state.st_dev)
          + ":"
          + std::to_string(state.st_ino)
          + ":"
          + std::to_string(state.st_size)
          + ":"
          + std::to_string(state.st_mtim.tv_sec)
          + ":"
          + std::to_string(state.st_mtim.tv_nsec)
          + ":"
          + std::to_string(state.st_ctim.tv_sec)
          + ":"
          + std::to_string(state.st_ctim.tv_nsec);
    }

    bool copyProtectedAttributes(int source, int destination, const struct stat& state) {
      if (fchown(destination, state.st_uid, state.st_gid) != 0 || fchmod(destination, state.st_mode & 07777) != 0)
        return false;
      const auto count = flistxattr(source, nullptr, 0);
      if (count < 0)
        return errno == EOPNOTSUPP;
      std::vector<char> names(static_cast<std::size_t>(count));
      const auto length = flistxattr(source, names.data(), names.size());
      if (length < 0 || length > count) {
        if (length > count)
          errno = EAGAIN;
        return false;
      }
      for (const char* name = names.data(); length > 0 && name < names.data() + length; name += std::strlen(name) + 1) {
        const auto size = fgetxattr(source, name, nullptr, 0);
        if (size < 0)
          return false;
        std::vector<char> value(static_cast<std::size_t>(size));
        const auto actual = fgetxattr(source, name, value.data(), value.size());
        if (actual < 0
            || actual > size
            || fsetxattr(destination, name, value.data(), static_cast<std::size_t>(actual), 0) != 0)
          return false;
      }
      // A staging directory can inherit a default ACL even when the original
      // had no access ACL.
      if (fgetxattr(source, "system.posix_acl_access", nullptr, 0) < 0
          && errno == ENODATA
          && fremovexattr(destination, "system.posix_acl_access") != 0
          && errno != ENODATA
          && errno != EOPNOTSUPP)
        return false;
      return true;
    }

  } // namespace

  Result executeProtectedFile(
      Request& request, GCancellable* cancellable, FileTransferPauseState* pause, const TransferCallbacks& callbacks
  ) {
    Result result;
    if (request.kind == Kind::ProtectedChecksum) {
      if (request.contentRevision != "0" && request.contentRevision != "1" && request.contentRevision != "2") {
        result.message = "Invalid checksum algorithm.";
        return result;
      }
      const auto algorithm = static_cast<checksum::Algorithm>(request.contentRevision[0] - '0');
      checksum::Result calculated;
      g_autoptr(GError) error = nullptr;
      result.ok = checksum::calculate(
          request.destination, algorithm, cancellable, calculated, &error,
          [&](std::uint64_t bytes, std::uint64_t total) {
            if (callbacks.report)
              callbacks.report(bytes, total, 0, 1, 0);
          }
      );
      result.cancelled = g_cancellable_is_cancelled(cancellable);
      result.customDescription = std::move(calculated.hash);
      if (error)
        result.message = error->message;
      return result;
    }
    if (request.kind == Kind::ProtectedFolderSize) {
      if (!request.renameIdentity) {
        result.message = "The folder identity is unavailable.";
        return result;
      }
      const auto [device, inode] = *request.renameIdentity;
      const auto folder = calculateFolderSize({request.destination, device, inode}, [cancellable] {
        return g_cancellable_is_cancelled(cancellable);
      });
      result.ok = !folder.failed && !folder.cancelled;
      result.cancelled = folder.cancelled;
      result.message = folder.reason;
      result.customDescription = encodeFolderSizeResult(folder);
      return result;
    }
    const bool saving = request.kind == Kind::SaveProtectedFile;
    const auto fail = [&](std::string message) {
      result.cancelled = g_cancellable_is_cancelled(cancellable);
      result.message = std::move(message);
      return result;
    };
    const auto failIo = [&](const char* action) { return fail(std::string(action) + ": " + std::strerror(errno)); };
    if (!request.invokingUid || request.sources.size() != 1)
      return fail("Protected file access requires an authenticated Administrator session.");

    const std::filesystem::path temporaryPath(saving ? request.sources.front() : request.destination);
    const auto& originalPath = saving ? request.destination : request.sources.front();
    const ScopedFd userDirectory{
        openProtectedPath(AT_FDCWD, temporaryPath.parent_path().c_str(), O_PATH | O_DIRECTORY)
    };
    struct stat directoryState{};
    if (userDirectory.fd < 0
        || fstat(userDirectory.fd, &directoryState) != 0
        || directoryState.st_uid != *request.invokingUid
        || (directoryState.st_mode & 0077) != 0)
      return fail("The temporary copy directory is unavailable.");
    const ScopedFd userEntry{
        openProtectedPath(userDirectory.fd, temporaryPath.filename().c_str(), O_PATH | O_NOFOLLOW)
    };
    struct stat userState{};
    if (userEntry.fd < 0
        || fstat(userEntry.fd, &userState) != 0
        || !S_ISREG(userState.st_mode)
        || userState.st_uid != *request.invokingUid
        || userState.st_nlink != 1)
      return fail("The temporary copy is no longer a regular file owned by your user.");
    const ScopedFd originalEntry{openProtectedPath(AT_FDCWD, originalPath.c_str(), O_PATH)};
    struct stat originalState{};
    if (originalEntry.fd < 0 || fstat(originalEntry.fd, &originalState) != 0)
      return failIo("Could not open the original file");
    if (!S_ISREG(originalState.st_mode))
      return fail("Only regular files can be opened as temporary copies.");
    const auto originalRevision = contentRevision(originalState);
    if (saving && originalRevision != request.contentRevision)
      return fail("The original file changed since it was opened. Your temporary copy is still available.");
    if (originalState.st_dev == userState.st_dev && originalState.st_ino == userState.st_ino)
      return fail("The temporary copy must be a separate file.");

    const ScopedFd original{open(fdPath(originalEntry.fd).c_str(), O_RDONLY | O_CLOEXEC)};
    const ScopedFd userFile{open(fdPath(userEntry.fd).c_str(), (saving ? O_RDONLY : O_WRONLY) | O_CLOEXEC)};
    if (original.fd < 0 || userFile.fd < 0)
      return failIo("Could not open the file for transfer");
    const auto transfer = [&](int source, int destination, const struct stat& initial) {
      TransferProgress progress{
          .callbacks = callbacks,
          .pause = pause,
          .cancellable = cancellable,
          .totalBytes = static_cast<std::uint64_t>(initial.st_size),
          .totalItems = 1
      };
      std::array<char, 128 * 1024> buffer;
      std::uint64_t completed = 0;
      while (transferCheckpoint(&progress, completed) && !g_cancellable_is_cancelled(cancellable)) {
        const auto readSize = read(source, buffer.data(), buffer.size());
        if (readSize < 0 && errno == EINTR)
          continue;
        if (readSize < 0)
          return failIo("Could not read the file");
        if (readSize == 0) {
          struct stat current{};
          if (fstat(source, &current) != 0 || contentRevision(current) != contentRevision(initial))
            return fail("The file being read changed during the transfer.");
          result.ok = true;
          return result;
        }
        for (ssize_t offset = 0; offset < readSize;) {
          const auto written = write(destination, buffer.data() + offset, static_cast<std::size_t>(readSize - offset));
          if (written < 0 && errno == EINTR)
            continue;
          if (written <= 0)
            return failIo("Could not write the file");
          offset += written;
        }
        completed += static_cast<std::uint64_t>(readSize);
        publishProgress(progress, completed);
      }
      return fail("Transfer cancelled.");
    };

    if (!saving) {
      if (ftruncate(userFile.fd, 0) != 0)
        return failIo("Could not prepare the temporary copy");
      result = transfer(original.fd, userFile.fd, originalState);
      if (result.ok)
        result.customDescription = originalRevision;
      return result;
    }

    g_autofree char* resolved = g_file_read_link(fdPath(originalEntry.fd).c_str(), nullptr);
    if (!resolved)
      return fail("The original file is no longer available.");
    const std::filesystem::path resolvedPath(resolved);
    const ScopedFd targetDirectory{
        openProtectedPath(AT_FDCWD, resolvedPath.parent_path().c_str(), O_PATH | O_DIRECTORY)
    };
    if (targetDirectory.fd < 0)
      return failIo("Could not open the original file's directory");
    const auto target = std::filesystem::path(fdPath(targetDirectory.fd)) / resolvedPath.filename();
    PrivateStaging staging(target.string());
    if (staging.directory.empty())
      return failIo("Could not stage the edited file");
    const ScopedFd stageDirectory{openProtectedPath(
        targetDirectory.fd, std::filesystem::path(staging.directory).filename().c_str(),
        O_PATH | O_DIRECTORY | O_NOFOLLOW
    )};
    struct stat stagingState{};
    if (stageDirectory.fd < 0
        || fstat(stageDirectory.fd, &stagingState) != 0
        || stagingState.st_uid != geteuid()
        || (stagingState.st_mode & 0077) != 0)
      return fail("Could not open a private directory for saving the changes.");
    struct StageEntry {
      std::string path;
      ~StageEntry() { unlink(path.c_str()); }
    } staged{fdPath(stageDirectory.fd) + "/item"};
    const ScopedFd edited{open(staged.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (edited.fd < 0)
      return failIo("Could not create a staged file");
    result = transfer(userFile.fd, edited.fd, userState);
    if (!result.ok)
      return result;
    result.ok = false;
    struct stat current{};
    if (!copyProtectedAttributes(original.fd, edited.fd, originalState) || fsync(edited.fd) != 0)
      return failIo("Could not preserve the original file's metadata");
    if (fstat(edited.fd, &current) != 0
        || current.st_uid != originalState.st_uid
        || current.st_gid != originalState.st_gid
        || (current.st_mode & 07777) != (originalState.st_mode & 07777))
      return fail("Could not verify the original file's ownership and permissions on the staged copy.");
    if (g_cancellable_is_cancelled(cancellable))
      return fail("Transfer cancelled.");
    const ScopedFd currentOriginal{openProtectedPath(AT_FDCWD, originalPath.c_str(), O_PATH)};
    if (currentOriginal.fd < 0
        || fstat(currentOriginal.fd, &current) != 0
        || contentRevision(current) != originalRevision
        || lstat(target.c_str(), &current) != 0
        || contentRevision(current) != originalRevision)
      return fail("The original file changed during the transfer. Your temporary copy is still available.");
    std::optional<fs_compat::ReplacementBackup> backup;
    g_autoptr(GError) error = nullptr;
    const auto expected = fs_compat::queryEntryIdentity(target.c_str());
    if (!expected
        || expected->inode != originalState.st_ino
        || expected->device != originalState.st_dev
        || !fs_compat::allocateReplacementBackup(target.c_str(), expected, backup, result.message, &error))
      return fail(
          result.message.empty() ? (error ? error->message : "Could not back up the original file.") : result.message
      );
    // Moving the original into the backup changes ctime. Check mtime and
    // size again to catch writes between the revision check and that move.
    if (fstat(original.fd, &current) != 0
        || current.st_size != originalState.st_size
        || current.st_mtim.tv_sec != originalState.st_mtim.tv_sec
        || current.st_mtim.tv_nsec != originalState.st_mtim.tv_nsec) {
      const char* reason = "The original file changed during the transfer. Your temporary copy is still available.";
      fs_compat::restoreOrReportBackup(*backup, reason, nullptr, result.message);
      return fail(result.message.empty() ? reason : result.message);
    }
    const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
        staged.path.c_str(), target.c_str(), true, fs_compat::queryEntryIdentity(staged.path.c_str()), false
    );
    result.mutated = outcome.destinationCommitted;
    if (!outcome.ok && !outcome.destinationCommitted) {
      fs_compat::restoreOrReportBackup(*backup, outcome.message.c_str(), nullptr, result.message);
      return fail(result.message.empty() ? outcome.message : result.message);
    }
    if (!fs_compat::removeOrReportBackup(*backup, "Saved changes but could not remove the backup: ", result.message))
      return result;
    if (fstat(edited.fd, &current) != 0)
      return failIo("Could not verify the saved file");
    result.ok = true;
    result.customDescription = contentRevision(current);
    result.message = "Saved changes back.";
    return result;
  }
} // namespace filemanager::file_operation_engine
