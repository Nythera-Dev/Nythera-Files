#include "filemanager/file_transfer_controller.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_transfer_payload.h"
#include "render/core/renderer.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/tooltip/tooltip_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>
#include <ranges>
#include <utility>
#include <wayland-client-protocol.h>

namespace filemanager {

  namespace {

    std::filesystem::path normalizeDirectoryPath(const std::filesystem::path& path) {
      if (path.empty())
        return {};
      auto normalized = path.lexically_normal();
      if (normalized.has_filename() && normalized.filename() == ".")
        normalized = normalized.parent_path();
      const auto text = normalized.string();
      if (text.size() > 1 && text.ends_with('/'))
        return std::filesystem::path(text.substr(0, text.size() - 1));
      return normalized;
    }

    // Drag-and-drop edge autoscroll adapted from Luciole (src/app/page/drag_drop_controller.cpp).
    // MIT License — Copyright (c) 2026 noctalia-dev
    // Permission is hereby granted, free of charge, to any person obtaining a copy of this software
    // and associated documentation files (the "Software"), to deal in the Software without restriction,
    // including without limitation the rights to use, copy, modify, merge, publish, distribute,
    // sublicense, and/or sell copies of the Software, subject to the MIT License conditions.
    constexpr float kDragScrollEdgeZonePx = 32.0F;
    constexpr float kDragScrollStepPx = 6.0F;
    constexpr float kDragScrollMaxDeltaPx = 18.0F;
    constexpr auto kDragScrollInterval = std::chrono::milliseconds(16);

    bool isNodeEffectivelyVisible(const Node* node) {
      for (const Node* current = node; current != nullptr; current = current->parent()) {
        if (!current->visible())
          return false;
      }
      return node != nullptr;
    }

    float edgeScrollSpeed(const ScrollView& scroll, float pointerY, float scrollY) {
      if (scroll.height() <= 0.0F || scroll.maxScrollOffset() <= 0.0F)
        return 0.0F;
      const float zone = std::min(kDragScrollEdgeZonePx, scroll.height() * 0.5F);
      if (pointerY < scrollY + zone && scroll.scrollOffset() > 0.0F)
        return -kDragScrollStepPx;
      if (pointerY > scrollY + scroll.height() - zone && scroll.scrollOffset() < scroll.maxScrollOffset())
        return kDragScrollStepPx;
      return 0.0F;
    }

  } // namespace

  FileTransferController::FileTransferController(
      WaylandConnection& wayland, RenderContext& renderContext, InputDispatcher& dispatcher,
      NativeDataTransfer& transfer, FileOperationManager& operations, TabManager& tabs, PlacesSidebarController& places,
      PathResolutionCache& pathCache, FileClipboardController& clipboard, Callbacks callbacks
  )
      : m_wayland(wayland), m_renderContext(renderContext), m_dispatcher(dispatcher), m_transfer(transfer),
        m_operations(operations), m_tabs(tabs), m_places(places), m_pathCache(pathCache), m_clipboard(clipboard),
        m_callbacks(std::move(callbacks)) {}

  FileTransferController::~FileTransferController() { shutdown(); }

  void FileTransferController::setupDragHandlers() {
    m_transfer.setSelectionChangedCallback([this] { m_clipboard.syncCutStateFromClipboard(); });
    m_transfer.setDragHandlers(
        [this](wl_surface* surface, float x, float y, std::uint32_t actions) {
          return updateProtocolDrag(surface, x, y, actions);
        },
        [this] {
          stopHoverDropTab();
          m_protocolDragInside = false;
          m_protocolDropDestination.reset();
          m_protocolDropSources.clear();
          clearDragFeedback();
          m_pinBefore.reset();
          m_places.showPlaceInsertion(std::nullopt);
          m_dragTargetValid = false;
          stopDragScroll();
          m_previewRenderer.hideOverlay();
          if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
            surface->requestUpdate();
          scheduleNativeDragPreview();
        },
        [this](std::uint32_t action) { return receiveProtocolDrop(action); },
        [this](bool success) {
          cancelFileGesture();
          if (m_callbacks.isWindowOpen && m_callbacks.isWindowOpen() && m_callbacks.onStatus)
            m_callbacks.onStatus(success ? "Drop completed" : "Drop cancelled");
        },
        [this] {
          if (m_transfer.dragDropped())
            clearNativeDragPreview();
          else
            scheduleNativeDragPreview();
        }
    );
  }

  void FileTransferController::shutdown() {
    if (m_alive)
      *m_alive = false;
    stopHoverDropTab();
    stopDragScroll();
    cancelDropValidation();
    m_transfer.cancelDrag();
    m_previewRenderer.shutdown();
  }

  void FileTransferController::detachUi() {
    stopHoverDropTab();
    stopDragScroll();
    m_previewRenderer.detachUi();
    m_dropPlace = nullptr;
  }

  void FileTransferController::onOperationFinished(const FileOperationManager::Operation& operation) {
    if (auto finish = m_dropFinishes.find(operation.id); finish != m_dropFinishes.end()) {
      auto callback = std::move(finish->second);
      m_dropFinishes.erase(finish);
      if (callback)
        callback(operation.succeeded);
    }
  }

  void FileTransferController::onBeforePathChange(BrowserTab& tab) {
    if (tab.id == m_tabs.activeTabId()) {
      stopDragScroll();
      if (m_draggingFiles || m_protocolDragInside) {
        clearDragFeedback();
        m_pinBefore.reset();
        m_places.showPlaceInsertion(std::nullopt);
        m_protocolDropDestination.reset();
        m_dragTargetValid = false;
      }
    }
  }

  std::optional<std::filesystem::path> FileTransferController::dropDestination(float x, float y, bool incoming) const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const bool archiveDrag = !incoming && m_archiveDrag.catalogue != nullptr;
    if (m_callbacks.fileDropTabAt) {
      const auto id = m_callbacks.fileDropTabAt(x, y);
      if (archiveDrag || id == tab.id) {
        if (const auto* target = m_tabs.findTab(id))
          return target->session.path();
      }
    }
    if (archiveDrag && session.browsingArchive())
      return m_places.dropDestinationAt(x, y);
    if (!grid
        || session.browsingTrash()
        || !tab.capabilities().canAcceptDrop
        || (archiveDrag && (tab.administratorBrowsing || tab.hasActiveAdminSession())))
      return std::nullopt;
    if (auto placeDest = m_places.dropDestinationAt(x, y))
      return placeDest;
    float gx = 0, gy = 0;
    Node::absolutePosition(grid, gx, gy);
    if (x < gx || y < gy || x >= gx + grid->width() || y >= gy + grid->height())
      return std::nullopt;
    const auto index = grid->indexAtSurface(x, y);
    if (index && *index < session.entries().size()) {
      if (!session.entries()[*index].isDir)
        return incoming ? std::optional(session.path()) : std::nullopt;
      return session.entries()[*index].absPath;
    }
    return session.path();
  }

  bool FileTransferController::dragSourcesAreFolders() const {
    const auto* origin = m_tabs.findTab(m_dragOriginTab);
    const auto& session = origin ? origin->session : m_tabs.activeTab().session;
    if (m_dragSources.empty())
      return false;
    return std::ranges::all_of(m_dragSources, [&session](const auto& source) {
      const auto found = std::ranges::find(session.entries(), std::filesystem::path(source), &FileEntry::absPath);
      return found != session.entries().end() && found->isDir;
    });
  }

  void FileTransferController::cancelDropValidation() {
    ++m_dropValidationGeneration;
    if (m_dropValidationCancellable != nullptr) {
      g_cancellable_cancel(m_dropValidationCancellable);
      g_clear_object(&m_dropValidationCancellable);
    }
    m_dropValidationPending.reset();
    m_dropValidation.reset();
  }

  void FileTransferController::validateDropAsync(
      TabId tabId, std::vector<std::string> sources, std::filesystem::path destination, std::uint64_t navigation,
      std::uint64_t dragSession, std::function<void(DropValidation)> onComplete
  ) {
    auto* activeTab = m_tabs.findTab(tabId);
    const bool administrator = activeTab && activeTab->administratorBrowsing;
    const auto adminGeneration = administrator ? activeTab->adminSession.sessionGeneration() : 0;
    if (m_dropValidation
        && m_dropValidation->sources == sources
        && m_dropValidation->rawDestination == destination
        && activeTab != nullptr
        && tabId == m_tabs.activeTabId()
        && activeTab->session.navigationGeneration() == navigation) {
      if (onComplete)
        onComplete(*m_dropValidation);
      return;
    }

    const auto generation = ++m_dropValidationGeneration;
    if (m_dropValidationCancellable != nullptr) {
      g_cancellable_cancel(m_dropValidationCancellable);
      g_clear_object(&m_dropValidationCancellable);
    }
    std::weak_ptr<bool> alive = m_alive;
    auto deliver = [this, alive, tabId, generation, navigation, dragSession, administrator, adminGeneration,
                    onComplete = std::move(onComplete)](DropValidation result) {
      const auto locked = alive.lock();
      const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
      const auto* tab = m_tabs.findTab(tabId);
      if (!locked
          || !*locked
          || quitting
          || m_dropValidationGeneration != generation
          || tab == nullptr
          || tabId != m_tabs.activeTabId()
          || tab->administratorBrowsing != administrator
          || (administrator
              && (!tab->hasActiveAdminSession() || tab->adminSession.sessionGeneration() != adminGeneration))
          || tab->session.navigationGeneration() != navigation
          || m_dragSessionGeneration != dragSession) {
        return;
      }
      m_dropValidationPending.reset();
      m_dropValidation = result;
      if (result.target && !administrator) {
        const auto normRaw = normalizeDirectoryPath(result.rawDestination);
        const auto normTarget = normalizeDirectoryPath(*result.target);
        const PlacePathResolution resolved{.exists = true, .isDirectory = true, .canonicalPath = normTarget};
        m_pathCache.cacheResolved(normRaw, resolved);
        m_pathCache.cacheResolved(normTarget, resolved);
      }
      if (onComplete)
        onComplete(std::move(result));
    };

    if (m_dropValidationHook) {
      if (m_dropValidationHook(sources, destination, deliver))
        return;
    }

    if (administrator) {
      struct AdminDrop {
        DropValidation validation;
        std::function<void(DropValidation)> deliver;
        AdminStatRecord target;
        std::vector<AdminStatRecord> sources;
        std::size_t remaining = 0;
        bool valid = true;
      };
      auto state = std::make_shared<AdminDrop>();
      state->validation.sources = std::move(sources);
      state->validation.rawDestination = destination;
      state->deliver = std::move(deliver);
      state->remaining = state->validation.sources.size() + 1;
      state->valid = !state->validation.sources.empty();
      const auto check = [&](const std::filesystem::path& path, bool target) {
        (void)activeTab->adminSession.statInfoAsync(
            path, false,
            [state, target](bool ok, const AdminStatRecord& stat, auto, const std::string&) {
              if (!ok || !stat.exists || stat.resolvedPath.empty()) {
                state->valid = false;
              } else if (target) {
                state->valid &= stat.isDir && stat.writable;
                state->target = stat;
              } else {
                state->sources.push_back(stat);
              }
              if (--state->remaining == 0) {
                const std::filesystem::path targetPath(state->target.resolvedPath);
                if (state->valid && std::ranges::none_of(state->sources, [&](const auto& source) {
                      const std::filesystem::path sourcePath(source.resolvedPath);
                      const auto relative = targetPath.lexically_relative(sourcePath);
                      return sourcePath.parent_path() == targetPath
                          || (source.isDir
                              && (sourcePath == targetPath || (!relative.empty() && *relative.begin() != "..")));
                    })) {
                  state->validation.target = state->validation.rawDestination;
                  state->validation.copyByDefault = std::ranges::any_of(state->sources, [&](const auto& source) {
                    return source.device != state->target.device;
                  });
                }
                state->deliver(std::move(state->validation));
              }
            },
            target ? AdminStatDetail::DropTarget : AdminStatDetail::DropSource
        );
      };
      for (const auto& path : state->validation.sources)
        check(path, false);
      check(destination, true);
      return;
    }

    m_dropValidationCancellable = g_cancellable_new();
    struct DropTaskInput {
      std::vector<std::string> sources;
      std::filesystem::path destination;
    };
    struct DropTaskCallback {
      std::function<void(DropValidation)> deliver;
    };
    GTask* task = g_task_new(
        nullptr, m_dropValidationCancellable,
        [](GObject*, GAsyncResult* result, gpointer userData) {
          std::unique_ptr<DropTaskCallback> cb(static_cast<DropTaskCallback*>(userData));
          GError* error = nullptr;
          auto* validated = static_cast<DropValidation*>(g_task_propagate_pointer(G_TASK(result), &error));
          const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
          g_clear_error(&error);
          if (validated != nullptr) {
            DropValidation value = std::move(*validated);
            delete validated;
            if (!cancelled && cb->deliver)
              cb->deliver(std::move(value));
          }
        },
        new DropTaskCallback{std::move(deliver)}
    );
    auto* input = new DropTaskInput{std::move(sources), std::move(destination)};
    g_task_set_task_data(task, input, [](gpointer p) { delete static_cast<DropTaskInput*>(p); });
    g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer taskData, GCancellable* cancellable) {
      if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(worker, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        return;
      }
      const auto& req = *static_cast<const DropTaskInput*>(taskData);
      DropValidation validated;
      validated.sources = req.sources;
      validated.rawDestination = req.destination;
      validated.target = filemanager::checkedDropTarget(req.sources, req.destination);
      if (validated.target)
        validated.copyByDefault = filemanager::copyDropByDefault(req.sources, *validated.target);
      g_task_return_pointer(worker, new DropValidation(std::move(validated)), [](gpointer p) {
        delete static_cast<DropValidation*>(p);
      });
    });
    g_object_unref(task);
  }

  void FileTransferController::requestHoverDropValidation(
      const std::vector<std::string>& sources, const std::filesystem::path& destination
  ) {
    if (m_dropValidation && m_dropValidation->sources == sources && m_dropValidation->rawDestination == destination)
      return;
    if (m_dropValidationPending
        && m_dropValidationPending->first == sources
        && m_dropValidationPending->second == destination)
      return;
    m_dropValidationPending = std::pair{sources, destination};
    const auto& active = m_tabs.activeTab();
    const TabId tabId = active.id;
    const auto navigation = active.session.navigationGeneration();
    const auto dragSession = m_dragSessionGeneration;
    validateDropAsync(
        tabId, sources, destination, navigation, dragSession,
        [this, alive = std::weak_ptr<bool>(m_alive), tabId, navigation, dragSession](DropValidation result) {
          const auto locked = alive.lock();
          const auto* tab = m_tabs.findTab(tabId);
          if (!locked
              || !*locked
              || tab == nullptr
              || tabId != m_tabs.activeTabId()
              || tab->session.navigationGeneration() != navigation
              || m_dragSessionGeneration != dragSession)
            return;
          if (m_draggingFiles || (m_protocolDragInside && m_transfer.dragging() && !m_transfer.dragDropped())) {
            const auto currentDest = dropDestination(m_dragPointerX, m_dragPointerY);
            if (currentDest && *currentDest == result.rawDestination && m_dragSources == result.sources) {
              clearDragFeedback();
              m_dragTargetValid = m_pinBefore.has_value() || result.target.has_value();
              const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
              if (result.target) {
                m_protocolDropDestination = result.target;
                m_protocolDropSources = m_dragSources;
                if (auto row = m_places.rowIndexAt(m_dragPointerX, m_dragPointerY)) {
                  m_dropPlace = m_places.placeRows()[*row].first;
                  m_dropPlace->setSelected(true);
                } else if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr) {
                  const auto& entries = tab->session.entries();
                  if (auto index = grid->indexAtSurface(m_dragPointerX, m_dragPointerY);
                      index && *index < entries.size() && entries[*index].isDir) {
                    m_dropIndex = index;
                    grid->notifyItemChanged(*index);
                  }
                }
                const bool copy = (modifiers & KeyMod::Ctrl) || (!(modifiers & KeyMod::Shift) && result.copyByDefault);
                if (m_callbacks.onStatus)
                  m_callbacks.onStatus(
                      std::string(copy ? "Copy " : "Move ")
                      + std::to_string(m_dragSources.size())
                      + " items to "
                      + result.target->string()
                      + " · Escape cancels"
                  );
              } else {
                m_protocolDropDestination.reset();
                if (m_callbacks.onStatus)
                  m_callbacks.onStatus("Cannot drop items here · Escape cancels");
              }
              const auto serial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
              if (serial)
                m_wayland.setCursorShape(
                    serial,
                    m_dragTargetValid ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                      : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
                );
              if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
                surface->requestUpdate();
              scheduleNativeDragPreview();
              if (m_protocolDragInside && m_transfer.dragging() && !m_transfer.dragDropped()) {
                DeferredCall::callLater([this, alive, tabId, navigation, dragSession] {
                  const auto stillAlive = alive.lock();
                  const auto* currentTab = m_tabs.findTab(tabId);
                  if (!stillAlive
                      || !*stillAlive
                      || currentTab == nullptr
                      || tabId != m_tabs.activeTabId()
                      || currentTab->session.navigationGeneration() != navigation
                      || m_dragSessionGeneration != dragSession
                      || !m_protocolDragInside
                      || !m_transfer.dragging()
                      || m_transfer.dragDropped())
                    return;
                  m_transfer.updateDragTarget();
                });
              }
            }
          }
        }
    );
  }

  void FileTransferController::stopHoverDropTab() {
    m_hoverDropTabTimer.stop();
    m_hoverDropTab = kInvalidTabId;
  }

  void FileTransferController::updateHoverDropTab() {
    const auto target =
        m_callbacks.fileDropTabAt ? m_callbacks.fileDropTabAt(m_dragPointerX, m_dragPointerY) : kInvalidTabId;
    const auto* origin = m_tabs.findTab(m_dragOriginTab);
    if (target == kInvalidTabId
        || target == m_tabs.activeTabId()
        || (origin && (origin->administratorBrowsing || origin->hasActiveAdminSession()))) {
      stopHoverDropTab();
      return;
    }
    if (target == m_hoverDropTab)
      return;
    m_hoverDropTab = target;
    m_hoverDropTabTimer.start(std::chrono::milliseconds(450), [this, target] {
      m_hoverDropTab = kInvalidTabId;
      if ((!m_protocolDragInside && !m_draggingFiles)
          || !m_callbacks.fileDropTabAt
          || m_callbacks.fileDropTabAt(m_dragPointerX, m_dragPointerY) != target)
        return;
      cancelDropValidation();
      m_switchingDragTab = true;
      const bool switched = m_tabs.activateTab(target);
      m_switchingDragTab = false;
      if (switched)
        m_refreshDropAfterLayout = true;
    });
  }

  std::uint32_t
  FileTransferController::updateProtocolDrag(wl_surface* surface, float x, float y, std::uint32_t actions) {
    constexpr auto copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    constexpr auto move = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    auto* mainSurface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    clearDragFeedback();
    m_pinBefore.reset();
    m_places.showPlaceInsertion(std::nullopt);
    m_dragTargetValid = false;
    m_protocolDropDestination.reset();
    m_protocolDropSources.clear();
    const bool wasProtocolDragInside = m_protocolDragInside;
    m_protocolDragInside = mainSurface && surface == mainSurface->wlSurface();
    const bool pointerMoved = !wasProtocolDragInside || x != m_dragPointerX || y != m_dragPointerY;
    m_dragPointerX = x;
    m_dragPointerY = y;
    if (windowOpen
        && m_protocolDragInside
        && (actions & (copy | move))
        && !m_places.hasActiveDeviceOperations()
        && !m_operations.replaying()
        && !renaming)
      updateHoverDropTab();
    else
      stopHoverDropTab();
    const auto& activeTab = m_tabs.activeTab();
    const auto& activeSession = activeTab.session;
    if (!windowOpen
        || !mainSurface
        || surface != mainSurface->wlSurface()
        || !grid
        || activeSession.browsingTrash()
        || !activeTab.capabilities().canAcceptDrop
        || m_places.hasActiveDeviceOperations()
        || m_operations.replaying()
        || renaming
        || activeSession.loading()) {
      stopDragScroll();
      if (mainSurface)
        mainSurface->requestUpdate();
      return 0;
    }
    if (pointerMoved)
      m_dispatcher.cancelPointerCapture();
    recordDragScrollOffsets();
    updateDragScroll();
    const bool own = m_transfer.dragging() && !m_transfer.dragDropped() && !m_dragSources.empty();
    const bool folders = own && dragSourcesAreFolders();
    m_pinBefore = folders ? m_places.placeInsertionAt(x, y, "Personal", true) : std::nullopt;
    m_places.showPlaceInsertion(m_pinBefore);
    auto rawDestination = m_pinBefore ? std::nullopt : dropDestination(x, y, !own);
    std::optional<std::filesystem::path> destination = rawDestination;
    bool copyDefault = true;
    if (own && rawDestination) {
      if (!(m_dropValidation
            && m_dropValidation->sources == m_dragSources
            && m_dropValidation->rawDestination == *rawDestination)) {
        requestHoverDropValidation(m_dragSources, *rawDestination);
      }
      if (m_dropValidation
          && m_dropValidation->sources == m_dragSources
          && m_dropValidation->rawDestination == *rawDestination) {
        destination = m_dropValidation->target;
        copyDefault = m_dropValidation->copyByDefault;
      } else {
        // Fail closed while asynchronous drop validation is in flight.
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Checking drop target… · Escape cancels");
        mainSurface->requestUpdate();
        scheduleNativeDragPreview();
        return 0;
      }
    }
    if (!m_pinBefore && !destination) {
      mainSurface->requestUpdate();
      return 0;
    }
    const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    std::uint32_t action = copy;
    if (!m_pinBefore) {
      if (modifiers & KeyMod::Ctrl)
        action = copy;
      else if (modifiers & KeyMod::Shift)
        action = move;
      else {
        action = own && !copyDefault ? move : copy;
        if (!(actions & action))
          action = actions & copy ? copy : move;
      }
    }
    if (!(actions & action)) {
      mainSurface->requestUpdate();
      return 0;
    }
    m_dragTargetValid = true;
    m_protocolDropDestination = destination;
    if (own)
      m_protocolDropSources = m_dragSources;
    if (auto index = grid->indexAtSurface(x, y); destination && index && activeSession.entries()[*index].isDir) {
      m_dropIndex = index;
      grid->notifyItemChanged(*index);
    }
    if (destination) {
      if (auto row = m_places.rowIndexAt(x, y)) {
        m_dropPlace = m_places.placeRows()[*row].first;
        m_dropPlace->setSelected(true);
      }
    }
    if (m_callbacks.onStatus)
      m_callbacks.onStatus(
          m_pinBefore ? "Pin folders in Places · Escape cancels"
                      : std::string(action == copy ? "Copy files to " : "Move files to ") + destination->string()
      );
    mainSurface->requestUpdate();
    scheduleNativeDragPreview();
    return action;
  }

  NativeDataTransfer::DropReceiver FileTransferController::receiveProtocolDrop(std::uint32_t action) {
    stopHoverDropTab();
    m_protocolDragInside = false;
    stopDragScroll();
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestUpdate();
    const auto pin = m_pinBefore;
    const auto destination = m_protocolDropDestination;
    if ((!pin && !destination) || !m_dragTargetValid)
      return {};
    const auto ownPaths = m_protocolDropSources;
    const auto& active = m_tabs.activeTab();
    const TabId tabId = active.id;
    const bool administrator = active.administratorBrowsing;
    const auto adminGeneration = active.adminSession.sessionGeneration();
    const auto navigation = active.session.navigationGeneration();
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const auto scrollGeneration = grid ? grid->scrollView().userScrollGeneration() : 0;
    return [this, alive = std::weak_ptr<bool>(m_alive), tabId, pin, destination, ownPaths, navigation, scrollGeneration,
            administrator, adminGeneration,
            action](std::optional<NativeDataTransfer::Payload> payload, NativeDataTransfer::FinishDrop finish) {
      const auto locked = alive.lock();
      const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
      const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
      auto* currentTab = m_tabs.findTab(tabId);
      if (!locked
          || !*locked
          || !windowOpen
          || quitting
          || currentTab == nullptr
          || tabId != m_tabs.activeTabId()
          || currentTab->administratorBrowsing != administrator
          || (administrator
              && (currentTab->adminSession.sessionGeneration() != adminGeneration
                  || currentTab->session.navigationGeneration() != navigation))
          || !payload
          || !payload->contains("text/uri-list")
          || m_places.hasActiveDeviceOperations()
          || m_operations.replaying()) {
        finish(false);
        return;
      }
      const auto dropNav = currentTab->session.navigationGeneration();
      const auto dropDragSession = m_dragSessionGeneration;
      try {
        auto paths = filemanager::decodeFileTransfer(payload->at("text/uri-list"), false).paths;
        if (paths.empty() || (!ownPaths.empty() && ownPaths != paths)) {
          finish(false);
          return;
        }
        if (pin) {
          if (action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
            finish(false);
            return;
          }
          struct PinCheck {
            std::vector<std::string> paths;
            std::string before;
            std::size_t remaining = 0;
            bool allFolders = true;
            NativeDataTransfer::FinishDrop finish;
            ~PinCheck() {
              if (finish)
                finish(false);
            }
          };
          auto check = std::make_shared<PinCheck>();
          check->paths = paths;
          check->before = *pin;
          check->remaining = paths.size();
          check->finish = std::move(finish);
          for (const auto& source : paths) {
            auto onResolved = [this, alive, tabId, navigation, administrator, adminGeneration,
                               check](bool isDirectory) {
              const auto stillAlive = alive.lock();
              if (!stillAlive || !*stillAlive || !check->finish)
                return;
              const bool stillQuitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
              const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
              const auto* tab = m_tabs.findTab(tabId);
              if (stillQuitting
                  || !stillOpen
                  || tab == nullptr
                  || tabId != m_tabs.activeTabId()
                  || tab->administratorBrowsing != administrator
                  || (administrator && tab->adminSession.sessionGeneration() != adminGeneration)
                  || tab->session.navigationGeneration() != navigation) {
                if (check->finish) {
                  std::exchange(check->finish, {})(false);
                }
                return;
              }
              if (!isDirectory)
                check->allFolders = false;
              if (--check->remaining > 0)
                return;
              auto done = std::exchange(check->finish, {});
              if (!check->allFolders) {
                if (done)
                  done(false);
                return;
              }
              m_places.pinFolders(check->paths, check->before, administrator);
              if (done)
                done(true);
            };
            if (administrator) {
              if (!currentTab->adminSession.statInfoAsync(
                      source, false, [onResolved](bool ok, const auto& stat, auto, const std::string&) {
                        onResolved(ok && stat.exists && stat.isDir);
                      }
                  )) {
                if (check->finish)
                  std::exchange(check->finish, {})(false);
                return;
              }
            } else {
              m_pathCache.resolveAsync(source, [onResolved](const auto& result) { onResolved(result.isDirectory); });
            }
          }
        } else {
          struct DropFinishGuard {
            NativeDataTransfer::FinishDrop finish;
            explicit DropFinishGuard(NativeDataTransfer::FinishDrop fn) : finish(std::move(fn)) {}
            DropFinishGuard(const DropFinishGuard&) = delete;
            DropFinishGuard& operator=(const DropFinishGuard&) = delete;
            void complete(bool ok) {
              if (finish) {
                auto fn = std::move(finish);
                fn(ok);
              }
            }
            ~DropFinishGuard() { complete(false); }
          };
          auto finishGuard = std::make_shared<DropFinishGuard>(std::move(finish));
          validateDropAsync(
              tabId, std::move(paths), *destination, dropNav, dropDragSession,
              [this, alive, tabId, navigation, dropNav, scrollGeneration, action,
               finishGuard](DropValidation validated) {
                const auto stillAlive = alive.lock();
                const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
                const bool stillQuitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
                auto* tab = m_tabs.findTab(tabId);
                if (!stillAlive
                    || !*stillAlive
                    || !stillOpen
                    || stillQuitting
                    || tab == nullptr
                    || tabId != m_tabs.activeTabId()
                    || tab->session.navigationGeneration() != dropNav
                    || m_places.hasActiveDeviceOperations()
                    || m_operations.replaying()) {
                  finishGuard->complete(false);
                  return;
                }
                if (validated.target) {
                  auto* adminTab = tab->administratorBrowsing ? tab : nullptr;
                  const auto id = action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
                      ? m_operations.move(
                            std::move(validated.sources), validated.target->string(), adminOperationContext(adminTab)
                        )
                      : m_operations.copy(
                            std::move(validated.sources), validated.target->string(), adminOperationContext(adminTab)
                        );
                  if (!id) {
                    finishGuard->complete(false);
                    return;
                  }
                  m_clipboard.registerPasteRequest(
                      id, PasteRequest{tabId, validated.target->string(), navigation, scrollGeneration}
                  );
                  m_dropFinishes.emplace(id, std::move(finishGuard->finish));
                } else {
                  if (m_callbacks.onStatus)
                    m_callbacks.onStatus("Cannot drop unavailable files or copy a folder into itself");
                  finishGuard->complete(false);
                }
              }
          );
        }
      } catch (const std::exception& error) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus(std::string("Cannot drop files: ") + error.what());
        if (finish)
          finish(false);
      }
    };
  }

  void FileTransferController::cancelFileGesture() {
    stopHoverDropTab();
    m_dragOriginTab = kInvalidTabId;
    m_archiveDrag = {};
    m_archiveDragPassword = {};
    ++m_dragSessionGeneration;
    cancelDropValidation();
    if (!m_transfer.dragDropped())
      m_transfer.cancelDrag();
    clearNativeDragPreview();
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
      grid->endRubberBand(true);
    m_dragPending = false;
    m_draggingFiles = false;
    m_dragPressSerial = 0;
    m_protocolDragInside = false;
    m_protocolDropDestination.reset();
    m_protocolDropSources.clear();
    m_dragIndex.reset();
    m_dragSources.clear();
    m_pinBefore.reset();
    if (m_callbacks.onCancelPlaceReorder)
      m_callbacks.onCancelPlaceReorder();
    m_previewRenderer.hideOverlay();
    m_places.showPlaceInsertion(std::nullopt);
    clearDragFeedback();
    stopDragScroll();
    m_dispatcher.cancelPointerCapture();
  }

  void FileTransferController::clearNativeDragPreview() { m_previewRenderer.clearNative(); }

  void FileTransferController::scheduleNativeDragPreview() {
    m_previewRenderer.scheduleNative(
        m_renderContext, m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr,
        [this] { return m_transfer.dragging() && !m_transfer.dragDropped(); },
        [this](Renderer& renderer) { updateDragPreview(renderer, true); },
        [this](const std::string& error) {
          cancelFileGesture();
          if (m_callbacks.onStatus)
            m_callbacks.onStatus("Could not render drag preview: " + error);
        }
    );
  }

  void FileTransferController::updateDragPreview(Renderer& renderer, bool nativeIcon) {
    auto* root = m_callbacks.sceneRoot ? m_callbacks.sceneRoot() : nullptr;
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!root || !surface)
      return;
    if (!m_draggingFiles
        || m_dragSources.empty()
        || (!nativeIcon
            && (m_transfer.dragIconVisible()
                || (m_transfer.dragging() && (!m_protocolDragInside || m_transfer.dragDropped()))))) {
      m_previewRenderer.hideOverlay();
      return;
    }
    const auto* origin = m_tabs.findTab(m_dragOriginTab);
    const auto& sourceSession = origin ? origin->session : m_tabs.activeTab().session;
    const std::filesystem::path path(m_dragSources.front());
    const auto previewEntry = std::ranges::find(sourceSession.entries(), path, &FileEntry::absPath);
    const auto destination = dropDestination(m_dragPointerX, m_dragPointerY);
    const bool externalTarget = nativeIcon && m_transfer.dragging() && !m_protocolDragInside;
    const bool targetValid = externalTarget ? m_transfer.dragAction() != 0 : m_dragTargetValid;
    const bool cachedCopyDefault = destination
            && m_dropValidation
            && m_dropValidation->sources == m_dragSources
            && m_dropValidation->rawDestination == *destination
        ? m_dropValidation->copyByDefault
        : true;
    const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    const bool copy = m_archiveDrag.catalogue
        || (externalTarget
                ? m_transfer.dragAction() == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                : (modifiers & KeyMod::Ctrl) || (destination && !(modifiers & KeyMod::Shift) && cachedCopyDefault));
    m_previewRenderer.updateOverlay(
        renderer, root, surface,
        {
            .primaryPath = path,
            .primaryIsDir = previewEntry != sourceSession.entries().end() && previewEntry->isDir,
            .allowFilesystemQueries = !m_archiveDrag.catalogue && !sourceSession.search().active(),
            .itemCount = m_dragSources.size(),
            .targetValid = targetValid,
            .actionLabel = m_pinBefore ? "Pin in Places"
                : !targetValid         ? "Cannot drop here"
                : copy                 ? "Copy here"
                                       : "Move here",
            .pointerX = m_dragPointerX,
            .pointerY = m_dragPointerY,
        }
    );
  }

  void FileTransferController::clearDragFeedback() {
    if (m_dropPlace)
      m_dropPlace->setSelected(false);
    m_dropPlace = nullptr;
    auto old = m_dropIndex;
    m_dropIndex.reset();
    if (old) {
      if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
        grid->notifyItemChanged(*old);
    }
    if (m_callbacks.onUpdateNavigationControls)
      m_callbacks.onUpdateNavigationControls();
  }

  void FileTransferController::beginFileDragPress(std::size_t index, float x, float y, std::uint32_t serial) {
    if (!m_tabs.activeTab().capabilities().canDragSource)
      return;
    m_dragIndex = index;
    m_dragStartX = x;
    m_dragStartY = y;
    m_dragPending = true;
    m_dragPressSerial = serial;
  }

  void FileTransferController::handlePointerLeaveDuringDrag() {
    stopDragScroll();
    if (!m_transfer.dragging())
      cancelFileGesture();
    else {
      clearDragFeedback();
      m_previewRenderer.hideOverlay();
    }
    m_dispatcher.pointerLeave();
  }

  void FileTransferController::handleAxisDuringProtocolDrag(const PointerEvent& event) {
    m_dispatcher.pointerAxis(
        m_dragPointerX, m_dragPointerY, event.axis, event.axisSource, event.axisValue, event.axisDiscrete,
        event.axisValue120, event.axisLines, event.axisGestureSerial
    );
    m_refreshDropAfterLayout = true;
    m_transfer.updateDragTarget();
    recordDragScrollOffsets();
    updateDragScroll();
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestUpdate();
  }

  void FileTransferController::cancelLocalDragState() {
    stopHoverDropTab();
    stopDragScroll();
    m_dragOriginTab = kInvalidTabId;
    m_archiveDrag = {};
    m_archiveDragPassword = {};
    m_dragPending = false;
    m_draggingFiles = false;
    m_dragIndex.reset();
    m_dragSources.clear();
    m_pinBefore.reset();
    m_previewRenderer.hideOverlay();
    clearDragFeedback();
  }

  bool FileTransferController::canDragScroll() const {
    if (!m_protocolDragInside && !(m_draggingFiles && !m_transfer.dragging()))
      return false;
    const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
    const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    const auto& activeTab = m_tabs.activeTab();
    const auto& activeSession = activeTab.session;
    return windowOpen
        && !quitting
        && !renaming
        && !m_places.hasActiveDeviceOperations()
        && !m_operations.replaying()
        && !activeSession.browsingTrash()
        && activeTab.capabilities().canAcceptDrop
        && !activeSession.loading();
  }

  float FileTransferController::dragScrollSpeed(bool& inPlaces) const {
    inPlaces = false;
    if (!canDragScroll())
      return 0.0F;
    if (auto* placesScroll = m_places.scrollView(); isNodeEffectivelyVisible(placesScroll)) {
      float px = 0.0F;
      float py = 0.0F;
      Node::absolutePosition(placesScroll, px, py);
      if (m_dragPointerX >= px
          && m_dragPointerX < px + placesScroll->width()
          && m_dragPointerY >= py
          && m_dragPointerY < py + placesScroll->height()) {
        inPlaces = true;
        return edgeScrollSpeed(*placesScroll, m_dragPointerY, py);
      }
    }
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr; isNodeEffectivelyVisible(grid)) {
      auto& scroll = grid->scrollView();
      float gx = 0.0F;
      float gy = 0.0F;
      Node::absolutePosition(&scroll, gx, gy);
      if (m_dragPointerX >= gx
          && m_dragPointerX < gx + scroll.width()
          && m_dragPointerY >= gy
          && m_dragPointerY < gy + scroll.height()) {
        return edgeScrollSpeed(scroll, m_dragPointerY, gy);
      }
    }
    return 0.0F;
  }

  void FileTransferController::recordDragScrollOffsets() {
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
      m_lastDragGridScrollOffset = grid->scrollView().scrollOffset();
    else
      m_lastDragGridScrollOffset.reset();
    if (auto* placesScroll = m_places.scrollView())
      m_lastDragPlacesScrollOffset = placesScroll->scrollOffset();
    else
      m_lastDragPlacesScrollOffset.reset();
  }

  void FileTransferController::stopDragScroll() {
    m_dragScroll.stop();
    m_dragScrollDelta = 0.0F;
    m_dragScrollInPlaces = false;
    m_refreshDropAfterLayout = false;
    m_lastDragGridScrollOffset.reset();
    m_lastDragPlacesScrollOffset.reset();
  }

  void FileTransferController::updateDragScroll() {
    bool places = false;
    const float speed = dragScrollSpeed(places);
    if (places != m_dragScrollInPlaces) {
      m_dragScrollDelta = 0.0F;
      m_dragScrollInPlaces = places;
    }
    if (speed == 0.0F) {
      m_dragScroll.stop();
      m_dragScrollDelta = 0.0F;
      return;
    }
    if (!m_dragScroll.active()) {
      std::weak_ptr<bool> alive = m_alive;
      m_dragScroll.startRepeating(kDragScrollInterval, [this, alive] {
        const auto locked = alive.lock();
        if (!locked || !*locked)
          return;
        bool tickPlaces = false;
        const float tickSpeed = dragScrollSpeed(tickPlaces);
        if (tickPlaces != m_dragScrollInPlaces) {
          m_dragScrollDelta = 0.0F;
          m_dragScrollInPlaces = tickPlaces;
        }
        if (tickSpeed == 0.0F) {
          m_dragScroll.stop();
          m_dragScrollDelta = 0.0F;
          return;
        }
        m_dragScrollDelta = std::clamp(m_dragScrollDelta + tickSpeed, -kDragScrollMaxDeltaPx, kDragScrollMaxDeltaPx);
        if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
          surface->requestLayout();
      });
    }
  }

  bool FileTransferController::applyDragScroll() {
    if (m_dragScrollDelta == 0.0F)
      return false;
    const float delta = std::exchange(m_dragScrollDelta, 0.0F);
    if (!canDragScroll()) {
      stopDragScroll();
      return false;
    }
    ScrollView* scroll = m_dragScrollInPlaces ? m_places.scrollView() : nullptr;
    if (!m_dragScrollInPlaces) {
      if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr; isNodeEffectivelyVisible(grid))
        scroll = &grid->scrollView();
    }
    if (scroll == nullptr || !isNodeEffectivelyVisible(scroll))
      return false;
    scroll->requestScrollToOffset(scroll->scrollOffset() + delta);
    m_refreshDropAfterLayout = true;
    return true;
  }

  bool FileTransferController::refreshDropTargetAfterScroll() {
    const bool activeDrag = m_protocolDragInside || (m_draggingFiles && !m_transfer.dragging());
    if (!activeDrag) {
      m_refreshDropAfterLayout = false;
      return false;
    }
    bool scrollChanged = m_refreshDropAfterLayout;
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr) {
      const float current = grid->scrollView().scrollOffset();
      if (!m_lastDragGridScrollOffset || std::abs(current - *m_lastDragGridScrollOffset) >= 0.001F)
        scrollChanged = true;
    }
    if (auto* placesScroll = m_places.scrollView()) {
      const float current = placesScroll->scrollOffset();
      if (!m_lastDragPlacesScrollOffset || std::abs(current - *m_lastDragPlacesScrollOffset) >= 0.001F)
        scrollChanged = true;
    }
    m_refreshDropAfterLayout = false;
    recordDragScrollOffsets();
    if (!scrollChanged)
      return false;
    if (m_protocolDragInside) {
      m_transfer.updateDragTarget();
      updateDragScroll();
    } else {
      updateLocalDragTarget(m_dragPointerX, m_dragPointerY);
    }
    return true;
  }

  void FileTransferController::updateLocalDragTarget(float x, float y) {
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    clearDragFeedback();
    const bool pointerMoved = x != m_dragPointerX || y != m_dragPointerY;
    m_dragPointerX = x;
    m_dragPointerY = y;
    if (pointerMoved)
      m_dispatcher.cancelPointerCapture();
    updateHoverDropTab();
    const bool archiveDrag = m_archiveDrag.catalogue != nullptr;
    const bool folders = !archiveDrag && dragSourcesAreFolders();
    m_pinBefore = folders ? m_places.placeInsertionAt(x, y, "Personal", true) : std::nullopt;
    m_places.showPlaceInsertion(m_pinBefore);
    const auto rawDestination = m_pinBefore ? std::nullopt : dropDestination(x, y);
    std::optional<std::filesystem::path> destination;
    bool checking = false;
    bool copyDefault = true;
    if (archiveDrag) {
      destination = rawDestination;
    } else if (rawDestination) {
      if (!(m_dropValidation
            && m_dropValidation->sources == m_dragSources
            && m_dropValidation->rawDestination == *rawDestination)) {
        requestHoverDropValidation(m_dragSources, *rawDestination);
      }
      if (m_dropValidation
          && m_dropValidation->sources == m_dragSources
          && m_dropValidation->rawDestination == *rawDestination) {
        destination = m_dropValidation->target;
        copyDefault = m_dropValidation->copyByDefault;
      } else {
        checking = true;
      }
    }
    if (destination) {
      if (auto row = m_places.rowIndexAt(x, y)) {
        m_dropPlace = m_places.placeRows()[*row].first;
        m_dropPlace->setSelected(true);
      } else if (grid) {
        if (auto index = grid->indexAtSurface(x, y)) {
          m_dropIndex = index;
          grid->notifyItemChanged(*index);
        }
      }
    }
    m_dragTargetValid = m_pinBefore.has_value() || destination.has_value();
    const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    if (m_callbacks.onStatus)
      m_callbacks.onStatus(
          m_pinBefore    ? "Pin folders in Places · Escape cancels"
              : checking ? "Checking drop target… · Escape cancels"
                         : std::string(
                               (archiveDrag
                                || (modifiers & KeyMod::Ctrl)
                                || (destination && !(modifiers & KeyMod::Shift) && copyDefault))
                                   ? "Copy "
                                   : "Move "
                           )
                  + std::to_string(m_dragSources.size())
                  + " items"
                  + (destination ? " to " + destination->string() : " — no drop target")
                  + " · Escape cancels"
      );
    const auto serial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
    if (serial)
      m_wayland.setCursorShape(
          serial,
          m_dragTargetValid ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
      );
    recordDragScrollOffsets();
    updateDragScroll();
    if (surface)
      surface->requestUpdate();
  }

  bool FileTransferController::handleFileDragPointer(const PointerEvent& event, float x, float y) {
    if (!m_dragPending && !m_draggingFiles)
      return false;
    if (!m_tabs.activeTab().capabilities().canDragSource) {
      cancelFileGesture();
      return false;
    }
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (event.type == PointerEvent::Type::Axis) {
      if (m_draggingFiles && (x != m_dragPointerX || y != m_dragPointerY))
        m_dispatcher.cancelPointerCapture();
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      if (m_draggingFiles) {
        m_dragPointerX = x;
        m_dragPointerY = y;
        m_refreshDropAfterLayout = true;
        updateLocalDragTarget(x, y);
      } else if (m_callbacks.onSynthesizeMotion) {
        auto motion = event;
        motion.type = PointerEvent::Type::Motion;
        m_callbacks.onSynthesizeMotion(motion);
      }
      return true;
    }
    if (event.type == PointerEvent::Type::Motion) {
      if (!m_draggingFiles && std::hypot(x - m_dragStartX, y - m_dragStartY) > 5.0F) {
        m_draggingFiles = true;
        m_dragSources =
            m_callbacks.selectedPathsOrdered ? m_callbacks.selectedPathsOrdered() : std::vector<std::string>{};
        if (m_dragSources.empty()) {
          cancelFileGesture();
          return true;
        }
        if (m_callbacks.onClearLastPointerIndex)
          m_callbacks.onClearLastPointerIndex();
        m_dispatcher.pointerLeave();
        TooltipManager::instance().forceDestroy();
        const auto& origin = m_tabs.activeTab();
        m_dragOriginTab = origin.id;
        if (origin.session.browsingArchive()) {
          m_archiveDrag = {origin.session.archiveCatalogue(), m_dragSources};
          m_archiveDragPassword = origin.session.archivePassword();
          updateLocalDragTarget(x, y);
          return true;
        }
        try {
          auto payload = filemanager::encodeFileTransfer(m_dragSources, false);
          // Clipboard cut metadata is not drag action negotiation.
          payload.erase("application/x-kde-cutselection");
          payload.erase("x-special/gnome-copied-files");
          m_dragPointerX = x;
          m_dragPointerY = y;
          clearNativeDragPreview();
          m_previewRenderer.setNativeSurface(
              m_wayland.compositor() ? wl_compositor_create_surface(m_wayland.compositor()) : nullptr
          );
          if (surface
              && m_transfer.startDrag(
                  std::move(payload), surface->wlSurface(), m_dragPressSerial, nullptr,
                  m_previewRenderer.nativeSurface()
              )) {
            scheduleNativeDragPreview();
            return true;
          }
          clearNativeDragPreview();
        } catch (const std::exception& error) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus(std::string("Cannot start file drag: ") + error.what());
          cancelFileGesture();
          return true;
        }
      }
      if (m_draggingFiles)
        updateLocalDragTarget(x, y);
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      stopHoverDropTab();
      if (m_draggingFiles && m_archiveDrag.catalogue) {
        if (const auto destination = dropDestination(x, y); destination && !m_places.hasActiveDeviceOperations()) {
          const auto id = m_operations.extractSelected(
              std::move(m_archiveDrag), destination->native(), std::move(m_archiveDragPassword)
          );
          if (!id && m_callbacks.onStatus)
            m_callbacks.onStatus("Could not start extracting the archive selection.");
        }
        cancelFileGesture();
        return true;
      }
      if (m_draggingFiles) {
        const bool folders = dragSourcesAreFolders();
        const auto pin = folders ? m_places.placeInsertionAt(x, y, "Personal", true) : std::nullopt;
        const auto destination = pin ? std::nullopt : dropDestination(x, y);
        if (pin)
          m_places.pinFolders(m_dragSources, *pin, m_tabs.activeTab().administratorBrowsing);
        else if (
            destination && !m_dragSources.empty() && !m_places.hasActiveDeviceOperations() && !m_operations.replaying()
        ) {
          const auto& active = m_tabs.activeTab();
          const TabId tabId = active.id;
          const auto navigation = active.session.navigationGeneration();
          const auto dragSession = m_dragSessionGeneration;
          const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
          validateDropAsync(
              tabId, m_dragSources, *destination, navigation, dragSession,
              [this, alive = std::weak_ptr<bool>(m_alive), tabId, navigation, modifiers](DropValidation validated) {
                const auto locked = alive.lock();
                const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
                const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
                auto* tab = m_tabs.findTab(tabId);
                if (!locked
                    || !*locked
                    || quitting
                    || !windowOpen
                    || tab == nullptr
                    || tabId != m_tabs.activeTabId()
                    || tab->session.navigationGeneration() != navigation
                    || m_places.hasActiveDeviceOperations()
                    || m_operations.replaying())
                  return;
                if (validated.target) {
                  const bool copy =
                      (modifiers & KeyMod::Ctrl) || (!(modifiers & KeyMod::Shift) && validated.copyByDefault);
                  auto* administrator = tab->administratorBrowsing ? tab : nullptr;
                  const auto id = copy ? m_operations.copy(
                                             std::move(validated.sources), validated.target->string(),
                                             adminOperationContext(administrator)
                                         )
                                       : m_operations.move(
                                             std::move(validated.sources), validated.target->string(),
                                             adminOperationContext(administrator)
                                         );
                  if (id)
                    m_clipboard.registerPasteRequest(id, PasteRequest{tabId, validated.target->string(), navigation});
                } else if (m_callbacks.onStatus) {
                  m_callbacks.onStatus("Cannot drop items into themselves or their own subfolders");
                }
              }
          );
        } else if (m_callbacks.onStatus) {
          m_callbacks.onStatus("Drop cancelled");
        }
      } else if (m_dragIndex && *m_dragIndex < m_tabs.activeTab().session.entries().size()) {
        if (m_callbacks.onClickUnmovedDragItem)
          m_callbacks.onClickUnmovedDragItem(*m_dragIndex, event.time);
      }
      m_dragOriginTab = kInvalidTabId;
      m_dragPending = false;
      m_draggingFiles = false;
      m_dragIndex.reset();
      m_dragSources.clear();
      m_pinBefore.reset();
      m_places.showPlaceInsertion(std::nullopt);
      m_previewRenderer.hideOverlay();
      clearDragFeedback();
      stopDragScroll();
      m_dispatcher.cancelPointerCapture();
      const auto serial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
      if (serial)
        m_wayland.setCursorShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      return true;
    }
    return false;
  }

} // namespace filemanager
