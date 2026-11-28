#include "archive_safety.h"
#include "filemanager/file_interaction.h"
#include "filemanager/fs_compat.h"

#include <algorithm>
#include <cerrno>
#include <set>
#include <tuple>

namespace filemanager::archive_backend {
  namespace {
    namespace fs = std::filesystem;
    namespace engine = file_operation_engine;
    using namespace detail;

    std::optional<struct stat> existingEntry(const fs::path& path) {
      struct stat entry{};
      if (::lstat(path.c_str(), &entry) == 0)
        return entry;
      if (errno != ENOENT)
        systemError("Could not inspect archive destination");
      return std::nullopt;
    }

    fs_compat::EntryIdentity identity(const struct stat& entry) {
      return {entry.st_dev, entry.st_ino, entry.st_mode, entry.st_nlink};
    }

    GFileType type(mode_t mode) {
      if (S_ISREG(mode))
        return G_FILE_TYPE_REGULAR;
      if (S_ISDIR(mode))
        return G_FILE_TYPE_DIRECTORY;
      if (S_ISLNK(mode))
        return G_FILE_TYPE_SYMBOLIC_LINK;
      return G_FILE_TYPE_SPECIAL;
    }

    fs::path alternatePath(const fs::path& target, bool directory, const std::string& requested) {
      if (!requested.empty()) {
        if (!filemanager::validFileName(requested))
          throwFileError(FileErrorKind::InvalidArgument, "Invalid archive conflict rename.");
        auto candidate = target.parent_path() / requested;
        if (existingEntry(candidate))
          throwFileError(FileErrorKind::Other, "The alternative archive destination already exists.");
        return candidate;
      }
      const auto stem = (directory ? target.filename() : target.stem()).native();
      const auto suffix = directory ? std::string{} : target.extension().native();
      for (std::uint64_t number = 1;; ++number) {
        const auto label = number == 1 ? " (copy)" : " (copy " + std::to_string(number) + ")";
        auto candidate = target.parent_path() / (stem + label + suffix);
        if (!existingEntry(candidate))
          return candidate;
      }
    }

    class Publication {
    public:
      Publication(Work& work, Result& result, std::span<const struct stat> inputs)
          : m_work(work), m_result(result), m_inputs(inputs) {}

      void directory(const fs::path& source, const fs::path& destination, const fs::path& relative = {}) {
        std::vector<fs::path> children;
        for (const auto& item : fs::directory_iterator(source))
          children.push_back(item.path().filename());
        std::ranges::sort(children);
        for (std::size_t index = 0; index < children.size(); ++index) {
          const auto& name = children[index];
          item(source / name, destination / name, relative / name, index + 1 < children.size());
        }
      }

      void item(const fs::path& source, fs::path target, const fs::path& relative, bool hasRemaining) {
        m_work.check();
        const auto staged = existingEntry(source);
        if (!staged)
          throwFileError(FileErrorKind::Other, "Staged archive entry is missing.");
        auto existing = existingEntry(target);
        std::optional<fs_compat::ReplacementBackup> backup;
        if (existing) {
          const auto incoming = type(staged->st_mode), current = type(existing->st_mode);
          const bool isInput =
              std::ranges::any_of(m_inputs, [&](const auto& input) { return sameIdentity(input, *existing); });
          auto decision = conflict(source, target, relative, incoming, current, isInput, hasRemaining);
          m_work.check();
          switch (decision.action) {
          case engine::ConflictAction::Skip:
            m_result.skippedEntries.push_back({relative.native(), SkipReason::Conflict});
            return;
          case engine::ConflictAction::Cancel:
            throwFileError(FileErrorKind::Cancelled, "Archive publication cancelled.");
          case engine::ConflictAction::Rename:
            target = alternatePath(target, incoming == G_FILE_TYPE_DIRECTORY, decision.alternativeName);
            break;
          case engine::ConflictAction::Merge: {
            const auto folder = existingEntry(target);
            if (incoming != G_FILE_TYPE_DIRECTORY
                || current != G_FILE_TYPE_DIRECTORY
                || !folder
                || !S_ISDIR(folder->st_mode))
              throwFileError(FileErrorKind::InvalidArgument, "Archive conflict cannot merge these entries.");
            directory(source, target, relative);
            return;
          }
          case engine::ConflictAction::Replace:
            if (isInput || incoming != current || incoming == G_FILE_TYPE_DIRECTORY)
              throwFileError(FileErrorKind::InvalidArgument, "Archive conflict cannot replace this entry.");
            backup = backupTarget(target, *existing);
            break;
          }
        }

        // A complete staged directory uses Files' native no-replace rename and its
        // vacancy-check fallback on filesystems such as NFS.
        const auto published =
            fs_compat::moveOrPublishEntryNoReplace(source.c_str(), target.c_str(), true, identity(*staged), true);
        if (published.destinationCommitted) {
          m_result.mutated = true;
          const auto result = fs_compat::queryEntryIdentity(target.c_str());
          if (result)
            m_result.completedEntries.push_back({target.native(), result->device, result->inode});
          ++m_result.completedMeasure.items;
        }
        std::string message = published.message;
        if (!published.ok) {
          if (backup && !published.destinationCommitted) {
            fs_compat::restoreOrReportBackup(*backup, published.message.c_str(), nullptr, message);
            m_result.mutated |= message != published.message;
          } else if (backup) {
            message += " Previous destination remains at " + backup->backupPath.native();
          }
          throwFileError(FileErrorKind::Other, std::move(message));
        }
        if (backup
            && !fs_compat::removeOrReportBackup(*backup, "Could not remove archive replacement backup: ", message))
          throwFileError(FileErrorKind::Other, std::move(message));
      }

    private:
      Work& m_work;
      Result& m_result;
      std::span<const struct stat> m_inputs;
      std::optional<std::tuple<GFileType, GFileType, engine::ConflictAction>> m_batch;

      engine::ConflictDecision conflict(
          const fs::path& source, const fs::path& target, const fs::path& relative, GFileType incoming,
          GFileType existing, bool isInput, bool hasRemaining
      ) {
        if (!isInput && m_batch && std::get<0>(*m_batch) == incoming && std::get<1>(*m_batch) == existing)
          return {.action = std::get<2>(*m_batch)};
        if (!m_work.callbacks.resolveConflict)
          throwFileError(FileErrorKind::Other, "Archive destination already exists.");
        const std::unique_ptr<GFile, decltype(&g_object_unref)> from(
            g_file_new_for_path(source.c_str()), g_object_unref
        );
        const std::unique_ptr<GFile, decltype(&g_object_unref)> to(g_file_new_for_path(target.c_str()), g_object_unref);
        auto metadata = engine::queryConflictMetadata(from.get(), to.get(), incoming, existing, m_work.cancellable);
        metadata.sameItem = isInput;
        auto decision = m_work.callbacks.resolveConflict(
            relative.native(), target.native(), hasRemaining, metadata, m_work.cancellable
        );
        if (decision.applyToAll && !isInput && decision.alternativeName.empty())
          m_batch = {incoming, existing, decision.action};
        return decision;
      }

      fs_compat::ReplacementBackup backupTarget(const fs::path& target, const struct stat& existing) {
        std::optional<fs_compat::ReplacementBackup> backup;
        std::string message;
        GError* error = nullptr;
        const bool ok =
            fs_compat::allocateReplacementBackup(target.c_str(), identity(existing), backup, message, &error);
        const std::unique_ptr<GError, decltype(&g_error_free)> ownedError(error, g_error_free);
        if (!ok) {
          m_result.mutated |= backup.has_value() || !message.empty();
          throwFileError(FileErrorKind::Other, message.empty() && error ? error->message : std::move(message));
        }
        return std::move(*backup);
      }
    };

    template <typename Operation>
    Result
    run(const fs::path& destination, GCancellable* cancellable, const Callbacks& callbacks, Operation operation) {
      Result result;
      try {
        const ArchiveLocale locale;
        (void)openDirectory(destination);
        Work work{.cancellable = cancellable, .callbacks = callbacks};
        work.check();
        Stage stage(destination);
        try {
          operation(stage, work, result);
          result.ok = true;
        } catch (const Error& error) {
          result.cancelled = error.kind == FileErrorKind::Cancelled;
          result.message = error.what();
        } catch (const std::exception& error) {
          result.message = error.what();
        }
        result.processedMeasure = {.bytes = work.bytes, .items = work.count};
        if (!stage.cleanup()) {
          result.ok = false;
          result.mutated = true;
          if (!result.message.empty())
            result.message += ' ';
          result.message += "Could not remove private archive staging at " + stage.path.native() + ".";
        }
      } catch (const Error& error) {
        result.cancelled = error.kind == FileErrorKind::Cancelled;
        result.message = error.what();
      } catch (const std::exception& error) {
        result.message = error.what();
      }
      return result;
    }
  } // namespace

  std::shared_ptr<const Catalogue>
  inspect(const fs::path& source, GCancellable* cancellable, const Callbacks& callbacks) {
    const ArchiveLocale locale;
    const auto input = openSource(source);
    Work work{.cancellable = cancellable, .callbacks = callbacks};
    auto catalogue = std::make_shared<Catalogue>();
    catalogue->source = source;
    catalogue->revision = input.revision;
    try {
      catalogue->entries = inspectArchive(input, work);
    } catch (const Error& error) {
      if (error.kind != FileErrorKind::EncryptedSevenZip)
        throw;
      Stage temporary(fs::temp_directory_path());
      catalogue->entries = inspectEncryptedSevenZip(input, temporary, work);
      catalogue->encryptedSevenZip = true;
    }
    return catalogue;
  }

  Result extractSelected(
      const Selection& selection, const fs::path& destination, GCancellable* cancellable, const Callbacks& callbacks
  ) {
    return run(destination, cancellable, callbacks, [&](Stage& stage, Work& work, Result& result) {
      if (!selection.catalogue || selection.members.empty())
        throwFileError(FileErrorKind::InvalidArgument, "No archive members were selected.");
      const auto& catalog = *selection.catalogue;
      const auto source = openSource(catalog.source);
      if (!sameRevision(source.revision, catalog.revision))
        throwFileError(FileErrorKind::Changed, "The archive changed. Reload its contents before extracting.");
      const std::set<std::string, std::less<>> chosen(selection.members.begin(), selection.members.end());
      Projection items;
      std::set<std::string, std::less<>> outputs;
      for (const auto& root : chosen) {
        work.check();
        auto entry = catalog.entries.find(root);
        if (entry == catalog.entries.end())
          throwFileError(FileErrorKind::Changed, "An archive selection no longer exists.");
        bool covered = false;
        for (auto parent = parentName(root); !parent.empty(); parent = parentName(parent))
          covered |= chosen.contains(parent);
        if (covered)
          continue;
        const auto output = leaf(root);
        if (!outputs.insert(output).second)
          throwFileError(FileErrorKind::InvalidArgument, "Selected archive members have conflicting names.");
        items.emplace(root, output);
        if (entry->second.kind == EntryKind::Directory) {
          const auto prefix = root + "/";
          for (auto child = catalog.entries.lower_bound(prefix);
               child != catalog.entries.end() && child->first.starts_with(prefix); ++child) {
            work.check();
            items.emplace(child->first, output + child->first.substr(root.size()));
          }
        }
      }
      if (catalog.encryptedSevenZip)
        readSelectedSevenZip(source, catalog, items, stage, work);
      else
        readSelected(source, catalog, items, stage, work);
      result.totalMeasure = {.bytes = work.bytes, .items = work.count};
      Publication publisher(work, result, std::span(&source.revision, 1));
      publisher.directory(stage.path, destination);
      if (result.skippedEntries.empty())
        result.completedMeasure = result.totalMeasure;
    });
  }

  Result extract(
      const fs::path& source, const fs::path& destination, GCancellable* cancellable, const Callbacks& callbacks,
      Extraction mode
  ) {
    return run(destination, cancellable, callbacks, [&](Stage& stage, Work& work, Result& result) {
      const auto input = openSource(source);
      try {
        readArchive(source, input, stage, work);
      } catch (const Error& error) {
        if (error.kind != FileErrorKind::EncryptedSevenZip)
          throw;
        for (const auto& item : fs::directory_iterator(stage.path))
          fs::remove_all(item.path());
        readEncryptedSevenZip(input, stage, work);
      }
      result.totalMeasure = {.bytes = work.bytes, .items = work.count};
      Publication publisher(work, result, std::span(&input.revision, 1));
      if (mode == Extraction::Direct) {
        publisher.directory(stage.path, destination);
      } else {
        const auto name = archiveStem(source.filename().native());
        if (!filemanager::validFileName(name) || name.contains('\\'))
          throwFileError(FileErrorKind::InvalidArgument, "Invalid extraction folder name.");
        const fs::path target = destination / name;
        const fs::directory_iterator it(stage.path), end;
        const auto matching = stage.path / name;
        const bool redundant = mode == Extraction::SmartFolder
            && it != end
            && it->path() == matching
            && fs::is_directory(it->symlink_status())
            && std::next(it) == end;
        if (redundant) {
          publisher.item(matching, target, name, false);
        } else {
          Stage wrapper(destination);
          const auto folder = wrapper.path / name;
          if (::mkdir(folder.c_str(), 0777) != 0)
            systemError("Could not create the containing archive folder");
          const auto mode = fs::status(folder).permissions();
          fs::remove(folder);
          fs::rename(stage.path, folder);
          if (::chmod(folder.c_str(), static_cast<mode_t>(mode)) != 0)
            systemError("Could not finish the containing archive folder");
          publisher.item(folder, target, name, false);
          if (!wrapper.cleanup())
            throwFileError(FileErrorKind::Other, "Could not remove private extraction staging.");
        }
      }
      if (result.skippedEntries.empty())
        result.completedMeasure = result.totalMeasure;
    });
  }

  Result compress(
      std::span<const fs::path> sources, const fs::path& output, Format format, Level level, GCancellable* cancellable,
      const Callbacks& callbacks, const EncryptionOptions& options
  ) {
    return run(output.parent_path(), cancellable, callbacks, [&](Stage& stage, Work& work, Result& result) {
      const auto all = capabilities();
      const auto cap = std::ranges::find(all, format, &Capability::format);
      const auto index = static_cast<std::size_t>(level);
      if (cap == all.end()
          || !cap->create
          || index >= cap->levels.size()
          || (format != Format::Tar && !cap->levels[index]))
        throwFileError(FileErrorKind::Unsupported, "Archive creation format or compression level is unavailable.");
      const auto name = output.filename().native();
      if (!filemanager::validFileName(name) || name.contains('\\'))
        throwFileError(FileErrorKind::InvalidArgument, "Invalid output archive filename.");
      const auto named = formatFromName(name);
      if ((named && named != format) || (format == Format::Gzip && !named))
        throwFileError(FileErrorKind::InvalidArgument, "Output filename does not match the archive format.");
      std::vector<struct stat> inputs;
      for (const auto& source : sources) {
        const auto parent = openDirectory(source.parent_path());
        inputs.push_back(statAt(parent.get(), source.filename().native()));
      }
      if (options.password.contains('\0')
          || (!options.password.empty() && format != Format::Zip && format != Format::SevenZip))
        throwFileError(FileErrorKind::InvalidArgument, "Password protection is unavailable for this archive format.");
      if (!options.password.empty() && format == Format::SevenZip)
        writeEncryptedSevenZip(sources, stage, level, options, work);
      else
        writeArchive(sources, stage, format, level, work, options);
      result.totalMeasure = {.bytes = work.bytes, .items = 1};
      Publication(work, result, inputs).item(stage.path / "output", output, name, false);
      if (result.skippedEntries.empty())
        result.completedMeasure = result.totalMeasure;
    });
  }
} // namespace filemanager::archive_backend
