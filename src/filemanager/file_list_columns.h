#pragma once

#include "core/files/display_text.h"
#include "filemanager/directory_session.h"
#include "filemanager/file_columns.h"
#include "render/core/renderer.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/style.h"

#include <algorithm>
#include <filesystem>
#include <optional>

namespace filemanager {

  inline std::string searchLocationText(const std::filesystem::path& path) {
    return sanitizeFilenameForDisplay(path.parent_path().native());
  }

  inline float measureListColumnText(Renderer& renderer, FileColumn column, bool searchView, std::string_view text) {
    const float fontSize =
        column == FileColumn::Name ? Style::fontSizeBody : Style::fontSizeCaption + kMetadataFontSizeIncrease;
    TextFont font;
    if (fileColumnUsesTabularNumerals(column, searchView))
      font.features = "tnum";
    return renderer.measureText(text, fontSize, font).width;
  }

  inline float listColumnWidth(FileColumn column, float width) {
    // Leave room for the resize handle and an ellipsis, plus artwork in Name.
    const float minimum = column == FileColumn::Name ? 80.0F : 32.0F;
    return std::clamp(width, minimum, 10000.0F);
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

  inline std::string fileColumnText(const DirectorySession& session, const FileEntry& entry, FileColumn column) {
    const auto location =
        session.search().active() && column == kSearchLocationSlot ? searchLocationText(entry.absPath) : std::string{};
    const auto count = column == FileColumn::Size && entry.isDir ? session.cachedFolderItemCount(entry) : std::nullopt;
    return FileEntryRow::columnText(entry, column, count, location);
  }

  inline float measureFileColumn(Renderer& renderer, const DirectorySession& session, FileColumn column) {
    const bool searchView = session.search().active();
    float width = 0;
    for (const auto& entry : session.entries()) {
      const auto text = fileColumnText(session, entry, column);
      width = std::max(width, measureListColumnText(renderer, column, searchView, text));
    }
    return width;
  }

} // namespace filemanager
