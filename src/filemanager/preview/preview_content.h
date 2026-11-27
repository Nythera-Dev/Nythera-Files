#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/preview/preview_options.h"

#include <gio/gio.h>
#include <string>
#include <string_view>

namespace filemanager::preview {
  struct Request {
    FileEntry entry;
    std::string source;
    bool protectedCopy = false;
    PreviewMode mode = PreviewMode::Hidden;
    ThumbnailPolicy policy = ThumbnailPolicy::Local;
    int side = 0;
  };

  struct Content {
    bool imageAllowed = false;
    std::string text;
    std::string details;
  };

  [[nodiscard]] Content collect(const Request& request, GCancellable* cancel);
  [[nodiscard]] bool textMime(std::string_view mime);
  [[nodiscard]] std::string previewText(std::string_view bytes, bool truncated);
} // namespace filemanager::preview
