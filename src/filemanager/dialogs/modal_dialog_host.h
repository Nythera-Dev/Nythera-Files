#pragma once

#include "core/timer_manager.h"
#include "filemanager/file_operation_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Button;
class DialogBackgroundArtwork;
class FileManagerTestAccess;
class Flex;
class Image;
class Input;
class InputArea;
class Label;
class Node;
class RenderContext;
class Renderer;
class SelectDropdownPopup;
class WaylandConnection;

namespace filemanager {

  class FileManagerSettings;
  class ConflictDialogController;
  class SymlinkDialogController;

  enum class DialogChoiceStyle : std::uint8_t { Normal, Cancel, Destructive };
  struct DialogChoice {
    std::string label;
    int response;
    DialogChoiceStyle style = DialogChoiceStyle::Normal;
  };
  enum class DialogStyle : std::uint8_t { Standard, Warning, Conflict, TextPrompt };

  enum class ConflictDialogKind : std::uint8_t {
    File,
    Directory,
    RestoreDirectory,
    TypeMismatch,
    SelfConflict,
  };

  enum class ConflictPathAccess { Local, ExecutorOnly };

  struct ConflictItemInfo {
    std::string name;
    std::string metadata;
    std::filesystem::path path;
    bool isDirectory = false;
  };

  struct ConflictDialogData {
    ConflictDialogKind kind = ConflictDialogKind::File;
    ConflictItemInfo incoming;
    ConflictItemInfo existing;
    std::string comparisonSummary;
    ConflictPathAccess pathAccess = ConflictPathAccess::Local;
  };

  class ModalDialogHost {
  public:
    struct CustomContent {
      std::function<void(Flex& root, float dialogWidth)> buildFields;
      std::function<void(int result, Button* button)> bindChoiceButton;
      std::function<void()> afterBuild;
      std::function<InputArea*()> initialFocus;
      std::function<void()> onThemeUpdated;
      std::function<void(Renderer&)> onPrepareLayout;
      // Return false while the initial content is still loading.
      std::function<bool(Renderer&, float normalHeight, std::optional<float>& compactHeight)> onInitialLayout;
      std::function<bool(const KeyboardEvent&)> onKeyEvent;
      std::function<bool(int response)> interceptResponse;
      std::function<void()> onBeforeClose;
      std::function<void()> onDismiss;
      bool useMeasuredMinHeight = false;
      bool fillContent = false;
      bool usePreferredInitialHeight = false;
      bool hoverTooltips = false;
      bool headerCloseButton = false;
      bool compositorBorder = false;
      bool compactHeightMode = false;
      bool selectDropdowns = false;
      bool rememberSize = true;
      std::uint32_t preferredWidth = 0;
      std::uint32_t preferredHeight = 0;
      std::uint32_t minWidth = 0;
      std::uint32_t minHeight = 0;
      std::string sizeStateKey;
    };

    struct Callbacks {
      std::function<ToplevelSurface*()> defaultParent;
      std::function<bool()> isMainWindowOpen;
      std::function<ToplevelSurface*()> mainSurface;
      std::function<ToplevelSurface*()> progressSurface;
      std::function<void()> syncProgressWindow;
      std::function<void(std::string)> setStatus;
    };

    ModalDialogHost(
        WaylandConnection& wayland, RenderContext& renderContext, FileManagerSettings& settings, Callbacks callbacks
    );
    ~ModalDialogHost();

    void showDialog(
        std::string title, std::string message, std::vector<DialogChoice> choices, std::function<void(int)> response,
        ToplevelSurface* parent = nullptr, std::optional<CustomContent> customContent = std::nullopt,
        DialogStyle style = DialogStyle::Standard
    );
    void textPrompt(
        std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
    );
    enum class SymlinkField : std::uint8_t { None, Name, Target };
    struct SymlinkError {
      std::string message;
      SymlinkField field = SymlinkField::None;
    };
    void showSymlinkDialog(
        std::function<std::optional<SymlinkError>(const std::string& name, const std::string& target)> onSubmit,
        std::function<void(const std::string& currentTarget)> onBrowseTarget = {}, std::function<void()> onDismiss = {}
    );
    void setSymlinkTargetPath(std::string_view path);
    void showSymlinkTargetError(std::string_view message);
    void closeDialog(int response);
    void forceCloseImmediate();
    void updateTheme();
    void handlePointer(const PointerEvent& event);
    void handleKeyboard(const KeyboardEvent& event);
    void requestLayout();
    void setTitle(std::string title);
    [[nodiscard]] std::uint32_t normalHeight() const noexcept;
    void setCompactHeight(std::optional<float> height);
    void setFocus(InputArea* area);
    [[nodiscard]] InputArea* focusedArea() const noexcept { return m_dispatcher.focusedArea(); }
    void registerCard(Flex* card);

    [[nodiscard]] FileOperationManager::ConflictResolver
    makeConflictResolver(ConflictPathAccess pathAccess = ConflictPathAccess::Local);
    [[nodiscard]] std::function<std::optional<std::string>(const std::string&, bool, GCancellable*)>
    makeArchivePasswordResolver();
    [[nodiscard]] static std::string sanitizeConflictDisplayText(std::string_view text);

    [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr || m_pendingPresentation.has_value(); }
    [[nodiscard]] bool hoverTooltipsEnabled() const noexcept {
      return m_surface && !m_closeQueued && m_customContent && m_customContent->hoverTooltips;
    }
    [[nodiscard]] bool pointerCaptured() const noexcept { return m_dispatcher.pointerCaptured(); }
    [[nodiscard]] bool closeQueued() const noexcept { return m_closeQueued; }
    [[nodiscard]] bool backgroundConflictPending() const noexcept;
    [[nodiscard]] ToplevelSurface* surface() const noexcept { return m_surface.get(); }
    [[nodiscard]] wl_surface* wlSurface() const noexcept { return m_surface ? m_surface->wlSurface() : nullptr; }
    [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;

  private:
    friend class ::FileManagerTestAccess;

    void recordConfiguredSize(std::uint32_t width, std::uint32_t height);
    void savePendingDialogSize();
    [[nodiscard]] std::optional<std::uint32_t> compactExtent(std::optional<float> height) const;
    void prepareInitialLayout();
    void cancelInitialLayout();
    void presentDialog(ToplevelSurface* parent, const ToplevelSurfaceConfig& config);
    using DialogSize = std::pair<std::uint32_t, std::uint32_t>;
    static std::uint32_t
    initialExtent(std::uint32_t saved, std::uint32_t fallback, std::uint32_t minimum, std::uint32_t available);
    void buildDialog(float dialogWidth);
    void wrapDialogBodyForScrolling();
    void prepareFrame(bool needsUpdate, bool needsLayout);

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    FileManagerSettings& m_settings;
    Callbacks m_callbacks;

    std::unique_ptr<ToplevelSurface> m_surface;
    struct PendingPresentation {
      ToplevelSurface* parent;
      ToplevelSurfaceConfig config;
    };
    std::optional<PendingPresentation> m_pendingPresentation;
    unsigned int m_initialLayoutSource = 0;
    std::optional<ToplevelSurface::ModalManager> m_modalManager;
    ToplevelSurface::ModalHint m_modalHint;
    InputDispatcher m_dispatcher;
    std::unique_ptr<Flex> m_root;
    std::unique_ptr<SelectDropdownPopup> m_selectDropdown;
    std::vector<Flex*> m_cards;
    std::function<void(int)> m_response;
    std::string m_title;
    std::string m_message;
    DialogStyle m_style = DialogStyle::Standard;
    bool m_resizable = false;
    std::string m_sizeStateKey;
    std::uint32_t m_preferredWidth = 0;
    std::uint32_t m_preferredHeight = 0;
    std::optional<DialogSize> m_configuredDialogSize;
    std::optional<DialogSize> m_pendingDialogSize;
    std::optional<DialogSize> m_normalDialogSize;
    std::optional<std::uint32_t> m_compactDialogHeight;
    std::uint32_t m_dialogMinHeight = 0;
    bool m_applyingDialogSize = false;
    AnimationManager m_dialogHeightAnimation;
    Timer m_dialogSizeSave;
    std::array<InputArea*, 8> m_resizeEdges{};
    InputArea* m_moveHandle = nullptr;
    Flex* m_titleRow = nullptr;
    Label* m_titleLabel = nullptr;
    Flex* m_spacer = nullptr;
    Flex* m_footer = nullptr;
    Node* m_divider = nullptr;
    Image* m_deleteWarning = nullptr;
    DialogBackgroundArtwork* m_topAccent = nullptr;
    DialogBackgroundArtwork* m_cornerAccent = nullptr;
    // Allows headless tests to exercise dialog contents, parenting and responses.
    std::function<void(ToplevelSurface&, ToplevelSurface*, DialogSize)> m_presentToplevelForTest;
    Renderer* m_initialLayoutRendererForTest = nullptr;
    std::vector<DialogChoice> m_choices;
    bool m_closeQueued = false;
    guint m_closeSource = 0;
    std::string m_promptValue;
    Input* m_promptInput = nullptr;
    bool m_passwordPrompt = false;
    std::function<void()> m_cancelArchivePassword;
    bool m_backgroundArchivePasswordPending = false;
    std::unique_ptr<ConflictDialogController> m_conflictController;
    std::unique_ptr<SymlinkDialogController> m_symlinkController;
    std::optional<CustomContent> m_customContent;
    int m_resultPending = -1;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
