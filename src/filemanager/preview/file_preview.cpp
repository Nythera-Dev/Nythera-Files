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
#include "ui/controls/separator.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_icon.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

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

    void setShown(Node& node, bool shown) {
      node.setVisible(shown);
      node.setParticipatesInLayout(shown);
    }
  } // namespace

  FilePreview::FilePreview(ThumbnailService& thumbnails, std::function<void()> redraw)
      : m_thumbnails(thumbnails), m_redraw(std::move(redraw)) {}

  FilePreview::~FilePreview() { clear(); }

  std::unique_ptr<Flex> FilePreview::build(
      std::function<void()> close, std::function<void()> open, std::function<void()> openWith, float headerHeight
  ) {
    auto root = std::make_unique<Flex>();
    root->setDirection(FlexDirection::Vertical);
    root->setAlign(FlexAlign::Stretch);
    root->setGap(Style::spaceSm);

    auto addLabel = [](Flex& box, std::string_view text = "") {
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setUseMarkup(false);
      label->setColor(colorSpecFromRole(ColorRole::OnSurface));
      return static_cast<Label*>(box.addChild(std::move(label)));
    };
    const bool docked = static_cast<bool>(close);
    if (docked) {
      auto header = std::make_unique<Flex>();
      header->setAlign(FlexAlign::Center);
      header->setMinHeight(headerHeight);
      header->setMaxHeight(headerHeight);
      header->setGap(Style::spaceSm);
      auto* heading = addLabel(*header, "Preview");
      heading->setFontWeight(FontWeight::SemiBold);
      heading->setFontSize(Style::fontSizeCaption);
      heading->setFlexGrow(1.0F);
      auto button = std::make_unique<Button>();
      button->setGlyph("x");
      button->setVariant(ButtonVariant::Ghost);
      button->setControlHeight(Style::controlHeightSm);
      button->setTooltip("Hide Preview Pane (F8)");
      button->setOnClick(std::move(close));
      header->addChild(std::move(button));
      root->addChild(std::move(header));
    }

    auto scroll = std::make_unique<ScrollView>();
    scroll->setFlexGrow(1.0F);
    scroll->setViewportPaddingH(0.0F);
    scroll->setViewportPaddingV(0.0F);
    auto& body = *scroll->content();
    body.setAlign(FlexAlign::Stretch);
    body.setGap(Style::spaceSm);
    m_scroll = scroll.get();
    m_empty = addLabel(body, "Select an item to preview");
    m_empty->setTextAlign(TextAlign::Center);
    m_empty->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));

    auto imageFrame = std::make_unique<Flex>();
    imageFrame->setClipChildren(true);
    auto image = std::make_unique<Image>();
    image->setFit(ImageFit::Contain);
    image->setParticipatesInLayout(false);
    m_image = static_cast<Image*>(imageFrame->addChild(std::move(image)));
    auto icon = std::make_unique<FileIcon>();
    icon->setAllowFilesystemQueries(false);
    icon->setParticipatesInLayout(false);
    m_icon = static_cast<FileIcon*>(imageFrame->addChild(std::move(icon)));
    m_imageFrame = static_cast<Flex*>(body.addChild(std::move(imageFrame)));
    if (docked) {
      m_name = addLabel(body);
      m_name->setFontWeight(FontWeight::SemiBold);
      m_name->setTextAlign(TextAlign::Center);
      m_name->setMaxLines(2);
      m_name->setEllipsize(TextEllipsize::End);
    }

    auto details = std::make_unique<Flex>();
    details->setDirection(FlexDirection::Vertical);
    details->setAlign(FlexAlign::Stretch);
    details->setGap(Style::spaceXs);
    m_details = static_cast<Flex*>(body.addChild(std::move(details)));
    m_notice = addLabel(body);
    m_notice->setFontSize(Style::fontSizeCaption);
    m_notice->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_notice->setMaxLines(0);

    auto divider = std::make_unique<Separator>();
    divider->setOrientation(SeparatorOrientation::HorizontalRule);
    divider->setColor(colorSpecFromRole(ColorRole::OutlineVariant));
    m_textDivider = body.addChild(std::move(divider));
    m_text = addLabel(body);
    m_text->setFontFamily("monospace");
    m_text->setFontSize(Style::fontSizeCaption);
    m_text->setMaxLines(0);
    root->addChild(std::move(scroll));

    if (docked) {
      auto actions = std::make_unique<Flex>();
      actions->setAlign(FlexAlign::Center);
      actions->setJustify(FlexJustify::End);
      actions->setGap(Style::spaceSm);
      actions->setWrap(true);
      auto addAction = [&](std::string_view label, ButtonVariant variant, std::function<void()> callback) {
        auto button = std::make_unique<Button>();
        button->setText(label);
        button->setVariant(variant);
        button->setOnClick(std::move(callback));
        return static_cast<Button*>(actions->addChild(std::move(button)));
      };
      m_openWith = addAction("Open With…", ButtonVariant::Outline, std::move(openWith));
      m_open = addAction("Open", ButtonVariant::Primary, std::move(open));
      root->addChild(std::move(actions));
    }
    return root;
  }

  void FilePreview::bindOpenAction(bool with, Button* button) {
    (with ? m_openWith : m_open) = button;
    if (button && !with)
      button->setVariant(ButtonVariant::Primary);
  }

  void FilePreview::setActionsEnabled(bool open, bool openWith) {
    if (m_open)
      m_open->setEnabled(open);
    if (m_openWith)
      m_openWith->setEnabled(openWith);
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
    m_name = m_empty = m_notice = m_text = nullptr;
    m_details = nullptr;
    m_textDivider = nullptr;
    m_open = m_openWith = nullptr;
    m_fields.clear();
    m_fieldsWidth = 0;
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

  FilePreview::Layout FilePreview::render(Renderer& renderer, Flex& root, bool initialLayout) {
    if (!m_scroll || root.width() <= 0.0F)
      return {.pending = true};
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
    const bool hasText = m_content && !m_content->text.empty();
    const bool fallback = entry && !handle.valid() && !hasText;
    const bool centeredSummary = fallback && !m_name;
    const bool pending = centeredSummary
        && (initialLayout || (!entry->isDir && !entry->isSymlink))
        && ((!m_content && !m_request->source.empty())
            || (m_content
                && m_content->imageAllowed
                && !m_imagePath.empty()
                && !m_thumbnails.failed(m_imagePath, m_imageSide, m_diskCache)));
    if (initialLayout && (pending || !centeredSummary))
      return {.pending = pending};
    setShown(*m_empty, !entry);
    setShown(*m_imageFrame, handle.valid() || fallback);
    m_image->setVisible(handle.valid());
    m_icon->setVisible(fallback);
    if (m_name) {
      m_name->setText(entry ? sanitizeFilenameForDisplay(entry->name) : "");
      setShown(*m_name, entry != nullptr);
    }
    m_notice->setText(m_content ? m_content->notice : "");
    m_notice->setTextAlign(centeredSummary ? TextAlign::Center : TextAlign::Start);
    setShown(*m_notice, !m_notice->text().empty());
    m_text->setText(hasText ? m_content->text : "");
    setShown(*m_textDivider, hasText);
    setShown(*m_text, hasText);

    std::vector<preview::Detail> fields;
    if (entry) {
      fields.push_back({"Type", FileEntryRow::columnText(*entry, FileColumn::Type, std::nullopt)});
      if (!entry->isDir)
        fields.push_back({"File Size", FileEntryRow::columnText(*entry, FileColumn::Size, std::nullopt)});
      if (m_content)
        fields.insert(fields.end(), m_content->details.begin(), m_content->details.end());
      auto modified = FileEntryRow::columnText(*entry, FileColumn::Modified, std::nullopt);
      if (!modified.empty())
        fields.push_back({"Modified", std::move(modified)});
      fields.push_back({"Location", sanitizeFilenameForDisplay(entry->absPath.parent_path().string())});
    }
    setShown(*m_details, !fields.empty());
    auto& body = *m_scroll->content();
    const float viewportHeight = m_scroll->height();
    body.setMinHeight(centeredSummary ? viewportHeight : 0.0F);
    body.setJustify(centeredSummary ? FlexJustify::Center : FlexJustify::Start);

    if (handle.valid()) {
      m_image->setExternalTexture(renderer, handle);
    } else {
      if (m_image->hasImage())
        m_image->clear(renderer);
      if (fallback) {
        m_icon->setFile(entry->absPath, entry->isDir);
        m_icon->setSymlinkState(entry->isSymlink);
      }
    }

    // ScrollView owns the padding and the scrollbar gutter. Measure against its
    // actual content width and repeat if this update changes scrollbar visibility.
    std::optional<float> compactHeight;
    const auto layout = [&](float width) {
      width = std::max(1.0F, width);
      const float detailWidth = centeredSummary ? std::min(width, 432.0F) : width;
      m_details->setPadding(0.0F, (width - detailWidth) * 0.5F);
      layoutDetails(fields, detailWidth);
      if (m_name)
        m_name->setMaxWidth(width);
      m_notice->setMaxWidth(width);
      m_text->setMaxWidth(width);

      const auto occupiedHeight = [&](Node* node) {
        if (!node || !node->visible())
          return 0.0F;
        auto constraints = LayoutConstraints::unconstrained();
        constraints.setExactWidth(width);
        return node->measure(renderer, constraints).height + body.gap();
      };
      const float detailsHeight = occupiedHeight(m_details) + occupiedHeight(m_notice);
      const float budget =
          std::max(std::min(72.0F, viewportHeight), viewportHeight - detailsHeight - occupiedHeight(m_name));
      float extent = 0.0F;
      if (handle.valid()) {
        const float imageBudget = m_request->mode == PreviewMode::Information ? std::min(budget, 168.0F) : budget;
        extent = std::min(imageBudget, width * static_cast<float>(handle.height) / static_cast<float>(handle.width));
      } else if (fallback) {
        const float iconHeight = std::min(m_name ? 80.0F : 104.0F, width);
        extent = std::min(iconHeight, budget);
        if (centeredSummary && !pending)
          compactHeight = iconHeight + detailsHeight + 2.0F * Style::spaceSm;
      }
      m_imageFrame->setMinHeight(extent);
      m_imageFrame->setMaxHeight(extent);
      root.layout(renderer);
      return extent;
    };
    const float width = body.width();
    float extent = layout(width);
    if (std::abs(body.width() - width) >= 0.5F)
      extent = layout(body.width());

    if (handle.valid()) {
      m_image->setPosition(0.0F, 0.0F);
      m_image->setSize(m_imageFrame->width(), extent);
      m_image->layout(renderer);
    } else if (fallback) {
      m_icon->setGlyphSize(extent);
      m_icon->setPosition(std::max(0.0F, (m_imageFrame->width() - extent) * 0.5F), 0.0F);
      m_icon->layout(renderer);
    }
    return {.pending = pending, .compactHeight = compactHeight};
  }

  void FilePreview::layoutDetails(const std::vector<preview::Detail>& fields, float width) {
    if (fields != m_fields || width != m_fieldsWidth) {
      while (!m_details->children().empty())
        m_details->removeChild(m_details->children().back().get());
      const float labelWidth = std::min(140.0F, width * 0.37F);
      for (const auto& field : fields) {
        auto row = std::make_unique<Flex>();
        row->setAlign(FlexAlign::Start);
        row->setGap(Style::spaceSm);
        auto label = std::make_unique<Label>();
        label->setText(field.label);
        label->setFontSize(Style::fontSizeCaption);
        label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        label->setMinWidth(labelWidth);
        label->setMaxWidth(labelWidth);
        label->setMaxLines(2);
        row->addChild(std::move(label));
        auto value = std::make_unique<Label>();
        value->setText(field.value);
        value->setUseMarkup(false);
        value->setFontSize(Style::fontSizeCaption);
        value->setMaxLines(2);
        value->setMaxWidth(std::max(1.0F, width - labelWidth - Style::spaceSm));
        value->setEllipsize(TextEllipsize::End);
        row->addChild(std::move(value));
        m_details->addChild(std::move(row));
      }
      m_fields = fields;
      m_fieldsWidth = width;
    }
  }
} // namespace filemanager
