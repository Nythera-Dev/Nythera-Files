#include "filemanager/wastebin_operations.h"

#include "filemanager/file_interaction.h"
#include "filemanager/fs_compat.h"
#include "filemanager/wastebin_fallback.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace filemanager::wastebin_operations {
  namespace {
    namespace engine = file_operation_engine;
    using Kind = FileOperationKind;
    using ConflictAction = engine::ConflictAction;
    using ConflictDecision = engine::ConflictDecision;
    using engine::cleanupStagedTree;
    using engine::copyTree;
    using engine::entryExistsNoFollow;
    using engine::FileIdentity;
    using engine::FileTransferPauseState;
    using engine::measureTree;
    using engine::parentIdentity;
    using engine::PrivateStaging;
    using engine::publishProgress;
    using engine::reject;
    using engine::removeTree;
    using engine::revision;
    using engine::setErrorFromPublishOutcome;
    using engine::toCompatIdentity;
    using engine::transferCheckpoint;
    using engine::TransferMeasure;
    using engine::TransferProgress;

    bool queryTrashOriginalPath(GFile* file, GCancellable* cancellable, std::string& path, std::string& queryError) {
      GError* error = nullptr;
      GFileInfo* info =
          g_file_query_info(file, "trash::orig-path", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
      if (info == nullptr && error != nullptr)
        queryError = error->message;
      g_clear_error(&error);
      if (info == nullptr)
        return false;
      const char* original = g_file_info_get_attribute_byte_string(info, "trash::orig-path");
      if (original != nullptr && original[0] == '/')
        path = original;
      g_object_unref(info);
      return !path.empty();
    }

    bool gioOriginalPath(const std::string& uri, GCancellable* cancellable, std::string& path, std::string& message) {
      if (wastebin_fallback::productionWastebinForbidden()) {
        path.clear();
        message = "Production Wastebin metadata access is disabled in test isolation mode.";
        return false;
      }
      GFile* current = g_file_new_for_uri(uri.c_str());
      std::vector<std::string> suffix;
      std::string lastQueryError;
      GFile* trashRoot = g_file_new_for_uri("trash:///");
      while (current != nullptr) {
        path.clear();
        if (queryTrashOriginalPath(current, cancellable, path, lastQueryError)) {
          for (auto it = suffix.rbegin(); it != suffix.rend(); ++it)
            path = (std::filesystem::path(path) / *it).string();
          g_object_unref(current);
          g_object_unref(trashRoot);
          return true;
        }
        GFile* parent = g_file_get_parent(current);
        if (parent == nullptr)
          break;
        char* name = g_file_get_basename(current);
        if (name != nullptr)
          suffix.emplace_back(name);
        g_free(name);
        g_object_unref(current);
        current = parent;
        if (g_file_equal(current, trashRoot)) {
          g_object_unref(current);
          current = nullptr;
          break;
        }
      }
      if (current != nullptr)
        g_object_unref(current);
      g_object_unref(trashRoot);
      message =
          lastQueryError.empty() ? "Original location metadata is unavailable for this Wastebin item." : lastQueryError;
      return false;
    }

    bool gioEnumerateTrash(GCancellable* cancellable, std::vector<std::string>& uris, std::string& message) {
      if (wastebin_fallback::productionWastebinForbidden()) {
        uris.clear();
        message = "Production Wastebin access is disabled in test isolation mode.";
        return false;
      }
      GFile* root = g_file_new_for_uri("trash:///");
      GError* error = nullptr;
      GFileEnumerator* entries = g_file_enumerate_children(
          root, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error
      );
      if (entries == nullptr) {
        message = error == nullptr ? "Could not enumerate the Wastebin." : error->message;
        g_clear_error(&error);
        g_object_unref(root);
        return false;
      }
      while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, &error)) {
        GFile* item = g_file_get_child(root, g_file_info_get_name(child));
        char* uri = g_file_get_uri(item);
        if (uri != nullptr)
          uris.emplace_back(uri);
        g_free(uri);
        g_object_unref(item);
        g_object_unref(child);
      }
      const bool ok = error == nullptr;
      if (!ok)
        message = error->message;
      g_clear_error(&error);
      g_object_unref(entries);
      g_object_unref(root);
      return ok;
    }

    bool isTrashUri(const std::string& uri) {
      GFile* item = g_file_new_for_uri(uri.c_str());
      char* resolvedUri = g_file_get_uri(item);
      const bool valid = resolvedUri != nullptr && g_str_has_prefix(resolvedUri, "trash:");
      g_free(resolvedUri);
      g_object_unref(item);
      return valid;
    }

    bool isDirectTrashChild(const std::string& uri) {
      GFile* item = g_file_new_for_uri(uri.c_str());
      GFile* parent = g_file_get_parent(item);
      GFile* root = g_file_new_for_uri("trash:///");
      const bool direct = parent != nullptr && g_file_equal(parent, root);
      if (parent != nullptr)
        g_object_unref(parent);
      g_object_unref(root);
      g_object_unref(item);
      return direct;
    }

    bool gioMoveWithBackup(
        GFile* source, GFile* target, GFileType incomingType, bool replace,
        const std::optional<fs_compat::EntryIdentity>& expectedTarget, GCancellable* cancellable, std::string& message,
        bool& mutated
    ) {
      GError* error = nullptr;
      std::optional<fs_compat::ReplacementBackup> backup;
      bool backupMoved = false;
      bool preserveBackup = false;
      bool ok = true;
      g_autofree char* sourcePath = g_file_get_path(source);
      g_autofree char* targetPath = g_file_get_path(target);
      const bool isDirectory = incomingType == G_FILE_TYPE_DIRECTORY;
      const bool isSymlink = incomingType == G_FILE_TYPE_SYMBOLIC_LINK;

      if (sourcePath == nullptr && targetPath != nullptr) {
        ok = fs_compat::preflightTransferDestination(
            std::filesystem::path(targetPath).parent_path(), isDirectory, isSymlink, replace, &error
        );
      }
      if (ok && replace) {
        ok = fs_compat::allocateReplacementBackup(targetPath, expectedTarget, backup, message, &error);
        backupMoved = ok;
        mutated |= ok || !message.empty();
      }
      if (ok) {
        int directErrno = 0;
        if (sourcePath != nullptr && targetPath != nullptr) {
          const auto outcome =
              fs_compat::moveOrPublishEntryNoReplace(sourcePath, targetPath, false, std::nullopt, true);
          ok = outcome.ok;
          directErrno = outcome.savedErrno;
          mutated |= outcome.destinationCommitted;
          if (outcome.splitState) {
            preserveBackup = backupMoved;
            message = outcome.message
                + (backupMoved ? "; previous destination backup remains at " + backup->backupPath.string()
                               : std::string{});
          } else if (!ok) {
            setErrorFromPublishOutcome(outcome, &error);
          }
        } else {
          ok = false;
        }
        if (!ok && !preserveBackup && (sourcePath == nullptr || directErrno == EXDEV)) {
          g_clear_error(&error);
          GFile* parent = g_file_get_parent(target);
          if (parent == nullptr) {
            g_set_error_literal(
                &error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not resolve the destination parent folder."
            );
          } else {
            g_autofree char* parentPath = g_file_get_path(parent);
            const bool preflightOk = parentPath != nullptr
                && fs_compat::preflightTransferDestination(parentPath, isDirectory, isSymlink, replace, &error);
            if (!preflightOk) {
              g_object_unref(parent);
            } else {
              char* uuid = g_uuid_string_random();
              char* leaf = g_strdup_printf(".nythera-restore-%s", uuid);
              GFile* staged = g_file_get_child(parent, leaf);
              g_free(leaf);
              g_free(uuid);
              g_object_unref(parent);
              const bool absent = !g_file_query_exists(staged, cancellable);
              g_autofree char* stagedPath = g_file_get_path(staged);
              bool destinationCommitted = false;
              if (!absent) {
                g_set_error_literal(
                    &error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary restore destination."
                );
              } else if (!copyTree(
                             source, staged, cancellable, &error, nullptr, engine::CopyMetadata::DirectoryModeAndTimes
                         )) {
                // Preserve the source when staging fails.
              } else {
                const auto outcome =
                    fs_compat::moveOrPublishEntryNoReplace(stagedPath, targetPath, true, std::nullopt, true);
                destinationCommitted = outcome.destinationCommitted;
                mutated |= destinationCommitted;
                if (outcome.splitState) {
                  preserveBackup = backupMoved;
                  message = outcome.message
                      + "; source remains in Wastebin"
                      + (backupMoved ? "; previous destination backup remains at " + backup->backupPath.string()
                                     : std::string{});
                } else if (!outcome.ok) {
                  setErrorFromPublishOutcome(outcome, &error);
                } else {
                  // Production sources are trash:// GFiles, so recursive semantics
                  // remain owned by that backend. Local recursion is limited to the
                  // explicit isolated provider seam used by tests.
                  ok = g_file_has_uri_scheme(source, "trash") ? g_file_delete(source, nullptr, &error)
                                                              : removeTree(source, nullptr, &error);
                  if (!ok && error == nullptr)
                    g_set_error_literal(
                        &error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Restored a copy but could not remove the original Wastebin item."
                    );
                  if (!ok) {
                    preserveBackup = backupMoved;
                    message = std::string("A restored copy remains at ")
                        + (targetPath == nullptr ? "the destination" : targetPath)
                        + "; the original could not be removed from Wastebin"
                        + (backupMoved ? "; previous destination backup remains at " + backup->backupPath.string()
                                       : std::string{});
                  }
                }
              }
              if (!destinationCommitted && absent && g_file_query_exists(staged, nullptr)) {
                cleanupStagedTree(staged);
              }
              g_object_unref(staged);
            }
          }
        }
      }
      if (!ok && backupMoved && !preserveBackup && backup) {
        fs_compat::restoreOrReportBackup(*backup, "Restore failed", error, message);
      }
      if (!ok && message.empty())
        message = error == nullptr ? "Could not restore Wastebin item." : error->message;
      g_clear_error(&error);
      if (ok && backup) {
        ok = fs_compat::removeOrReportBackup(
            *backup, "Restored item, but could not remove the replaced destination backup: ", message
        );
      }
      return ok;
    }

  } // namespace
  Result execute(
      FileOperationKind kind, const std::vector<std::string>& sources, const std::string& destinationDirectory,
      const Backend& backend, const file_operation_engine::ConflictResolver& conflictResolver,
      const file_operation_engine::TransferCallbacks& transferCallbacks, GCancellable* cancellable,
      file_operation_engine::FileTransferPauseState* pause
  ) {
    Result result;
    GError* error = nullptr;
    const auto fail = [&](const char* fallback) {
      result.ok = false;
      result.message = error == nullptr ? fallback : error->message;
      result.cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_clear_error(&error);
    };
    TransferProgress progress{.callbacks = transferCallbacks, .pause = pause, .cancellable = cancellable};
    if (kind == Kind::Restore || kind == Kind::EmptyWastebin) {
      std::vector<std::string> uris = sources;
      if (kind == Kind::EmptyWastebin) {
        const bool enumerated = backend.enumerateTopLevel ? backend.enumerateTopLevel(cancellable, uris, result.message)
                                                          : gioEnumerateTrash(cancellable, uris, result.message);
        if (!enumerated) {
          result.ok = false;
          result.cancelled = cancellable != nullptr && g_cancellable_is_cancelled(cancellable);
          result.customTitle = result.cancelled ? "Empty Wastebin cancelled" : "Empty Wastebin failed";
          result.customDescription = result.message;

          return result;
        }
        if (!backend.enumerateTopLevel) {
          const auto invalid =
              std::ranges::find_if(uris, [](const std::string& uri) { return !isDirectTrashChild(uri); });
          if (invalid != uris.end()) {
            result.ok = false;
            result.message = "The Wastebin provider returned an item outside the trash root.";
            result.customTitle = "Empty Wastebin failed";
            result.customDescription = result.message;

            return result;
          }
        }
      }
      const std::size_t requested = uris.size();
      result.requestedCount = requested;
      std::size_t restored = 0;
      std::size_t failed = 0;
      std::size_t skipped = 0;
      std::vector<std::string> failures;
      std::optional<ConflictAction> applyConflictToAll;
      std::optional<std::pair<GFileType, GFileType>> applyConflictTypes;
      auto noteFailure = [&](const std::string& uri, const std::string& detail) {
        ++failed;
        if (failures.size() < 4) {
          GFile* file = g_file_new_for_uri(uri.c_str());
          char* name = g_file_get_basename(file);
          failures.emplace_back(std::string(name == nullptr ? "item" : name) + ": " + detail);
          g_free(name);
          g_object_unref(file);
        }
      };
      for (std::size_t i = 0; i < requested; ++i) {
        if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
          result.cancelled = true;
          g_clear_error(&error);
          break;
        }
        std::string itemError;
        bool succeeded = false;
        if (kind == Kind::EmptyWastebin) {
          if (wastebin_fallback::productionWastebinForbidden()
              && (uris[i].starts_with("trash:") || isTrashUri(uris[i]))) {
            succeeded = false;
            itemError = "Production Wastebin deletion is disabled in test isolation mode.";
          } else {
            // The enumerator supplies only the trash backend's direct children.
            // Deletion stays within GIO so the backend owns recursive trash removal.
            GFile* item = g_file_new_for_uri(uris[i].c_str());
            GError* deleteError = nullptr;
            succeeded = g_file_delete(item, cancellable, &deleteError);
            result.mutated |= succeeded;
            if (!succeeded)
              itemError = deleteError == nullptr ? "Could not delete Wastebin entry." : deleteError->message;
            g_clear_error(&deleteError);
            g_object_unref(item);
          }
        } else {
          if (!backend.originalPath && !isTrashUri(uris[i])) {
            noteFailure(uris[i], "Restore source is not a GIO trash URI.");
            continue;
          }
          std::string originalPath;
          const bool resolved = backend.originalPath
              ? backend.originalPath(uris[i], cancellable, originalPath, itemError)
              : gioOriginalPath(uris[i], cancellable, originalPath, itemError);
          if (resolved && !originalPath.empty() && originalPath.front() == '/') {
            const auto original = std::filesystem::path(originalPath);
            const std::string filename = original.filename().string();
            std::filesystem::path destination =
                destinationDirectory.empty() ? original : std::filesystem::path(destinationDirectory) / filename;
            if (const auto originalNameError = filemanager::invalidFileNameReason(filename); !originalNameError.empty())
              itemError = "Invalid original location metadata. " + std::string(originalNameError);
            else if (originalPath.find('\0') != std::string::npos)
              itemError = "The original location contains a NUL character.";
            else if (
                wastebin_fallback::productionWastebinForbidden()
                && (uris[i].starts_with("trash:") || isTrashUri(uris[i]))
            ) {
              itemError = "Production Wastebin restore is disabled in test isolation mode.";
            } else {
              GFile* source = g_file_new_for_uri(uris[i].c_str());
              GFile* target = g_file_new_for_path(destination.string().c_str());
              GError* sourceQueryError = nullptr;
              GFileInfo* incoming = g_file_query_info(
                  source, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable,
                  &sourceQueryError
              );
              GFileInfo* existing = g_file_query_info(
                  target, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
              );
              const auto existingBefore =
                  existing != nullptr ? fs_compat::queryEntryIdentity(destination.string().c_str()) : std::nullopt;
              const auto incomingType = incoming == nullptr ? G_FILE_TYPE_UNKNOWN : g_file_info_get_file_type(incoming);
              bool replace = false;
              bool proceed = true;
              if (incoming == nullptr) {
                itemError = sourceQueryError == nullptr ? "Could not inspect the Wastebin source item."
                                                        : sourceQueryError->message;
                proceed = false;
              } else if (existing != nullptr) {
                const auto existingType = g_file_info_get_file_type(existing);
                ConflictDecision decision{.action = ConflictAction::Cancel};
                if (applyConflictToAll && applyConflictTypes == std::pair{incomingType, existingType})
                  decision.action = *applyConflictToAll;
                else if (conflictResolver && !g_cancellable_is_cancelled(cancellable)) {
                  const auto metadata =
                      engine::queryConflictMetadata(source, target, incomingType, existingType, cancellable);
                  decision = conflictResolver(uris[i], destination.string(), i + 1 < requested, metadata, cancellable);
                }
                if (decision.applyToAll) {
                  applyConflictToAll = decision.action;
                  applyConflictTypes = std::pair{incomingType, existingType};
                }
                if (decision.action == ConflictAction::Skip) {
                  ++skipped;
                  proceed = false;
                } else if (
                    decision.action == ConflictAction::Replace
                    && incomingType != G_FILE_TYPE_UNKNOWN
                    && incomingType == existingType
                    && incomingType != G_FILE_TYPE_DIRECTORY
                ) {
                  replace = true;
                } else if (decision.action == ConflictAction::Rename) {
                  std::string renamed = decision.alternativeName;
                  if (renamed.empty()) {
                    const auto stem =
                        incomingType == G_FILE_TYPE_DIRECTORY ? original.filename().string() : original.stem().string();
                    const auto extension =
                        incomingType == G_FILE_TYPE_DIRECTORY ? std::string{} : original.extension().string();
                    for (unsigned suffix = 1; suffix < 10000; ++suffix) {
                      const std::string candidate = stem
                          + " (restored"
                          + (suffix == 1 ? std::string{} : " " + std::to_string(suffix))
                          + ")"
                          + extension;
                      GFile* parent = g_file_get_parent(target);
                      GFile* next = g_file_get_child(parent, candidate.c_str());
                      g_object_unref(parent);
                      const bool exists = g_file_query_exists(next, cancellable);
                      if (!exists) {
                        g_object_unref(target);
                        target = next;
                        destination = std::filesystem::path(destination).parent_path() / candidate;
                        renamed = candidate;
                        break;
                      }
                      g_object_unref(next);
                    }
                    if (renamed.empty())
                      itemError = "Could not find an available restore name.";
                  } else if (const auto reason = filemanager::invalidFileNameReason(renamed); !reason.empty()) {
                    itemError = reason;
                  } else {
                    GFile* parent = g_file_get_parent(target);
                    GFile* next = g_file_get_child(parent, renamed.c_str());
                    g_object_unref(parent);
                    if (g_file_query_exists(next, cancellable))
                      itemError = "The alternative name already exists.";
                    else {
                      g_object_unref(target);
                      target = next;
                      destination = std::filesystem::path(destination).parent_path() / renamed;
                    }
                    if (target != next)
                      g_object_unref(next);
                  }
                } else {
                  proceed = false;
                  result.cancelled = true;
                  itemError = "Restore cancelled at the name conflict.";
                }
              }
              if (proceed && itemError.empty())
                succeeded = gioMoveWithBackup(
                    source, target, incomingType, replace, existingBefore, cancellable, itemError, result.mutated
                );
              g_clear_error(&sourceQueryError);
              if (incoming != nullptr)
                g_object_unref(incoming);
              if (existing != nullptr)
                g_object_unref(existing);
              g_object_unref(source);
              g_object_unref(target);
            }
          } else if (itemError.empty()) {
            itemError = "Original location metadata is unavailable or invalid for this Wastebin item.";
          }
        }
        if (succeeded)
          ++restored;
        else if (
            !itemError.empty() && !(kind == Kind::Restore && itemError == "Restore cancelled at the name conflict.")
        ) {
          noteFailure(uris[i], itemError);
        }
        if (result.cancelled)
          break;
      }
      result.cancelled |= g_cancellable_is_cancelled(cancellable) && restored + skipped < requested;
      result.ok = failed == 0 && !result.cancelled;
      result.customTitle = result.cancelled ? operationName(kind) + std::string(" cancelled")
          : failed == 0                     ? operationName(kind) + std::string(" completed")
          : restored + skipped > 0          ? operationName(kind) + std::string(" partially completed")
                                            : operationName(kind) + std::string(" failed");
      std::string label = kind == Kind::Restore ? "restored" : "deleted";
      result.customDescription = std::to_string(restored) + " of " + std::to_string(requested) + " items " + label;
      if (skipped > 0)
        result.customDescription += ", " + std::to_string(skipped) + " skipped";
      if (failed > 0)
        result.customDescription += ", " + std::to_string(failed) + " failed";
      if (result.cancelled)
        result.customDescription += "; operation cancelled";
      for (const auto& failure : failures)
        result.customDescription += "; " + failure;
      if (failed > failures.size())
        result.customDescription += "; additional failures omitted";
      if (requested == 0) {
        result.ok = true;
        result.customTitle = operationName(kind) + std::string(" completed");
        result.customDescription = kind == Kind::EmptyWastebin ? "Wastebin is already empty" : "No items to restore";
      }

      return result;
    }
    if (kind == Kind::TrashToHome) {
      const auto count = sources.size();
      std::vector<TransferMeasure> measures(count);
      for (std::size_t i = 0; i < count; ++i) {
        if (!transferCheckpoint(&progress)) {
          g_cancellable_set_error_if_cancelled(cancellable, &error);
          fail("Cancelled");
          break;
        }
        GFile* source = g_file_new_for_path(sources[i].c_str());
        const bool measured = measureTree(source, cancellable, measures[i], &error);
        g_object_unref(source);
        if (!measured) {
          fail("Could not calculate transfer size");
          break;
        }
        result.totalMeasure.bytes += measures[i].bytes;
        result.totalMeasure.items += measures[i].items;
      }
      std::filesystem::path trashRoot;
      std::filesystem::path infoDir;
      std::filesystem::path filesDir;
      if (result.message.empty() && !wastebin_fallback::ensureHomeWastebinDirs(trashRoot, infoDir, filesDir, &error)) {
        fail("Could not prepare Home Wastebin directories.");
      }
      if (result.message.empty()) {
        if (const auto available = wastebin_fallback::queryAvailableBytes(filesDir);
            available.has_value() && *available < result.totalMeasure.bytes) {
          result.ok = false;
          result.message = "Insufficient free space in Home Wastebin ("
              + wastebin_fallback::formatByteSize(result.totalMeasure.bytes)
              + " required, "
              + wastebin_fallback::formatByteSize(*available)
              + " available).";
          result.customDescription = result.message;
        }
      }
      if (result.message.empty()) {
        transferCallbacks.report(0, result.totalMeasure.bytes, 0, result.totalMeasure.items, 0);
      }
      std::uint64_t completedBytes = 0;
      std::uint64_t completedItems = 0;
      progress.totalBytes = result.totalMeasure.bytes;
      progress.totalItems = result.totalMeasure.items;
      progress.lastReport = std::chrono::steady_clock::now();
      if (progress.pause)
        progress.reportedPause = progress.pause->pausedTime();

      struct stat filesDirStat{};
      const std::uint64_t filesDirDevice =
          lstat(filesDir.c_str(), &filesDirStat) == 0 ? static_cast<std::uint64_t>(filesDirStat.st_dev) : 0;

      for (std::size_t i = 0; i < count && result.message.empty(); ++i) {
        if (!transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error)) {
          g_cancellable_set_error_if_cancelled(cancellable, &error);
          fail("Cancelled");
          break;
        }
        const auto& sourcePath = sources[i];
        const auto sourceBefore = revision(sourcePath);
        if (!sourceBefore) {
          result.ok = false;
          result.message = "The source item is missing or could not be inspected: " + sourcePath;
          break;
        }
        const bool sourceIsDir = S_ISDIR(sourceBefore->mode);
        const bool sourceIsSymlink = S_ISLNK(sourceBefore->mode);
        if (!fs_compat::preflightTransferDestination(filesDir, sourceIsDir, sourceIsSymlink, false, &error)) {
          fail("Home Wastebin filesystem does not support safe file publication.");
          break;
        }
        std::error_code absEc;
        std::filesystem::path absSource = std::filesystem::absolute(sourcePath, absEc).lexically_normal();
        if (absEc || absSource.empty())
          absSource = sourcePath;

        wastebin_fallback::ReservedTrashEntry reserved;
        if (!wastebin_fallback::reserveHomeTrashEntry(absSource, reserved, &error)) {
          fail("Could not reserve Home Wastebin entry.");
          break;
        }

        bool committedToHomeTrash = false;
        bool removedSourceDirectly = false;
        if (filesDirDevice != 0 && sourceBefore->device == filesDirDevice) {
          const auto directOutcome = fs_compat::moveOrPublishEntryNoReplace(
              sourcePath.c_str(), reserved.filesPath.c_str(), false, toCompatIdentity(*sourceBefore), sourceIsSymlink
          );
          if (directOutcome.ok) {
            committedToHomeTrash = true;
            removedSourceDirectly = true;
            result.ok = true;
          } else if (directOutcome.splitState) {
            committedToHomeTrash = true;
            result.ok = false;
            result.mutated = true;
            result.message = "Trashed copy remains in Home Wastebin at "
                + reserved.filesPath.string()
                + "; source still exists at "
                + sourcePath
                + ": "
                + directOutcome.message;
            result.customDescription = result.message;
          }
        }

        if (!committedToHomeTrash && result.message.empty()) {
          char* uuid = g_uuid_string_random();
          char* leaf = g_strdup_printf(".nythera-trash-partial-%s-%zu", uuid, i);
          g_free(uuid);
          GFile* source = g_file_new_for_path(sourcePath.c_str());
          GFile* filesDirFile = g_file_new_for_path(filesDir.c_str());
          GFile* staged = g_file_get_child(filesDirFile, leaf);
          g_object_unref(filesDirFile);
          g_free(leaf);

          const bool stagedWasAbsent = !entryExistsNoFollow(staged);
          progress.baseBytes = completedBytes;
          progress.progressedBytes = completedBytes;
          progress.completedItems = completedItems;
          if (stagedWasAbsent)
            result.ok =
                copyTree(source, staged, cancellable, &error, &progress, engine::CopyMetadata::DirectoryModeAndTimes);
          else {
            g_set_error_literal(
                &error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary Home Wastebin staging path"
            );
            result.ok = false;
          }
          if (result.ok
              && (!transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error))) {
            g_cancellable_set_error_if_cancelled(cancellable, &error);
            if (error == nullptr)
              g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
            result.ok = false;
          }

          g_autofree char* stagePath = g_file_get_path(staged);
          const auto stagedRevision = result.ok && stagePath ? revision(stagePath) : std::nullopt;
          bool publishSplitState = false;
          bool publishMetadataFailed = false;
          if (result.ok) {
            const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
                stagePath == nullptr ? "" : stagePath, reserved.filesPath.c_str(), true,
                stagedRevision ? std::optional<fs_compat::EntryIdentity>(toCompatIdentity(*stagedRevision))
                               : std::nullopt,
                sourceIsSymlink
            );
            result.ok = outcome.ok;
            if (!outcome.ok) {
              publishSplitState = outcome.splitState;
              publishMetadataFailed = outcome.metadataFailed;
              setErrorFromPublishOutcome(outcome, &error);
            }
          }
          committedToHomeTrash = result.ok || publishSplitState;
          if (publishSplitState && stagedWasAbsent && entryExistsNoFollow(staged)) {
            cleanupStagedTree(staged);
            if (!publishMetadataFailed && !entryExistsNoFollow(staged)) {
              result.ok = true;
              publishSplitState = false;
              g_clear_error(&error);
            }
          }
          if (!committedToHomeTrash) {
            if (stagedWasAbsent && entryExistsNoFollow(staged))
              cleanupStagedTree(staged);
            unlink(reserved.infoPath.c_str());
          }
          g_object_unref(staged);
          g_object_unref(source);
        }

        if (committedToHomeTrash) {
          result.mutated = true;
          if (!removedSourceDirectly && result.ok) {
            GFile* source = g_file_new_for_path(sourcePath.c_str());
            const bool cancelledBeforeCleanup =
                !transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error);
            if (cancelledBeforeCleanup) {
              result.ok = false;
              result.cancelled = true;
            } else {
              result.ok = removeTree(source, cancellable, &error, &result.mutated);
              result.cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            }
            if (!result.ok) {
              const bool sourceStillExists =
                  !sourcePath.empty() ? revision(sourcePath).has_value() : entryExistsNoFollow(source);
              result.message = std::string("Trashed copy remains in Home Wastebin at ")
                  + reserved.filesPath.string()
                  + (sourceStillExists ? "; source still exists at " + sourcePath
                                       : "; source was already removed from " + sourcePath)
                  + (result.cancelled
                         ? " because the operation was cancelled before source cleanup finished"
                         : ": " + std::string(error == nullptr ? "could not remove source" : error->message));
              result.customDescription = result.message;
            }
            g_object_unref(source);
          }
          if (result.ok) {
            result.movedSources.push_back(sourcePath);
            completedBytes += measures[i].bytes;
            completedItems += measures[i].items;
            result.completedMeasure.bytes = completedBytes;
            result.completedMeasure.items = completedItems;
            progress.baseBytes = completedBytes;
            progress.progressedBytes = completedBytes;
            progress.completedItems = completedItems;
            publishProgress(progress, 0, true);
          }
        }

        if (!result.ok) {
          if (result.message.empty())
            fail("Could not move item to Home Wastebin");
          else {
            result.cancelled |= error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            g_clear_error(&error);
          }
          break;
        }
      }
      if (result.message.empty())
        result.ok = true;
    } else if (kind == Kind::Trash) {
      const auto count = sources.size();
      for (std::size_t i = 0; i < count && result.message.empty(); ++i) {
        if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
          fail("Cancelled");
          break;
        }
        const auto& sourcePath = sources[i];
        GFile* source = g_file_new_for_path(sourcePath.c_str());
        if (const int injectedErrno = wastebin_fallback::invokeTrashHook(sourcePath.c_str()); injectedErrno != 0) {
          result.ok = false;
          g_set_error_literal(&error, G_IO_ERROR, g_io_error_from_errno(injectedErrno), g_strerror(injectedErrno));
        } else if (
            std::string isolationError; wastebin_fallback::productionWastebinForbidden()
            && !wastebin_fallback::validateIsolatedTrashTarget(sourcePath, isolationError)
        ) {
          result.ok = false;
          g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, isolationError.c_str());
        } else {
          result.ok = g_file_trash(source, cancellable, &error);
        }
        result.mutated |= result.ok;
        if (result.ok) {
          result.movedSources.push_back(sourcePath);
        } else if (error == nullptr || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
          const auto diag = wastebin_fallback::diagnoseTrashFailure(sourcePath, error);
          if (!diag.summary.empty()) {
            result.message = diag.summary;
            result.customDescription = diag.summary;
          }
          if (diag.eligibleForHomeFallback) {
            result.trashFallback = FallbackOffer{
                .sources = std::vector<std::string>(sources.begin() + static_cast<std::ptrdiff_t>(i), sources.end()),
                .mountPath = diag.mountRoot.string(),
                .trashDirectory = diag.trashDirPath.string(),
                .diagnostic = diag.summary,
            };
          }
        }
        if (result.ok) {
          const auto done = static_cast<std::uint64_t>(i + 1);
          transferCallbacks.report(0, 0, done, static_cast<std::uint64_t>(count), 0);
        }
        if (!result.ok) {
          if (result.message.empty())
            fail("Operation failed");
          else {
            result.cancelled |= error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            g_clear_error(&error);
          }
        }
        g_object_unref(source);
      }
      if (result.message.empty())
        result.ok = true;
    }

    return result;
  }
} // namespace filemanager::wastebin_operations
