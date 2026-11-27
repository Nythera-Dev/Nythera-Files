#pragma once

#include "filemanager/file_operation_engine.h"

#include <array>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace filemanager::archive_backend {
  enum class Format { Zip, SevenZip, Tar, TarGzip, TarXz, TarZstd, TarBzip2, Gzip, Rar };
  enum class Level { Fast, Balanced, Maximum };

  struct Capability {
    Format format;
    std::string_view suffix;
    bool extract;
    bool create;
    std::array<bool, 3> levels;
  };

  std::span<const Capability> capabilities();
  std::optional<Format> formatFromName(std::string_view name);

  struct Callbacks {
    file_operation_engine::ConflictResolver resolveConflict;
    // Synchronous callbacks on the calling thread. Checkpoints also observe GCancellable.
    std::function<void()> checkpoint;
    std::function<void(std::uint64_t bytes, std::uint64_t items)> progress;
  };

  enum class SkipReason { Conflict };

  struct SkippedEntry {
    std::string archivePath;
    SkipReason reason;
  };

  struct Result : file_operation_engine::Result {
    std::vector<SkippedEntry> skippedEntries;
  };

  Result compress(
      std::span<const std::filesystem::path> sources, const std::filesystem::path& output, Format format, Level level,
      GCancellable* cancellable = nullptr, const Callbacks& callbacks = {}
  );
  // Destination is an existing native directory. The archive's root entries are published inside it.
  Result extract(
      const std::filesystem::path& source, const std::filesystem::path& destination,
      GCancellable* cancellable = nullptr, const Callbacks& callbacks = {}
  );
} // namespace filemanager::archive_backend
