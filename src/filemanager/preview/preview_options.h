#pragma once

#include <cstdint>

namespace filemanager {
  enum class PreviewMode : std::uint8_t { Hidden, Information, Full };
  enum class ThumbnailPolicy : std::uint8_t { Never, Local, All };
} // namespace filemanager
