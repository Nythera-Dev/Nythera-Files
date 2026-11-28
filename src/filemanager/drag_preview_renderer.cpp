#include "filemanager/drag_preview_renderer.h"

#include "core/deferred_call.h"
#include "core/files/display_text.h"
#include "core/ui_phase.h"
#include "render/core/renderer.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wayland-client-protocol.h>

namespace filemanager {

  DragPreviewRenderer::~DragPreviewRenderer() { shutdown(); }

  void DragPreviewRenderer::shutdown() {
    if (m_alive)
      *m_alive = false;
    clearNative();
  }

  void DragPreviewRenderer::detachUi() {
    m_dragPreview = nullptr;
    m_dragPreviewIcon = nullptr;
    m_dragPreviewLabel = nullptr;
    m_dragPreviewAction = nullptr;
  }

  void DragPreviewRenderer::clearNative() {
    ++m_nativePreviewGeneration;
    m_nativePreviewQueued = false;
    m_nativePreviewTarget.reset();
    if (m_nativePreviewSurface)
      wl_surface_destroy(m_nativePreviewSurface);
    m_nativePreviewSurface = nullptr;
  }

  void DragPreviewRenderer::hideOverlay() {
    if (m_dragPreview)
      m_dragPreview->setVisible(false);
  }

  void DragPreviewRenderer::scheduleNative(
      RenderContext& renderContext, ToplevelSurface* mainSurface, std::function<bool()> isActive,
      std::function<void(Renderer&)> preparePreview, std::function<void(const std::string&)> onError
  ) {
    if (!m_nativePreviewSurface || m_nativePreviewQueued || (isActive && !isActive()))
      return;
    m_nativePreviewQueued = true;
    const auto generation = m_nativePreviewGeneration;
    std::weak_ptr<bool> alive = m_alive;
    DeferredCall::callLater([this, alive, generation, &renderContext, mainSurface, isActive = std::move(isActive),
                             preparePreview = std::move(preparePreview), onError = std::move(onError)] {
      const auto locked = alive.lock();
      if (!locked || !*locked || generation != m_nativePreviewGeneration)
        return;
      m_nativePreviewQueued = false;
      if (!mainSurface || !m_nativePreviewSurface || (isActive && !isActive()))
        return;
      try {
        if (!renderContext.makeCurrent(mainSurface->renderTarget()))
          return;
        const bool canScale =
            wl_surface_get_version(m_nativePreviewSurface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
        const int scale = canScale
            ? std::clamp(static_cast<int>(std::ceil(mainSurface->renderTarget().renderer().renderScale())), 1, 4)
            : 1;
        ScaledRenderer renderer(renderContext, static_cast<float>(scale));
        {
          UiPhaseScope layoutPhase(UiPhase::Layout);
          if (preparePreview)
            preparePreview(renderer);
        }
        if (!m_dragPreview)
          return;
        const auto width = static_cast<std::uint32_t>(std::ceil(m_dragPreview->width()));
        const auto height = static_cast<std::uint32_t>(std::ceil(m_dragPreview->height()));
        if (!width || !height)
          return;
        if (!m_nativePreviewTarget) {
          m_nativePreviewTarget = std::make_unique<RenderTarget>();
          m_nativePreviewTarget->create(m_nativePreviewSurface, renderContext);
        }
        if (canScale)
          wl_surface_set_buffer_scale(m_nativePreviewSurface, scale);
        m_nativePreviewTarget->setContentScale(static_cast<float>(scale));
        m_nativePreviewTarget->setLogicalSize(width, height);
        m_nativePreviewTarget->resize(
            width * static_cast<std::uint32_t>(scale), height * static_cast<std::uint32_t>(scale)
        );
        const auto previousX = m_dragPreview->x(), previousY = m_dragPreview->y();
        m_dragPreview->setPosition(0, 0);
        renderContext.renderScene(*m_nativePreviewTarget, m_dragPreview);
        m_dragPreview->setPosition(previousX, previousY);
        m_dragPreview->setVisible(false);
        renderContext.makeCurrent(mainSurface->renderTarget());
      } catch (const std::exception& error) {
        if (onError)
          onError(error.what());
      }
    });
  }

  void DragPreviewRenderer::updateOverlay(
      Renderer& renderer, Flex* root, ToplevelSurface* surface, const DragPreviewState& state
  ) {
    if (!root || !surface)
      return;
    if (!m_dragPreview) {
      auto preview = std::make_unique<Flex>();
      preview->setParticipatesInLayout(false);
      preview->setHitTestVisible(false);
      preview->setExcludeSubtreeFromTabOrder(true);
      preview->setZIndex(1000);
      preview->setPadding(8.0F);
      preview->setGap(8.0F);
      preview->setAlign(FlexAlign::Center);
      constexpr float previewOpacity = 0.94F;
      preview->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh, previewOpacity));
      preview->setRadius(Style::scaledRadiusMd(1.0F));
      auto icon = std::make_unique<FileIcon>();
      icon->setHitTestVisible(false);
      icon->setGlyphSize(36.0F);
      m_dragPreviewIcon = static_cast<FileIcon*>(preview->addChild(std::move(icon)));
      auto text = std::make_unique<Flex>();
      text->setDirection(FlexDirection::Vertical);
      text->setHitTestVisible(false);
      auto name = std::make_unique<Label>();
      name->setFontSize(Style::fontSizeBody);
      name->setMaxWidth(220.0F);
      name->setMaxLines(1);
      name->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_dragPreviewLabel = static_cast<Label*>(text->addChild(std::move(name)));
      auto action = std::make_unique<Label>();
      action->setFontSize(Style::fontSizeCaption);
      action->setColor(colorSpecFromRole(ColorRole::Primary));
      m_dragPreviewAction = static_cast<Label*>(text->addChild(std::move(action)));
      preview->addChild(std::move(text));
      m_dragPreview = static_cast<Flex*>(root->addChild(std::move(preview)));
    }
    m_dragPreviewIcon->setAllowFilesystemQueries(state.allowFilesystemQueries);
    m_dragPreviewIcon->setFile(state.primaryPath, state.primaryIsDir);
    m_dragPreviewLabel->setText(
        state.itemCount == 1 ? sanitizeFilenameForDisplay(state.primaryPath.filename().string())
                             : std::to_string(state.itemCount) + " selected items"
    );
    m_dragPreviewAction->setText(state.actionLabel);
    constexpr float previewBorderOpacity = 0.85F;
    m_dragPreview->setBorder(
        colorSpecFromRole(state.targetValid ? ColorRole::Primary : ColorRole::OutlineVariant, previewBorderOpacity),
        1.0F
    );
    m_dragPreview->setVisible(true);
    m_dragPreview->layout(renderer);
    float px = state.pointerX + 18.0F;
    float py = state.pointerY + 20.0F;
    if (px + m_dragPreview->width() > static_cast<float>(surface->width()))
      px = state.pointerX - m_dragPreview->width() - 18.0F;
    if (py + m_dragPreview->height() > static_cast<float>(surface->height()))
      py = state.pointerY - m_dragPreview->height() - 20.0F;
    m_dragPreview->setPosition(std::max(0.0F, px), std::max(0.0F, py));
  }

} // namespace filemanager
