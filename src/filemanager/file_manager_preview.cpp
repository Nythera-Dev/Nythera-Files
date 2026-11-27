#include "filemanager/file_interaction.h"
#include "filemanager/file_manager.h"
#include "filemanager/preview/preview_options.h"
#include "ui/controls/virtual_grid_view.h"

#include <algorithm>
#include <format>

const FileEntry* FileManager::selectedPreviewEntry() {
  auto& tab = activeTab();
  if (!tab.capabilities().canGenerateThumbnails
      || !tab.selectedPath
      || tab.selectedPaths.size() != 1
      || !tab.selectedPaths.contains(tab.selectedPath->string())
      || !tab.selectedPath->is_absolute())
    return nullptr;
  const auto& entries = tab.session.entries();
  if (m_grid) {
    const auto index = m_grid->selectedIndex();
    if (index && *index < entries.size() && entries[*index].absPath == *tab.selectedPath)
      return &entries[*index];
  }
  const auto found = std::ranges::find(entries, *tab.selectedPath, &FileEntry::absPath);
  return found == entries.end() ? nullptr : &*found;
}

void FileManager::setPreviewMode(filemanager::PreviewMode mode) {
  if (mode != filemanager::PreviewMode::Hidden)
    m_lastVisiblePreviewMode = mode;
  if (!m_settings.setPreviewMode(mode))
    setStatus("Could not save the preview pane setting");
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::togglePreviewPane() {
  const auto current = m_settings.previewMode();
  if (current == filemanager::PreviewMode::Hidden) {
    setPreviewMode(m_lastVisiblePreviewMode);
  } else {
    m_lastVisiblePreviewMode = current;
    setPreviewMode(filemanager::PreviewMode::Hidden);
  }
}

void FileManager::syncFilePreview(filemanager::FilePreview& preview, filemanager::PreviewMode mode, int side) {
  auto& tab = activeTab();
  const auto* entry = selectedPreviewEntry();
  std::string source;
  bool protectedCopy = false;
  const auto policy = m_settings.thumbnailPolicy();
  if (entry && mode != filemanager::PreviewMode::Hidden) {
    if (!tab.hasActiveAdminSession() || filemanager::userCanReadPath(entry->absPath)) {
      source = entry->absPath.string();
    } else if (policy != filemanager::ThumbnailPolicy::Never) {
      const auto& entries = tab.session.entries();
      const auto index = static_cast<std::size_t>(entry - entries.data());
      if (auto copy = protectedThumbnailPath(tab, *entry, index); copy && !copy->empty()) {
        source = std::move(*copy);
        protectedCopy = true;
      }
    }
  }
  preview.select(entry, std::move(source), protectedCopy, mode, policy, side);
}

void FileManager::applyThumbnailLimits() {
  constexpr std::size_t mib = 1024 * 1024;
  m_pickerThumbnails.setSourceFileLimit(static_cast<std::size_t>(m_settings.thumbnailMaxFileMiB()) * mib);
  m_pickerThumbnails.setFreedesktopCacheBudget(static_cast<std::size_t>(m_settings.thumbnailCacheMiB()) * mib);
}

void FileManager::setThumbnailPolicy(filemanager::ThumbnailPolicy policy) {
  if (!m_settings.setThumbnailPolicy(policy)) {
    setStatus("Could not save the thumbnail policy");
    return;
  }
  m_searchThumbnails.clear();
  if (m_surface)
    m_surface->requestLayout();
  if (m_grid)
    m_grid->notifyDataChanged();
}

void FileManager::inspectThumbnailCache(bool clear) {
  const auto report = [this, alive = std::weak_ptr<bool>(m_alive)](ThumbnailService::DiskCacheInfo info) {
    const auto lifetime = alive.lock();
    if (!lifetime || !*lifetime)
      return;
    g_autofree char* bytes = g_format_size(info.bytes);
    setStatus(std::format("Shared thumbnail cache: {} thumbnails · {}", info.thumbnails, bytes));
  };
  if (!clear) {
    m_pickerThumbnails.requestFreedesktopCacheInfo(false, report);
    return;
  }
  showDialog(
      "Clear Thumbnail Cache", "Remove thumbnails saved by Nythera Files from the shared cache?",
      {{"Cancel", 0}, {"Clear", 1}}, [this, report](int response) {
        if (response == 1)
          m_pickerThumbnails.requestFreedesktopCacheInfo(true, report);
      }
  );
}
