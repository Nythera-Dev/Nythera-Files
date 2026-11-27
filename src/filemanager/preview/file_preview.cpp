// Selection supersession adapted from Luciole (noctalia-dev), MIT. See LICENSE.luciole.
#include "filemanager/preview/file_preview.h"

#include "core/files/display_text.h"
#include "filemanager/preview/preview_content.h"
#include "render/core/renderer.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_icon.h"
#include "ui/style.h"

#include <algorithm>

namespace filemanager {
  namespace {
    bool sameEntry(const FileEntry& a, const FileEntry& b) {
      return a.absPath == b.absPath
          && a.name == b.name
          && a.type == b.type
          && a.size == b.size
          && a.mtime == b.mtime
          && a.isDir == b.isDir
          && a.isSymlink == b.isSymlink
          && a.hasIdentity == b.hasIdentity
          && a.device == b.device
          && a.inode == b.inode;
    }

  } // namespace

  FilePreview::FilePreview(ThumbnailService& thumbnails, std::function<void()> redraw)
      : m_thumbnails(thumbnails), m_redraw(std::move(redraw)) {}

  FilePreview::~FilePreview() { clear(); }

  std::unique_ptr<Flex> FilePreview::build(std::function<void()> close) {
    auto root = std::make_unique<Flex>();
    root->setDirection(FlexDirection::Vertical);
    root->setAlign(FlexAlign::Stretch);
    root->setGap(Style::spaceSm);

    auto addLabel = [](Flex& box, const char* text = "") {
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setColor(colorSpecFromRole(ColorRole::OnSurface));
      return static_cast<Label*>(box.addChild(std::move(label)));
    };

    if (close) {
      auto header = std::make_unique<Flex>();
      header->setGap(Style::spaceSm);
      addLabel(*header, "Preview")->setFlexGrow(1.0F);
      auto button = std::make_unique<Button>();
      button->setGlyph("x");
      button->setTooltip("Hide Preview Pane (F8)");
      button->setOnClick(std::move(close));
      header->addChild(std::move(button));
      root->addChild(std::move(header));
    }
    m_name = addLabel(*root);
    m_name->setMaxLines(1);
    m_name->setEllipsize(TextEllipsize::End);

    auto scroll = std::make_unique<ScrollView>();
    scroll->setFlexGrow(1.0F);
    scroll->content()->setGap(Style::spaceSm);
    m_scroll = scroll.get();
    auto imageFrame = std::make_unique<Flex>();
    imageFrame->setDirection(FlexDirection::Vertical);
    imageFrame->setAlign(FlexAlign::Stretch);
    imageFrame->setClipChildren(true);
    auto image = std::make_unique<Image>();
    image->setFit(ImageFit::Contain);
    image->setFlexGrow(1.0F);
    m_image = static_cast<Image*>(imageFrame->addChild(std::move(image)));
    auto icon = std::make_unique<FileIcon>();
    icon->setAllowFilesystemQueries(false);
    icon->setFlexGrow(1.0F);
    m_icon = static_cast<FileIcon*>(imageFrame->addChild(std::move(icon)));
    m_imageFrame = static_cast<Flex*>(scroll->content()->addChild(std::move(imageFrame)));
    m_text = addLabel(*scroll->content());
    m_text->setFontFamily("monospace");
    m_text->setMaxLines(200);
    m_details = addLabel(*scroll->content());
    m_details->setMaxLines(12);
    m_details->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    root->addChild(std::move(scroll));
    return root;
  }

  void FilePreview::releaseImage() {
    if (!m_imagePath.empty()) {
      m_thumbnails.release(m_imagePath, m_imageSide, m_diskCache);
      if (m_previousImageSide != 0) {
        m_thumbnails.release(m_imagePath, m_previousImageSide, m_diskCache);
        m_previousImageSide = 0;
      }
      m_imagePath.clear();
    }
  }

  void FilePreview::clear() {
    ++m_generation;
    if (m_cancel)
      g_cancellable_cancel(m_cancel.get());
    m_request.reset();
    m_content.reset();
    m_attempted = false;
    releaseImage();
  }

  void FilePreview::detach() {
    clear();
    m_name = m_details = m_text = nullptr;
    m_scroll = nullptr;
    m_imageFrame = nullptr;
    m_image = nullptr;
    m_icon = nullptr;
  }

  void FilePreview::select(
      const FileEntry* entry, std::string source, bool protectedCopy, PreviewMode mode, ThumbnailPolicy policy, int side
  ) {
    if (!entry || mode == PreviewMode::Hidden) {
      if (m_request)
        clear();
      return;
    }
    if (m_request
        && sameEntry(m_request->entry, *entry)
        && m_request->source == source
        && m_request->protectedCopy == protectedCopy
        && m_request->mode == mode
        && m_request->policy == policy) {
      m_request->side = side;
      return;
    }
    clear();
    m_request = std::make_unique<preview::Request>(
        preview::Request{*entry, std::move(source), protectedCopy, mode, policy, side}
    );
    if (m_scroll)
      m_scroll->requestScrollToOffset(0.0F);
    start();
  }

  void FilePreview::start() {
    if (m_running || m_attempted || !m_request || m_request->source.empty())
      return;
    m_running = m_attempted = true;
    m_cancel = std::shared_ptr<GCancellable>(g_cancellable_new(), g_object_unref);
    struct Completion {
      FilePreview* self;
      std::weak_ptr<bool> alive;
      std::uint64_t generation;
    };
    g_autoptr(GTask) task = g_task_new(
        nullptr, m_cancel.get(),
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Completion> completion(static_cast<Completion*>(data));
          auto content = std::unique_ptr<preview::Content>(
              static_cast<preview::Content*>(g_task_propagate_pointer(G_TASK(result), nullptr))
          );
          if (completion->alive.expired())
            return;
          auto& self = *completion->self;
          self.m_running = false;
          self.m_cancel.reset();
          if (completion->generation == self.m_generation) {
            self.m_content = std::move(content);
            self.m_redraw();
          }
          self.start();
        },
        new Completion{this, m_alive, m_generation}
    );
    g_task_set_task_data(task, new preview::Request(*m_request), [](gpointer data) {
      delete static_cast<preview::Request*>(data);
    });
    g_task_run_in_thread(task, [](GTask* work, gpointer, gpointer input, GCancellable* cancel) {
      auto* result = new preview::Content(preview::collect(*static_cast<const preview::Request*>(input), cancel));
      g_task_return_pointer(work, result, [](gpointer value) { delete static_cast<preview::Content*>(value); });
    });
  }

  void FilePreview::render(Renderer& renderer, float width, float imageHeight) {
    if (!m_name)
      return;
    const FileEntry* entry = m_request ? &m_request->entry : nullptr;
    if (m_content && m_content->imageAllowed && (m_imagePath.empty() || m_imageSide != m_request->side)) {
      const bool diskCache = m_request->mode == PreviewMode::Information && !m_request->protectedCopy;
      (void)m_thumbnails.acquire(m_request->source, m_request->side, diskCache);
      if (!m_imagePath.empty()) {
        if (m_thumbnails.peek(m_imagePath, m_imageSide, m_diskCache).valid()) {
          if (m_previousImageSide != 0)
            m_thumbnails.release(m_imagePath, m_previousImageSide, m_diskCache);
          m_previousImageSide = m_imageSide;
        } else {
          m_thumbnails.release(m_imagePath, m_imageSide, m_diskCache);
        }
      }
      m_imagePath = m_request->source;
      m_imageSide = m_request->side;
      m_diskCache = diskCache;
    }
    auto handle = m_imagePath.empty() ? TextureHandle{} : m_thumbnails.peek(m_imagePath, m_imageSide, m_diskCache);
    if (m_previousImageSide != 0) {
      if (handle.valid()) {
        m_thumbnails.release(m_imagePath, m_previousImageSide, m_diskCache);
        m_previousImageSide = 0;
      } else {
        handle = m_thumbnails.peek(m_imagePath, m_previousImageSide, m_diskCache);
      }
    }
    if (handle.valid())
      m_image->setExternalTexture(renderer, handle);
    else if (m_image->hasImage())
      m_image->clear(renderer);
    const bool fallback = entry && !handle.valid() && (!m_content || m_content->text.empty());
    m_image->setVisible(handle.valid());
    m_image->setParticipatesInLayout(handle.valid());
    m_icon->setVisible(fallback);
    m_icon->setParticipatesInLayout(fallback);
    if (fallback) {
      m_icon->setFile(entry->absPath, entry->isDir);
      m_icon->setSymlinkState(entry->isSymlink);
    }
    const float extent = fallback ? std::min(96.0F, imageHeight) : imageHeight;
    m_imageFrame->setMinHeight(extent);
    m_imageFrame->setMaxHeight(extent);
    m_imageFrame->setVisible(handle.valid() || fallback);
    m_imageFrame->setParticipatesInLayout(handle.valid() || fallback);

    m_name->setMaxWidth(width);
    m_name->setText(entry ? sanitizeFilenameForDisplay(entry->name) : "Select a file to preview");
    m_text->setMaxWidth(width);
    m_text->setText(m_content ? m_content->text : "");
    m_text->setVisible(!m_text->text().empty());
    m_text->setParticipatesInLayout(m_text->visible());
    std::string details;
    if (entry) {
      details = FileEntryRow::columnText(*entry, FileColumn::Type, std::nullopt);
      if (!entry->isDir)
        details += "\n" + FileEntryRow::columnText(*entry, FileColumn::Size, std::nullopt);
      const auto modified = FileEntryRow::columnText(*entry, FileColumn::Modified, std::nullopt);
      if (!modified.empty())
        details += "\n" + modified;
      if (m_content && !m_content->details.empty())
        details += "\n" + m_content->details;
    }
    m_details->setMaxWidth(width);
    m_details->setText(details);
  }
} // namespace filemanager
