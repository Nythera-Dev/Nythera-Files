#include "core/files/display_text.h"
#include "core/input/key_symbols.h"
#include "filemanager/file_manager.h"
#include "render/core/renderer.h"
#include "ui/controls/flex.h"
#include "ui/controls/virtual_grid_view.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>

bool FileManager::openQuickLook() {
  if (activeSession().browsingArchive())
    return false;
  const auto* entry = selectedPreviewEntry();
  if (!m_surface || !m_grid || m_modalDialog.isOpen() || !entry)
    return false;
  using filemanager::ModalDialogHost;
  const auto layoutPreview = [this](Renderer& renderer, float dialogHeight, bool initialLayout) {
    const float width = m_quickLookContent->width();
    const float height = static_cast<float>(m_modalDialog.normalHeight());
    const int pixels = std::clamp(
        static_cast<int>(std::ceil(std::max(width, height) * renderer.renderScale() / 128.0F)) * 128, 256, 2048
    );
    syncFilePreview(m_quickLook, filemanager::PreviewMode::Full, pixels);
    (void)m_pickerThumbnails.uploadPending(renderer.textureManager());
    const auto* selected = selectedPreviewEntry();
    m_modalDialog.setTitle(selected ? filemanager::sanitizeFilenameForDisplay(selected->name) : "Quick Look");
    auto layout = m_quickLook.render(renderer, *m_quickLookContent, initialLayout);
    if (layout.compactHeight)
      *layout.compactHeight += dialogHeight - m_quickLookContent->height();
    return layout;
  };
  m_quickLookOpen = true;
  m_modalDialog.showDialog(
      filemanager::sanitizeFilenameForDisplay(entry->name), "", {{"Open With…", 2}, {"Open", 1}},
      [this](int result) {
        if (result == 1 || result == 2)
          openPreviewed(result == 2);
      },
      m_surface.get(),
      ModalDialogHost::CustomContent{
          .buildFields =
              [this](Flex& root, float) {
                auto content = m_quickLook.build();
                content->setFlexGrow(1.0F);
                m_quickLookContent = static_cast<Flex*>(root.addChild(std::move(content)));
              },
          .bindChoiceButton = [this](int result, Button* button) { m_quickLook.bindOpenAction(result == 2, button); },
          .onPrepareLayout =
              [this, layoutPreview](Renderer& renderer) {
                const auto layout =
                    layoutPreview(renderer, static_cast<float>(m_modalDialog.surface()->height()), false);
                if (!layout.pending)
                  m_modalDialog.setCompactHeight(layout.compactHeight);
              },
          .onInitialLayout =
              [layoutPreview](Renderer& renderer, float height, std::optional<float>& compactHeight) {
                const auto layout = layoutPreview(renderer, height, true);
                compactHeight = layout.compactHeight;
                return !layout.pending;
              },
          .onKeyEvent =
              [this](const KeyboardEvent& event) {
                if (event.modifiers != 0)
                  return false;
                if (KeySymbol::isSpace(event.sym)) {
                  if (!event.repeat)
                    m_modalDialog.closeDialog(0);
                  return true;
                }
                const bool vertical = KeySymbol::isUp(event.sym) || KeySymbol::isDown(event.sym);
                if (!vertical && !KeySymbol::isLeft(event.sym) && !KeySymbol::isRight(event.sym))
                  return false;
                if (const auto* selected = selectedPreviewEntry()) {
                  const auto& entries = activeSession().entries();
                  const auto current = selected - entries.data();
                  const auto stride =
                      vertical ? static_cast<std::ptrdiff_t>(std::max<std::size_t>(1, m_grid->layoutColumnCount())) : 1;
                  const bool previous = KeySymbol::isLeft(event.sym) || KeySymbol::isUp(event.sym);
                  const auto next = std::clamp(
                      current + (previous ? -stride : stride), std::ptrdiff_t{0},
                      static_cast<std::ptrdiff_t>(entries.size() - 1)
                  );
                  m_grid->setSelectedIndex(static_cast<std::size_t>(next), true);
                  m_modalDialog.requestLayout();
                }
                return true;
              },
          .onDismiss =
              [this] {
                if (m_surface)
                  m_renderContext.makeCurrent(m_surface->renderTarget());
                m_quickLook.detach();
                m_quickLookOpen = false;
                m_quickLookContent = nullptr;
              },
          .fillContent = true,
          .usePreferredInitialHeight = true,
          .hoverTooltips = true,
          .headerCloseButton = true,
          .compositorBorder = true,
          .compactHeightMode = true,
          .preferredWidth = 780,
          .preferredHeight = 620,
          .minWidth = 360,
          .minHeight = 300,
          .sizeStateKey = "quick_look",
      }
  );
  if (!m_modalDialog.isOpen())
    m_quickLookOpen = false;
  return m_quickLookOpen;
}
