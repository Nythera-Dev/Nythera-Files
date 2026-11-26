#include "filemanager/search_thumbnails.h"

#include "core/files/directory_scanner.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace filemanager {

  SearchThumbnails::SearchThumbnails(ThumbnailService& service, std::function<void()> update)
      : m_service(service), m_update(std::move(update)) {}

  SearchThumbnails::~SearchThumbnails() {
    m_alive.reset();
    clear();
    releaseRetired();
  }

  bool SearchThumbnails::eligible(const FileEntry& entry) {
    constexpr std::array<std::string_view, 12> supported = {
        "image/jpeg", "image/png",  "image/webp", "image/jxl",  "image/bmp",     "image/x-ms-bmp",
        "image/gif",  "image/heic", "image/heif", "image/avif", "image/svg+xml", "image/svg+xml-compressed",
    };
    const auto limit = entry.type == "image/svg+xml" || entry.type == "image/svg+xml-compressed"
        ? ThumbnailService::kMaxSvgSourceBytes
        : ThumbnailService::kMaxSourceFileBytes;
    return !entry.isDir
        && !entry.isSymlink
        && entry.size > 0
        && entry.size <= limit
        && std::ranges::contains(supported, entry.type);
  }

  bool SearchThumbnails::permitsAutomaticPreview(GFileInfo* filesystem) {
    if (!filesystem
        || !g_file_info_has_attribute(filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE)
        || g_file_info_get_attribute_boolean(filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE))
      return false;
    return !g_file_info_has_attribute(filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW)
        || g_file_info_get_attribute_uint32(filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW)
        == G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL;
  }

  void SearchThumbnails::queryPolicy(const std::string& path, Request& request) {
    struct Query {
      SearchThumbnails* owner;
      std::string path;
      std::weak_ptr<bool> alive;
      std::shared_ptr<GCancellable> cancellable;
    };
    request.cancellable = std::shared_ptr<GCancellable>(g_cancellable_new(), g_object_unref);
    m_policyInFlight = request.cancellable;
    g_autoptr(GFile) file = g_file_new_for_path(path.c_str());
    g_file_query_filesystem_info_async(
        file, "filesystem::remote,filesystem::use-preview", G_PRIORITY_LOW, request.cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Query> query(static_cast<Query*>(data));
          g_autoptr(GFileInfo) filesystem = g_file_query_filesystem_info_finish(G_FILE(source), result, nullptr);
          if (query->alive.expired())
            return;
          auto& self = *query->owner;
          if (!g_cancellable_is_cancelled(query->cancellable.get())) {
            auto& pending = self.m_requests.at(query->path);
            pending.cancellable.reset();
            pending.checked = true;
            pending.allowed = permitsAutomaticPreview(filesystem);
          }
          self.m_policyInFlight.reset();
          if (!self.m_requests.empty())
            self.m_update();
        },
        new Query{this, path, m_alive, request.cancellable}
    );
  }

  void SearchThumbnails::updateVisible(std::span<const std::string> paths, int targetPx) {
    if (m_targetPx != targetPx) {
      clear();
      m_targetPx = targetPx;
    }
    const std::unordered_set<std::string_view> visible(paths.begin(), paths.end());
    std::erase_if(m_requests, [&](const auto& item) {
      if (visible.contains(item.first))
        return false;
      retire(item.first, item.second);
      return true;
    });
    for (const auto& path : paths) {
      auto& request = m_requests.try_emplace(path).first->second;
      // Keep preview policy I/O bounded and lower priority than discovery.
      if (!request.checked && m_policyInFlight.expired())
        queryPolicy(path, request);
      if (request.allowed && !request.acquired) {
        (void)m_service.acquire(path, targetPx);
        request.acquired = true;
      }
    }
    releaseRetired();
  }

  TextureHandle SearchThumbnails::peek(const std::string& path) const {
    const auto entry = m_requests.find(path);
    return entry != m_requests.end() && entry->second.acquired ? m_service.peek(path, m_targetPx) : TextureHandle{};
  }

  void SearchThumbnails::retire(const std::string& path, const Request& request) {
    if (request.cancellable)
      g_cancellable_cancel(request.cancellable.get());
    if (!request.acquired)
      return;
    if (m_service.peek(path, m_targetPx).id == 0)
      m_service.release(path, m_targetPx);
    else
      m_retired.emplace_back(path, m_targetPx);
  }

  void SearchThumbnails::clear() {
    for (const auto& [path, request] : m_requests)
      retire(path, request);
    m_requests.clear();
  }

  void SearchThumbnails::releaseRetired() {
    for (const auto& [path, targetPx] : m_retired)
      m_service.release(path, targetPx);
    m_retired.clear();
  }

} // namespace filemanager
