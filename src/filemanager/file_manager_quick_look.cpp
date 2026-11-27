#include "core/input/key_symbols.h"
#include "filemanager/file_manager.h"
#include "render/core/renderer.h"
#include "ui/controls/flex.h"
#include "ui/controls/virtual_grid_view.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>

bool FileManager::openQuickLook() {
  if (!m_surface || !m_grid || m_modalDialog.isOpen() || !selectedPreviewEntry())
    return false;
  using filemanager::ModalDialogHost;
  m_quickLookOpen = true;
  m_modalDialog.showDialog(
      "Quick Look", "", {{"Close", 0}}, [](int) {}, m_surface.get(), false, false,
      ModalDialogHost::CustomContent{
          .buildFields =
              [this](Flex& root, float) {
                auto content = m_quickLook.build();
                content->setFlexGrow(1.0F);
                m_quickLookContent = static_cast<Flex*>(root.addChild(std::move(content)));
              },
          .onPrepareLayout =
              [this](Renderer& renderer) {
                const float width = m_quickLookContent->width();
                const float height = std::max(96.0F, m_quickLookContent->height() - 95.0F);
                const int pixels = std::clamp(
                    static_cast<int>(std::ceil(std::max(width, height) * renderer.renderScale() / 128.0F)) * 128, 256,
                    2048
                );
                syncFilePreview(m_quickLook, filemanager::PreviewMode::Full, pixels);
                (void)m_pickerThumbnails.uploadPending(renderer.textureManager());
                m_quickLook.render(renderer, width, height);
                m_quickLookContent->layout(renderer);
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
          .preferredWidth = 780,
          .preferredHeight = 620,
          .minWidth = 360,
          .minHeight = 300,
      }
  );
  if (!m_modalDialog.isOpen())
    m_quickLookOpen = false;
  return m_quickLookOpen;
}
