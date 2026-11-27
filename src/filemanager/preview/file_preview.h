#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/preview/preview_options.h"
#include "render/core/thumbnail_service.h"

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <vector>

class Button;
class FileIcon;
class Flex;
class Image;
class Label;
class Node;
class Renderer;
class ScrollView;

namespace filemanager {
  namespace preview {
    struct Request;
    struct Content;
    struct Detail;
  } // namespace preview

  class FilePreview {
  public:
    FilePreview(ThumbnailService& thumbnails, std::function<void()> redraw);
    ~FilePreview();
    FilePreview(const FilePreview&) = delete;
    FilePreview& operator=(const FilePreview&) = delete;

    std::unique_ptr<Flex> build(
        std::function<void()> close = {}, std::function<void()> open = {}, std::function<void()> openWith = {},
        float headerHeight = 0.0F
    );
    void bindOpenAction(bool with, Button* button);
    void setActionsEnabled(bool open, bool openWith);
    void select(
        const FileEntry* entry, std::string source, bool protectedCopy, PreviewMode mode, ThumbnailPolicy policy,
        int side
    );
    struct Layout {
      bool pending = false;
      std::optional<float> compactHeight;
    };
    Layout render(Renderer& renderer, Flex& root, bool initialLayout = false);
    void detach();
    void clear();

  private:
    void start();
    void releaseImage();
    void layoutDetails(const std::vector<preview::Detail>& fields, float width);

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
    Flex* m_summary = nullptr;
    Label* m_name = nullptr;
    Flex* m_details = nullptr;
    std::vector<preview::Detail> m_fields;
    float m_fieldsWidth = 0.0F;
    Button* m_open = nullptr;
    Button* m_openWith = nullptr;
    ScrollView* m_scroll = nullptr;
    Flex* m_imageFrame = nullptr;
    Image* m_image = nullptr;
    FileIcon* m_icon = nullptr;
    Label* m_empty = nullptr;
    Label* m_notice = nullptr;
    Label* m_text = nullptr;
    Node* m_textDivider = nullptr;
  };
} // namespace filemanager
