#include "filemanager/file_list_columns.h"
#include "filemanager/file_manager.h"
#include "render/core/renderer.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/style.h"

#include <algorithm>

std::string FileManager::fileColumnText(const FileEntry& entry, filemanager::FileColumn column) const {
  const auto& session = activeSession();
  const auto location = session.search().active() && column == filemanager::kSearchLocationSlot
      ? filemanager::searchLocationText(entry.absPath)
      : std::string{};
  const auto count =
      column == filemanager::FileColumn::Size && entry.isDir ? session.cachedFolderItemCount(entry) : std::nullopt;
  return FileEntryRow::columnText(entry, column, count, location);
}

float FileManager::measureFileColumn(Renderer& renderer, filemanager::FileColumn column) const {
  const auto fontSize = column == filemanager::FileColumn::Name ? Style::fontSizeBody : Style::fontSizeCaption;
  float width = 0;
  for (const auto& entry : activeSession().entries()) {
    const auto text = fileColumnText(entry, column);
    width = std::max(width, renderer.measureText(text, fontSize).width);
  }
  return width;
}

void FileManager::openFileColumnMenu(std::optional<filemanager::FileColumn> column, float x, float y) {
  using namespace filemanager::menu_id;
  auto entries = m_menus.columnVisibilityEntries();
  entries.push_back({.separator = true});
  if (column) {
    entries.push_back({
        .id = kFirstFitColumn + static_cast<int>(*column),
        .label = "Fit " + std::string(m_listHeader.columnLabel(*column)) + " to Contents",
    });
  }
  entries.push_back({.id = kFitAllColumns, .label = "Fit All Columns to Contents"});
  m_menus.openColumnHeaderMenu(std::move(entries), x, y);
}
