#pragma once

#include "filemanager/file_operation_engine.h"

#include <array>
#include <filesystem>
#include <map>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace filemanager::archive_backend {
  enum class Format { Zip, SevenZip, Tar, TarGzip, TarXz, TarZstd, TarBzip2, Gzip, Rar };
  enum class Level { Fast, Balanced, Maximum };
  enum class Method { Default, Deflate, Store, Lzma2, Lzma, Copy };
  enum class Extraction { Direct, Folder, SmartFolder };
  struct EncryptionOptions {
    Method method = Method::Default;
    std::string password;
  };

  struct Capability {
    Format format;
    std::string_view suffix;
    bool extract;
    bool create;
    std::array<bool, 3> levels;
  };

  std::span<const Capability> capabilities();
  std::optional<Format> formatFromName(std::string_view name);
  std::string_view extension(Format format);
  std::string archiveStem(std::string_view name);
  bool canWriteMethod(Format format, Method method);
  bool canEncryptZip();
  std::optional<std::string> sevenZipProgram();

  struct Callbacks {
    file_operation_engine::ConflictResolver resolveConflict;
    // Synchronous callbacks on the calling thread. Checkpoints also observe GCancellable.
    std::function<void()> checkpoint;
    std::function<void(std::uint64_t bytes, std::uint64_t items)> progress;
    // True for retries where a passphrase was rejected or the data may be damaged.
    std::function<std::optional<std::string>(bool)> password;
    // Temporarily suspend the child while the existing pause checkpoint waits.
    std::function<void(const std::function<void(bool)>&)> helperCheckpoint;
  };

  enum class SkipReason { Conflict };

  struct SkippedEntry {
    std::string archivePath;
    SkipReason reason;
  };

  struct Result : file_operation_engine::Result {
    file_operation_engine::TransferMeasure processedMeasure;
    std::vector<SkippedEntry> skippedEntries;
  };

  enum class EntryKind { File, Directory, Symlink, Hardlink };
  struct Entry {
    EntryKind kind = EntryKind::File;
    std::string link;
    unsigned mode = 0600;
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modified;
  };
  struct Catalogue {
    std::filesystem::path source;
    struct stat revision{};
    std::map<std::string, Entry, std::less<>> entries;
    bool encryptedSevenZip = false;
  };
  struct Selection {
    std::shared_ptr<const Catalogue> catalogue;
    std::vector<std::string> members;
  };

  std::shared_ptr<const Catalogue>
  inspect(const std::filesystem::path& source, GCancellable* cancellable = nullptr, const Callbacks& callbacks = {});
  Result extractSelected(
      const Selection& selection, const std::filesystem::path& destination, GCancellable* cancellable = nullptr,
      const Callbacks& callbacks = {}
  );

  Result compress(
      std::span<const std::filesystem::path> sources, const std::filesystem::path& output, Format format, Level level,
      GCancellable* cancellable = nullptr, const Callbacks& callbacks = {}, const EncryptionOptions& options = {}
  );
  // Destination is an existing native directory. The archive's root entries are published inside it.
  Result extract(
      const std::filesystem::path& source, const std::filesystem::path& destination,
      GCancellable* cancellable = nullptr, const Callbacks& callbacks = {}, Extraction mode = Extraction::Direct
  );
} // namespace filemanager::archive_backend
