#pragma once

#include "filemanager/file_operation_engine.h"
#include "filemanager/file_operation_kind.h"

namespace filemanager::wastebin_operations {
  struct FallbackOffer {
    std::vector<std::string> sources;
    std::string mountPath;
    std::string trashDirectory;
    std::string diagnostic;
  };
  // Optional provider seam for isolated tests. Production operations use GIO's
  // trash:// backend; callbacks receive trash URIs and native destination paths.
  struct Backend {
    std::function<bool(GCancellable*, std::vector<std::string>&, std::string&)> enumerateTopLevel;
    std::function<bool(const std::string&, GCancellable*, std::string&, std::string&)> originalPath;
  };

  struct Result : file_operation_engine::Result {
    std::string customTitle;
    std::optional<std::size_t> requestedCount;
    std::size_t skippedEntries = 0;
    std::optional<FallbackOffer> trashFallback;
  };

  Result execute(
      FileOperationKind kind, const std::vector<std::string>& sources, const std::string& destinationDirectory,
      const Backend& backend, const file_operation_engine::ConflictResolver& conflictResolver,
      const file_operation_engine::TransferCallbacks& transferCallbacks, GCancellable* cancellable,
      file_operation_engine::FileTransferPauseState* pause
  );
} // namespace filemanager::wastebin_operations
