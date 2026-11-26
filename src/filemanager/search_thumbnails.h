#pragma once

#include "render/core/thumbnail_service.h"

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct FileEntry;

namespace filemanager {

  // Owns only the current viewport's search previews. Filesystem policy and the
  // shared service's source inspection both run off the UI thread.
  class SearchThumbnails {
  public:
    SearchThumbnails(ThumbnailService& service, std::function<void()> update);
    ~SearchThumbnails();

    [[nodiscard]] static bool eligible(const FileEntry& entry);
    [[nodiscard]] static bool permitsAutomaticPreview(GFileInfo* filesystem);

    void updateVisible(std::span<const std::string> paths, int targetPx);
    [[nodiscard]] TextureHandle peek(const std::string& path) const;

    // Cancel outstanding work immediately. Ready textures are released with
    // the render context current, at the next frame or during window teardown.
    void clear();
    void releaseRetired();

  private:
    struct Request {
      std::shared_ptr<GCancellable> cancellable;
      bool checked = false;
      bool allowed = false;
      bool acquired = false;
    };

    void queryPolicy(const std::string& path, Request& request);
    void retire(const std::string& path, const Request& request);

    ThumbnailService& m_service;
    std::function<void()> m_update;
    std::unordered_map<std::string, Request> m_requests;
    std::weak_ptr<GCancellable> m_policyInFlight;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::vector<std::pair<std::string, int>> m_retired;
    int m_targetPx = 0;
  };

} // namespace filemanager
