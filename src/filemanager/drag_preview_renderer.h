#pragma once

#include "render/render_context.h"
#include "render/render_target.h"
#include "wayland/toplevel_surface.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class FileIcon;
class FileManagerTestAccess;
class Flex;
class Label;
class Renderer;
struct wl_surface;

namespace filemanager {

  struct DragPreviewState {
    std::filesystem::path primaryPath;
    bool primaryIsDir = false;
    bool allowFilesystemQueries = true;
    std::size_t itemCount = 0;
    bool targetValid = false;
    std::string actionLabel;
    float pointerX = 0.0F;
    float pointerY = 0.0F;
  };

  class DragPreviewRenderer {
  public:
    DragPreviewRenderer() = default;
    ~DragPreviewRenderer();

    DragPreviewRenderer(const DragPreviewRenderer&) = delete;
    DragPreviewRenderer& operator=(const DragPreviewRenderer&) = delete;
    DragPreviewRenderer(DragPreviewRenderer&&) = delete;
    DragPreviewRenderer& operator=(DragPreviewRenderer&&) = delete;

    void shutdown();
    void detachUi();

    void clearNative();
    void setNativeSurface(wl_surface* surface) noexcept { m_nativePreviewSurface = surface; }
    [[nodiscard]] wl_surface* nativeSurface() const noexcept { return m_nativePreviewSurface; }

    void scheduleNative(
        RenderContext& renderContext, ToplevelSurface* mainSurface, std::function<bool()> isActive,
        std::function<void(Renderer&)> preparePreview, std::function<void(const std::string&)> onError
    );

    void updateOverlay(Renderer& renderer, Flex* root, ToplevelSurface* surface, const DragPreviewState& state);
    void hideOverlay();
    [[nodiscard]] bool hasPreview() const noexcept { return m_dragPreview != nullptr; }

  private:
    friend class ::FileManagerTestAccess;

    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::unique_ptr<RenderTarget> m_nativePreviewTarget;
    wl_surface* m_nativePreviewSurface = nullptr;
    bool m_nativePreviewQueued = false;
    std::uint64_t m_nativePreviewGeneration = 0;
    Flex* m_dragPreview = nullptr;
    FileIcon* m_dragPreviewIcon = nullptr;
    Label* m_dragPreviewLabel = nullptr;
    Label* m_dragPreviewAction = nullptr;
  };

} // namespace filemanager
