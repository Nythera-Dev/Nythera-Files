#pragma once

#include "core/timer_manager.h"
#include "filemanager/tab_manager.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/toplevel_surface.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class FileManagerTestAccess;
struct PointerEvent;
class VirtualGridView;

namespace filemanager {

  enum class SelectionChange { All, None, Invert };

  class FileSelectionController {
  public:
    struct Callbacks {
      std::function<VirtualGridView*()> grid;
      std::function<ToplevelSurface*()> mainSurface;
      std::function<bool()> canChangeSelection;
      std::function<std::uint32_t()> keyboardModifiers;
      std::function<bool()> isRenaming;
      std::function<bool()> hasActiveDeviceOperations;
      std::function<bool()> isSingleClickActivation;
      std::function<void()> onUpdateSelectionSummary;
      std::function<void(BrowserTab&)> onAdminActivity;
      std::function<void(const std::filesystem::path&)> onActivatePath;
      std::function<void(std::size_t, float, float, std::uint32_t)> onBeginFileDragPress;
    };

    FileSelectionController(TabManager& tabs, InputDispatcher& dispatcher, Callbacks callbacks);
    ~FileSelectionController();

    FileSelectionController(const FileSelectionController&) = delete;
    FileSelectionController& operator=(const FileSelectionController&) = delete;
    FileSelectionController(FileSelectionController&&) = delete;
    FileSelectionController& operator=(FileSelectionController&&) = delete;

    void shutdown();

    void changeSelection(SelectionChange change);
    void restoreSelection(bool chooseFirst = false);

    void handleGridSelectionChanged(std::optional<std::size_t> index);
    void handleRubberBandSelection(const std::vector<std::size_t>& hits, bool finished, bool cancelled);
    bool tryBeginRubberBand(float x, float y);
    bool handleActiveRubberBandPointer(const PointerEvent& event, float x, float y);
    void cancelRubberBand();
    void resetForTabSwitch();

    bool handleDragPressSelection(float x, float y, std::uint32_t serial);
    void handleUnmovedDragClick(std::size_t index, std::uint32_t clickTime);
    void activatePointerItem(std::size_t index);
    void selectSingleForContextMenu(std::size_t index);
    void clearSelectionForEmptyMenu();
    void applyPasteSelection(TabId tabId, std::unordered_set<std::string> paths, const std::filesystem::path& primary);

    bool navigateByKeyboard(std::uint32_t sym, std::uint32_t mods);
    bool handleSpaceSelection(std::uint32_t mods);
    bool handleTypeahead(std::uint32_t utf32);
    [[nodiscard]] bool isTypeaheadActive(std::chrono::milliseconds timeout) const;
    void clearTypeahead();

    void setFocusVisible(bool visible) noexcept { m_focusVisible = visible; }
    [[nodiscard]] bool focusVisible() const noexcept { return m_focusVisible; }
    void recordPointerClickTime(std::uint32_t clickTime) noexcept { m_pointerClickTime = clickTime; }
    void clearLastPointerIndex() noexcept { m_lastPointerIndex.reset(); }

  private:
    friend class ::FileManagerTestAccess;

    TabManager& m_tabs;
    InputDispatcher& m_dispatcher;
    Callbacks m_callbacks;

    std::unordered_set<std::string> m_rubberBaseline;
    std::optional<std::filesystem::path> m_rubberSelectedPath;
    std::optional<std::filesystem::path> m_rubberAnchor;
    std::uint32_t m_rubberModifiers = 0;
    Timer m_selectionTimer;
    std::chrono::steady_clock::time_point m_selectionTick;
    bool m_restoringSelection = false;
    bool m_focusVisible = false;
    std::string m_typeaheadQuery;
    std::chrono::steady_clock::time_point m_typeaheadLastTime{};
    std::optional<std::size_t> m_lastPointerIndex;
    std::uint32_t m_lastPointerClickTime = 0;
    std::uint32_t m_pointerClickTime = 0;
  };

} // namespace filemanager
