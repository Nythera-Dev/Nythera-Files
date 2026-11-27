#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/preview/preview_options.h"

#include <gio/gio.h>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager::preview {
  struct Request {
    FileEntry entry;
    std::string source;
    bool protectedCopy = false;
    PreviewMode mode = PreviewMode::Hidden;
    ThumbnailPolicy policy = ThumbnailPolicy::Local;
    int side = 0;
  };

  struct Detail {
    std::string label;
    std::string value;
    bool operator==(const Detail&) const = default;
  };

  struct TextPreview {
    std::string text;
    std::string notice;
  };

  struct Content {
    bool imageAllowed = false;
    std::string text;
    std::string notice;
    std::vector<Detail> details;
  };

  [[nodiscard]] Content collect(const Request& request, GCancellable* cancel);
  [[nodiscard]] bool textMime(std::string_view mime);
  [[nodiscard]] TextPreview previewText(std::string_view bytes, bool truncated);
} // namespace filemanager::preview
