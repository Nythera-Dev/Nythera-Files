#pragma once

#include "filemanager/preview/preview_content.h"
#include "render/core/image_decoder.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace filemanager::preview {
  [[nodiscard]] bool mediaThumbnail(std::string_view mime);
  [[nodiscard]] bool imageThumbnail(std::string_view mime);
  [[nodiscard]] std::optional<DecodedRasterImage> provideThumbnail(
      int fd, const std::string& path, std::size_t bytes, std::size_t sourceLimit, int side, bool browsing,
      const std::function<bool()>& canceled
  );
  [[nodiscard]] std::vector<Detail> mediaDetails(int fd, std::string_view mime, const std::function<bool()>& canceled);
  int runImageWorker(const char* side, const char* output);
} // namespace filemanager::preview
