#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/preview/preview_options.h"
#include "render/core/thumbnail_service.h"

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>

class FileIcon;
class Flex;
class Image;
class Label;
class Renderer;
class ScrollView;

namespace filemanager {
  namespace preview {
    struct Request;
    struct Content;
  } // namespace preview

  class FilePreview {
  public:
    FilePreview(ThumbnailService& thumbnails, std::function<void()> redraw);
    ~FilePreview();
    FilePreview(const FilePreview&) = delete;
    FilePreview& operator=(const FilePreview&) = delete;

    std::unique_ptr<Flex> build(std::function<void()> close = {});
    void select(
        const FileEntry* entry, std::string source, bool protectedCopy, PreviewMode mode, ThumbnailPolicy policy,
        int side
    );
    void render(Renderer& renderer, float width, float imageHeight);
    void detach();
    void clear();

  private:
    void start();
    void releaseImage();

    ThumbnailService& m_thumbnails;
    std::function<void()> m_redraw;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::unique_ptr<preview::Request> m_request;
    std::unique_ptr<preview::Content> m_content;
    std::shared_ptr<GCancellable> m_cancel;
    bool m_running = false;
    bool m_attempted = false;
    std::uint64_t m_generation = 0;
    std::string m_imagePath;
    int m_imageSide = 0;
    int m_previousImageSide = 0;
    bool m_diskCache = false;
    Label* m_name = nullptr;
    Label* m_details = nullptr;
    ScrollView* m_scroll = nullptr;
    Flex* m_imageFrame = nullptr;
    Image* m_image = nullptr;
    FileIcon* m_icon = nullptr;
    Label* m_text = nullptr;
  };
} // namespace filemanager
