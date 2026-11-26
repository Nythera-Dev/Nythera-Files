#pragma once

#include "core/files/display_text.h"
#include "filemanager/file_columns.h"

#include <filesystem>
#include <optional>

namespace filemanager {

  inline std::string searchLocationText(const std::filesystem::path& path) {
    return sanitizeFilenameForDisplay(path.parent_path().native());
  }

  inline float listColumnWidth(FileColumn column, float width) {
    return std::clamp(width, kMinimumFileColumns[column], 10000.0F);
  }

  inline float listContentWidth(const FileColumns& widths, std::span<const FileColumn> visible, float gap) {
    return fileColumnTotal(widths, visible) + static_cast<float>(visible.size() + 1) * gap;
  }

  inline FileColumns
  listDisplayWidths(FileColumns widths, float viewport, std::span<const FileColumn> visible, float gap) {
    const bool automaticName = widths[FileColumn::Name] == 0.0F;
    for (const auto column : visible)
      widths[column] = listColumnWidth(column, widths[column]);
    if (automaticName)
      widths[FileColumn::Name] = listColumnWidth(
          FileColumn::Name, widths[FileColumn::Name] + std::max(0.0F, viewport - listContentWidth(widths, visible, gap))
      );
    return widths;
  }

  struct FileColumnBounds {
    float left;
    float right;
  };

  inline FileColumnBounds listColumnBounds(
      FileColumn target, const FileColumns& widths, std::span<const FileColumn> visible, float gap, float offset = 0.0F
  ) {
    float left = gap - offset;
    for (const auto column : visible) {
      const float right = left + widths[column];
      if (target == column)
        return {left, right};
      left = right + gap;
    }
    return {left, left};
  }

} // namespace filemanager
