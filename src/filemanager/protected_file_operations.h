#pragma once

#include "filemanager/file_operation_engine.h"

namespace filemanager::file_operation_engine {
  Result executeProtectedFile(
      Request& request, GCancellable* cancellable, FileTransferPauseState* pause, const TransferCallbacks& callbacks
  );
}
