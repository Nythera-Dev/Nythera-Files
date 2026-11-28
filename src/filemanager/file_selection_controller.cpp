#include "filemanager/file_selection_controller.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "filemanager/browser_tab.h"
#include "filemanager/file_search_session.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"
#include "ui/tooltip/tooltip_manager.h"
#include "util/string_utils.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <utility>

namespace filemanager {

  FileSelectionController::FileSelectionController(TabManager& tabs, InputDispatcher& dispatcher, Callbacks callbacks)
      : m_tabs(tabs), m_dispatcher(dispatcher), m_callbacks(std::move(callbacks)) {}

  FileSelectionController::~FileSelectionController() { shutdown(); }

  void FileSelectionController::shutdown() { m_selectionTimer.stop(); }

  void FileSelectionController::clearTypeahead() { m_typeaheadQuery.clear(); }

  bool FileSelectionController::isTypeaheadActive(std::chrono::milliseconds timeout) const {
    return !m_typeaheadQuery.empty() && (std::chrono::steady_clock::now() - m_typeaheadLastTime <= timeout);
  }

  void FileSelectionController::resetForTabSwitch() {
    m_typeaheadQuery.clear();
    m_lastPointerIndex.reset();
    m_rubberBaseline.clear();
    m_rubberSelectedPath.reset();
    m_rubberAnchor.reset();
  }

  void FileSelectionController::cancelRubberBand() {
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
      grid->endRubberBand(true);
  }

  void FileSelectionController::changeSelection(SelectionChange change) {
    if (m_callbacks.canChangeSelection && !m_callbacks.canChangeSelection())
      return;
    auto& tab = m_tabs.activeTab();
    std::unordered_set<std::string> paths;
    std::optional<std::filesystem::path> first;
    if (change != SelectionChange::None) {
      paths.reserve(tab.session.entries().size());
      for (const auto& entry : tab.session.entries()) {
        const auto path = entry.absPath.string();
        if (change == SelectionChange::Invert && tab.isPathSelected(path))
          continue;
        paths.insert(path);
        if (!first)
          first = entry.absPath;
      }
    }
    auto primary = tab.selectedPath();
    if (!primary || !paths.contains(primary->string()))
      primary = std::move(first);
    tab.setSelectionWithAnchor(std::move(paths), std::move(primary));
    tab.session.clearChooseFirstAfterLoad();
    restoreSelection();
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
      grid->notifyDataChanged();
    if (m_callbacks.onUpdateSelectionSummary)
      m_callbacks.onUpdateSelectionSummary();
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestRedraw();
  }

  void FileSelectionController::restoreSelection(bool chooseFirst) {
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (grid == nullptr)
      return;
    auto& tab = m_tabs.activeTab();
    auto& session = tab.session;
    const auto setGridSelection = [this, grid, &tab](std::optional<std::size_t> index) {
      m_restoringSelection = true;
      // Metadata/monitor refreshes before the next layout must not cancel the
      // paste's pending reveal in VirtualGridView.
      const bool revealLink = index && tab.linkTargetToReveal && tab.selectedPath() == tab.linkTargetToReveal;
      if (revealLink || !tab.session.loading())
        tab.linkTargetToReveal.reset();
      const bool reveal = revealLink
          || (index
              && tab.pendingPasteRevealPath
              && tab.selectedPath() == tab.pendingPasteRevealPath
              && !tab.pendingDropScrollGeneration);
      grid->setSelectedIndex(index, reveal);
      m_restoringSelection = false;
    };
    if (tab.selectedPath().has_value()) {
      const auto index = grid->selectedIndex();
      if (index && *index < session.entries().size() && session.entries()[*index].absPath == *tab.selectedPath()) {
        setGridSelection(index);
        return;
      }
      const auto selected = std::ranges::find(session.entries(), *tab.selectedPath(), &FileEntry::absPath);
      if (selected != session.entries().end()) {
        setGridSelection(static_cast<std::size_t>(selected - session.entries().begin()));
        return;
      }
      if (session.search().status() == FileSearchSession::Status::Searching) {
        setGridSelection(std::nullopt);
        return;
      }
      if (session.loading())
        return;
      tab.resetSelectedPath();
    }
    if (chooseFirst && !session.entries().empty()) {
      setGridSelection(0);
      return;
    }
    setGridSelection(std::nullopt);
  }

  void FileSelectionController::handleGridSelectionChanged(std::optional<std::size_t> index) {
    if (m_restoringSelection)
      return;
    m_typeaheadQuery.clear();
    m_focusVisible = false;
    auto& tab = m_tabs.activeTab();
    if (m_callbacks.onAdminActivity)
      m_callbacks.onAdminActivity(tab);
    const auto& entries = tab.session.entries();
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (index.has_value() && *index < entries.size()) {
      const auto previousPath = tab.selectedPath();
      const auto& targetPath = entries[*index].absPath;
      const auto mods = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
      const bool ctrl = (mods & KeyMod::Ctrl) != 0U;
      const bool shift = (mods & KeyMod::Shift) != 0U;
      if (ctrl && shift) {
        tab.selectRange(*index, true, previousPath ? previousPath : targetPath);
      } else if (ctrl) {
        tab.toggleSelection(targetPath);
      } else if (shift) {
        tab.selectRange(*index, false, previousPath ? previousPath : targetPath);
      } else {
        tab.selectSingle(targetPath);
      }
      if (grid)
        grid->notifyDataChanged();
    } else {
      tab.clearSelection();
    }
    if (m_callbacks.onUpdateSelectionSummary)
      m_callbacks.onUpdateSelectionSummary();
  }

  void FileSelectionController::handleRubberBandSelection(
      const std::vector<std::size_t>& hits, bool finished, bool cancelled
  ) {
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (!grid)
      return;
    auto& tab = m_tabs.activeTab();
    const auto& entries = tab.session.entries();
    if (cancelled) {
      tab.setSelection(m_rubberBaseline, m_rubberSelectedPath, m_rubberAnchor);
    } else {
      const bool ctrl = (m_rubberModifiers & KeyMod::Ctrl) != 0U;
      const bool shift = (m_rubberModifiers & KeyMod::Shift) != 0U;
      std::unordered_set<std::string> paths = (ctrl || shift) ? m_rubberBaseline : std::unordered_set<std::string>{};
      for (const auto index : hits) {
        if (index >= entries.size())
          continue;
        const auto path = entries[index].absPath.string();
        if (ctrl && m_rubberBaseline.contains(path))
          paths.erase(path);
        else
          paths.insert(path);
      }
      if (shift && !hits.empty() && m_rubberAnchor) {
        const auto anchor = std::ranges::find(entries, *m_rubberAnchor, &FileEntry::absPath);
        if (anchor != entries.end()) {
          const auto index = static_cast<std::size_t>(anchor - entries.begin());
          const auto endpoint = hits.back();
          for (auto i = std::min(index, endpoint); i <= std::max(index, endpoint) && i < entries.size(); ++i)
            paths.insert(entries[i].absPath.string());
        }
      }
      std::optional<std::filesystem::path> primary = tab.selectedPath();
      if (!hits.empty() && hits.back() < entries.size())
        primary = entries[hits.back()].absPath;
      else if (paths.empty())
        primary.reset();
      const auto anchor = (finished && !ctrl && !shift) ? primary : tab.selectionAnchor();
      tab.setSelection(std::move(paths), std::move(primary), std::move(anchor));
    }
    m_restoringSelection = true;
    const auto selected =
        tab.selectedPath() ? std::ranges::find(entries, *tab.selectedPath(), &FileEntry::absPath) : entries.end();
    grid->setSelectedIndex(
        selected == entries.end() ? std::nullopt : std::optional<std::size_t>(selected - entries.begin()), false
    );
    m_restoringSelection = false;
    grid->notifyDataChanged();
    if (m_callbacks.onUpdateSelectionSummary)
      m_callbacks.onUpdateSelectionSummary();
    if (finished) {
      m_rubberBaseline.clear();
      m_selectionTimer.stop();
    }
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestRedraw();
  }

  bool FileSelectionController::tryBeginRubberBand(float x, float y) {
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (!grid || (m_callbacks.isRenaming && m_callbacks.isRenaming()))
      return false;
    const auto& tab = m_tabs.activeTab();
    m_rubberBaseline = tab.selectedPaths();
    m_rubberSelectedPath = tab.selectedPath();
    m_rubberAnchor = tab.selectionAnchor();
    m_rubberModifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    if (!grid->beginRubberBand(x, y))
      return false;
    m_dispatcher.pointerLeave();
    TooltipManager::instance().forceDestroy();
    m_selectionTick = std::chrono::steady_clock::now();
    m_selectionTimer.startRepeating(std::chrono::milliseconds(16), [this] {
      const auto now = std::chrono::steady_clock::now();
      const float seconds = std::chrono::duration<float>(now - m_selectionTick).count();
      m_selectionTick = now;
      if (auto* gridPtr = m_callbacks.grid ? m_callbacks.grid() : nullptr)
        gridPtr->advanceRubberBandScroll(std::min(seconds, 0.05F));
      if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
        surface->requestLayout();
    });
    m_lastPointerIndex.reset();
    m_dispatcher.cancelPointerCapture();
    m_dispatcher.setFocus(grid->focusArea());
    return true;
  }

  bool FileSelectionController::handleActiveRubberBandPointer(const PointerEvent& event, float x, float y) {
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (!grid || !grid->rubberBandActive())
      return false;
    if (event.type == PointerEvent::Type::Motion)
      grid->updateRubberBand(x, y);
    else if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed)
      grid->endRubberBand();
    else if (event.type == PointerEvent::Type::Axis)
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
    return true;
  }

  bool FileSelectionController::handleDragPressSelection(float x, float y, std::uint32_t serial) {
    auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    const bool deviceBusy = m_callbacks.hasActiveDeviceOperations && m_callbacks.hasActiveDeviceOperations();
    if (!tab.capabilities().canDragSource || !grid || renaming || deviceBusy)
      return false;
    const auto index = grid->indexAtSurface(x, y);
    if (!index)
      return false;
    if (m_callbacks.onBeginFileDragPress)
      m_callbacks.onBeginFileDragPress(*index, x, y, serial);
    const auto mods = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    const bool preserve =
        tab.isPathSelected(session.entries()[*index].absPath.string()) && !(mods & (KeyMod::Ctrl | KeyMod::Shift));
    m_restoringSelection = true;
    if (!preserve)
      grid->setSelectedIndex(std::nullopt, false);
    m_restoringSelection = preserve;
    grid->setSelectedIndex(index, false);
    m_restoringSelection = false;
    m_dispatcher.setFocus(grid->focusArea());
    return true;
  }

  void FileSelectionController::handleUnmovedDragClick(std::size_t index, std::uint32_t clickTime) {
    const auto mods = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    if (!(mods & (KeyMod::Ctrl | KeyMod::Shift))) {
      auto& tab = m_tabs.activeTab();
      tab.selectSingle(tab.session.entries()[index].absPath, false);
      if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
        grid->notifyDataChanged();
      m_pointerClickTime = clickTime;
      activatePointerItem(index);
    } else {
      m_lastPointerIndex.reset();
    }
  }

  void FileSelectionController::activatePointerItem(std::size_t index) {
    const auto& entries = m_tabs.activeTab().session.entries();
    if (index >= entries.size())
      return;
    const auto mods = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    const bool deviceBusy = m_callbacks.hasActiveDeviceOperations && m_callbacks.hasActiveDeviceOperations();
    if ((mods & (KeyMod::Ctrl | KeyMod::Shift)) || renaming || deviceBusy) {
      m_lastPointerIndex.reset();
      return;
    }
    if (m_callbacks.isSingleClickActivation && m_callbacks.isSingleClickActivation()) {
      m_lastPointerIndex.reset();
      if (m_callbacks.onActivatePath)
        m_callbacks.onActivatePath(entries[index].absPath);
      return;
    }
    const std::uint32_t elapsed = m_pointerClickTime - m_lastPointerClickTime;
    if (m_lastPointerIndex == index && elapsed <= 500U) {
      m_lastPointerIndex.reset();
      if (m_callbacks.onActivatePath)
        m_callbacks.onActivatePath(entries[index].absPath);
      return;
    }
    m_lastPointerIndex = index;
    m_lastPointerClickTime = m_pointerClickTime;
  }

  void FileSelectionController::selectSingleForContextMenu(std::size_t index) {
    auto& tab = m_tabs.activeTab();
    tab.selectSingle(tab.session.entries()[index].absPath);
    m_restoringSelection = true;
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr) {
      grid->setSelectedIndex(index, false);
      grid->notifyDataChanged();
    }
    m_restoringSelection = false;
    if (m_callbacks.onUpdateSelectionSummary)
      m_callbacks.onUpdateSelectionSummary();
  }

  void FileSelectionController::clearSelectionForEmptyMenu() {
    m_tabs.activeTab().clearSelection();
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
      grid->setSelectedIndex(std::nullopt, false);
  }

  void FileSelectionController::applyPasteSelection(
      TabId tabId, std::unordered_set<std::string> paths, const std::filesystem::path& primary
  ) {
    if (auto* tab = m_tabs.findTab(tabId)) {
      tab->setSelectionWithAnchor(std::move(paths), primary);
      if (tabId == m_tabs.activeTabId())
        restoreSelection();
    }
  }

  bool FileSelectionController::navigateByKeyboard(std::uint32_t sym, std::uint32_t mods) {
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    auto& tab = m_tabs.activeTab();
    const auto& entries = tab.session.entries();
    if (grid == nullptr || entries.empty())
      return false;
    const std::size_t cols = std::max<std::size_t>(1, grid->layoutColumnCount());
    if (cols == 1 && (KeySymbol::isLeft(sym) || KeySymbol::isRight(sym)))
      return false;

    const std::size_t pageStride = std::max<std::size_t>(cols, grid->pageItemStride());
    const auto current = grid->selectedIndex();
    std::size_t target = 0;

    if (!current.has_value()) {
      if (KeySymbol::isDown(sym) || KeySymbol::isRight(sym) || KeySymbol::isHome(sym) || KeySymbol::isPageDown(sym))
        target = 0;
      else
        target = entries.size() - 1;
    } else {
      const std::size_t idx = std::min(*current, entries.size() - 1);
      if (KeySymbol::isHome(sym)) {
        target = 0;
      } else if (KeySymbol::isEnd(sym)) {
        target = entries.size() - 1;
      } else if (KeySymbol::isLeft(sym) || KeySymbol::isRight(sym)) {
        const bool backward = Style::rtl() ? KeySymbol::isRight(sym) : KeySymbol::isLeft(sym);
        if (backward)
          target = idx > 0 ? idx - 1 : 0;
        else
          target = idx + 1 < entries.size() ? idx + 1 : idx;
      } else if (KeySymbol::isUp(sym)) {
        target = idx >= cols ? idx - cols : idx;
      } else if (KeySymbol::isDown(sym)) {
        if (idx + cols < entries.size()) {
          target = idx + cols;
        } else {
          const std::size_t rowStart = (idx / cols) * cols;
          target = (rowStart + cols < entries.size()) ? (entries.size() - 1) : idx;
        }
      } else if (KeySymbol::isPageUp(sym)) {
        target = idx > pageStride ? idx - pageStride : 0;
      } else if (KeySymbol::isPageDown(sym)) {
        target = std::min(entries.size() - 1, idx + pageStride);
      }
    }

    m_typeaheadQuery.clear();
    m_focusVisible = true;
    m_dispatcher.setFocus(grid->focusArea());
    if (m_callbacks.onAdminActivity)
      m_callbacks.onAdminActivity(tab);
    const bool ctrl = (mods & KeyMod::Ctrl) != 0U;
    const bool shift = (mods & KeyMod::Shift) != 0U;
    if (shift) {
      const auto fallback =
          (current.has_value() && *current < entries.size()) ? entries[*current].absPath : entries[target].absPath;
      tab.selectRange(target, ctrl, fallback);
    } else if (ctrl) {
      // Ctrl+Arrow moves focus cursor independently while keeping existing selection intact.
      tab.moveSelectionFocus(entries[target].absPath);
    } else {
      tab.selectSingle(entries[target].absPath);
    }

    m_restoringSelection = true;
    grid->setSelectedIndex(target, true);
    m_restoringSelection = false;
    grid->notifyDataChanged();
    if (m_callbacks.onUpdateSelectionSummary)
      m_callbacks.onUpdateSelectionSummary();
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestRedraw();
    return true;
  }

  bool FileSelectionController::handleSpaceSelection(std::uint32_t mods) {
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    auto& tab = m_tabs.activeTab();
    const auto& entries = tab.session.entries();
    if (grid == nullptr || entries.empty())
      return false;
    const std::size_t idx = std::min(grid->selectedIndex().value_or(0), entries.size() - 1);
    m_focusVisible = true;
    m_dispatcher.setFocus(grid->focusArea());
    if (m_callbacks.onAdminActivity)
      m_callbacks.onAdminActivity(tab);
    const bool ctrl = (mods & KeyMod::Ctrl) != 0U;
    const bool shift = (mods & KeyMod::Shift) != 0U;

    if (ctrl && !shift)
      tab.toggleSelection(entries[idx].absPath);
    else if (shift)
      tab.selectRange(idx, ctrl);
    else
      tab.selectSingle(entries[idx].absPath);

    m_restoringSelection = true;
    grid->setSelectedIndex(idx, true);
    m_restoringSelection = false;
    grid->notifyDataChanged();
    if (m_callbacks.onUpdateSelectionSummary)
      m_callbacks.onUpdateSelectionSummary();
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestRedraw();
    return true;
  }

  bool FileSelectionController::handleTypeahead(std::uint32_t utf32) {
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    auto& tab = m_tabs.activeTab();
    const auto& entries = tab.session.entries();
    if (grid == nullptr || entries.empty())
      return false;
    const std::string chRaw = StringUtils::utf32ToUtf8(utf32);
    if (chRaw.empty())
      return false;
    g_autofree gchar* chFoldedRaw = g_utf8_casefold(chRaw.c_str(), static_cast<gssize>(chRaw.size()));
    const std::string ch = chFoldedRaw ? chFoldedRaw : StringUtils::toLower(chRaw);
    if (ch.empty())
      return false;

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kTypeaheadTimeout = std::chrono::milliseconds(800);
    bool cycleSameLetter = false;
    if (now - m_typeaheadLastTime > kTypeaheadTimeout) {
      m_typeaheadQuery = ch;
    } else if (m_typeaheadQuery == ch) {
      cycleSameLetter = true;
    } else {
      m_typeaheadQuery += ch;
    }
    m_typeaheadLastTime = now;

    const auto findMatch = [&entries](std::string_view prefix, std::size_t startIndex) -> std::optional<std::size_t> {
      const std::size_t count = entries.size();
      for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t idx = (startIndex + offset) % count;
        g_autofree gchar* foldedNameRaw =
            g_utf8_casefold(entries[idx].name.c_str(), static_cast<gssize>(entries[idx].name.size()));
        const std::string_view foldedName =
            foldedNameRaw ? std::string_view(foldedNameRaw) : std::string_view(entries[idx].name);
        if (foldedName.starts_with(prefix))
          return idx;
      }
      return std::nullopt;
    };

    const auto current = grid->selectedIndex();
    const std::size_t startIdx =
        current.has_value() ? (cycleSameLetter ? (*current + 1) % entries.size() : *current) : 0;
    std::optional<std::size_t> match = findMatch(m_typeaheadQuery, startIdx);
    if (!match.has_value() && m_typeaheadQuery.size() > ch.size()) {
      m_typeaheadQuery = ch;
      match = findMatch(m_typeaheadQuery, current.has_value() ? (*current + 1) % entries.size() : 0);
    }
    if (!match.has_value())
      return false;

    m_focusVisible = true;
    m_dispatcher.setFocus(grid->focusArea());
    if (m_callbacks.onAdminActivity)
      m_callbacks.onAdminActivity(tab);
    tab.selectSingle(entries[*match].absPath);
    m_restoringSelection = true;
    grid->setSelectedIndex(*match, true);
    m_restoringSelection = false;
    grid->notifyDataChanged();
    if (m_callbacks.onUpdateSelectionSummary)
      m_callbacks.onUpdateSelectionSummary();
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestRedraw();
    return true;
  }

} // namespace filemanager
