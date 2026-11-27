#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_manager.h"
#include "render/core/renderer.h"
#include "ui/controls/flex.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

void FileManager::attachPreviewPane(Flex& host, float dividerWidth, const ColorSpec& fill) {
  auto previewDivider = std::make_unique<Flex>();
  previewDivider->setMinWidth(dividerWidth);
  previewDivider->setMaxWidth(dividerWidth);
  m_previewDivider = static_cast<Flex*>(host.addChild(std::move(previewDivider)));
  auto previewPane = m_selectionPreview.build(
      [this] { setPreviewMode(filemanager::PreviewMode::Hidden); }, [this] { openPreviewed(false); },
      [this] { openPreviewed(true); }, filemanager::kFileListHeaderHeight
  );

  previewPane->setPadding(0.0F, Style::spaceMd, Style::spaceMd, Style::spaceMd);
  previewPane->setFill(fill);
  previewPane->setRadius(Style::scaledRadiusMd(1.0F));
  m_previewPane = static_cast<Flex*>(host.addChild(std::move(previewPane)));
  m_previewWidth = m_settings.loadPreviewWidth();
}

void FileManager::layoutPreviewPane(float available, float dividerSpace) {
  const float previewBudget = std::min(800.0F, available - 440.0F - dividerSpace);
  const bool showPreview = m_settings.previewMode() != filemanager::PreviewMode::Hidden && previewBudget >= 208.0F;
  m_previewDivider->setVisible(showPreview);
  m_previewDivider->setParticipatesInLayout(showPreview);
  m_previewPane->setVisible(showPreview);
  m_previewPane->setParticipatesInLayout(showPreview);
  const float previewSpace = showPreview ? std::min(m_previewWidth, previewBudget) : 0.0F;
  if (showPreview) {
    m_previewWidthLimit = previewBudget;
    m_previewPane->setMinWidth(previewSpace);
    m_previewPane->setMaxWidth(previewSpace);
  } else {
    m_previewResize.reset();
  }
}

void FileManager::renderPreviewPane(Renderer& renderer) {
  if (m_previewPane->visible()) {
    const auto mode = m_settings.previewMode();
    const float width = std::max(1.0F, m_previewPane->width() - 2.0F * Style::spaceMd);
    const float height = std::max(1.0F, m_previewPane->height() - 2.0F * Style::controlHeightSm);
    const int pixels = mode == filemanager::PreviewMode::Information
        ? ThumbnailService::kDefaultTargetPx
        : std::clamp(
              static_cast<int>(std::ceil(std::max(width, height) * renderer.renderScale() / 64.0F)) * 64, 128, 2048
          );
    syncFilePreview(m_selectionPreview, mode, pixels);
    m_selectionPreview.render(renderer, *m_previewPane);
  } else {
    syncFilePreview(m_selectionPreview, filemanager::PreviewMode::Hidden, 0);
  }
}

bool FileManager::handlePreviewDivider(const PointerEvent& event, float x, float y) {
  if (!m_previewDivider || !m_previewDivider->visible())
    return false;
  float dividerX = 0;
  float dividerY = 0;
  Node::absolutePosition(m_previewDivider, dividerX, dividerY);
  const bool hovered = x >= dividerX - 2.0F
      && x <= dividerX + m_previewDivider->width() + 2.0F
      && y >= dividerY
      && y <= dividerY + m_previewDivider->height();
  m_previewDivider->setFill(hovered || m_previewResize ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec());
  if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT) {
    if (event.pressed && hovered) {
      m_dispatcher.cancelPointerCapture();
      m_previewResize = m_previewPane->width() + x;
      return true;
    }
    if (!event.pressed && m_previewResize) {
      m_previewResize.reset();
      (void)m_settings.savePreviewWidth(m_previewWidth);
      return true;
    }
  }
  if (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter) {
    if (m_previewResize) {
      m_previewWidth = std::clamp(*m_previewResize - x, 208.0F, m_previewWidthLimit);
      m_surface->requestLayout();
    }
    if (hovered || m_previewResize) {
      m_dispatcher.pointerLeave();
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      m_surface->requestRedraw();
      return true;
    }
  }
  return m_previewResize.has_value();
}
