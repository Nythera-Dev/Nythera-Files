#include "core/input/key_modifiers.h"
#include "core/timer_manager.h"
#include "filemanager/admin_helper_session.h"
#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "wayland/toplevel_surface.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client-protocol.h>

namespace {

  using namespace filemanager;

  class StubTextures final : public TextureManager {
  public:
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRgba(const std::uint8_t*, int, int, bool) override { return {}; }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override {
      return {};
    }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * fontSize * 0.5F;
      return {.width = maxWidth > 0.0F ? std::min(natural, maxWidth) : natural, .bottom = fontSize};
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.width = size, .bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
        const TextFont&
    ) override {
      outStops.assign(byteOffsets.size(), 0.0F);
    }
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>& byteOffsets, float,
        std::vector<TextCursorStop>& outStops, const TextFont&
    ) override {
      outStops.assign(byteOffsets.size(), TextCursorStop{});
    }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .bottom = size}; }
    TextureManager& textureManager() override { return m_textures; }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    StubTextures m_textures;
  };

  class Adapter final : public VirtualGridAdapter {
  public:
    explicit Adapter(const std::vector<FileEntry>& entries) : m_entries(entries) {}
    std::size_t itemCount() const override { return m_entries.size(); }
    std::unique_ptr<Node> createTile() override { return std::make_unique<Node>(); }
    void bindTile(Node&, std::size_t, bool, bool) override {}

  private:
    const std::vector<FileEntry>& m_entries;
  };

  template <typename Predicate>
  void pumpUntil(
      Predicate&& predicate, int timeoutMs = 5000, std::source_location caller = std::source_location::current()
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      if (!g_main_context_iteration(nullptr, FALSE))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!predicate())
      g_printerr("Timed out at %s:%u\n", caller.file_name(), caller.line());
    TEST_CHECK(predicate());
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
  }

  void tickDragScrollTimer(int ticks = 1) {
    for (int i = 0; i < ticks; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      TimerManager::instance().tick();
    }
  }

  struct InProcessHelperFixture {
    struct SharedChannelState {
      int clientReadFd = -1;
      int clientWriteFd = -1;
      int helperReadFd = -1;
      int helperWriteFd = -1;
      std::mutex fdMutex;
      std::thread helperThread;

      ~SharedChannelState() {
        closeClientWrite();
        abortTransport();
        if (helperThread.joinable())
          helperThread.join();
        std::lock_guard lock(fdMutex);
        if (clientReadFd >= 0)
          ::close(clientReadFd);
        if (helperReadFd >= 0)
          ::close(helperReadFd);
      }

      void closeClientWrite() {
        std::lock_guard lock(fdMutex);
        if (clientWriteFd >= 0) {
          ::close(clientWriteFd);
          clientWriteFd = -1;
        }
      }

      void closeHelperWrite() {
        std::lock_guard lock(fdMutex);
        if (helperWriteFd >= 0) {
          ::close(helperWriteFd);
          helperWriteFd = -1;
        }
      }

      void abortTransport() {
        closeClientWrite();
        std::lock_guard lock(fdMutex);
        if (clientReadFd >= 0) {
          ::close(clientReadFd);
          clientReadFd = -1;
        }
      }
    };

    AdminTransportLauncher makeLauncher() {
      return [](GCancellable*) -> AdminTransportLaunchResult {
        int c2h[2]{-1, -1};
        int h2c[2]{-1, -1};
        TEST_CHECK(::pipe2(c2h, O_CLOEXEC) == 0);
        if (::pipe2(h2c, O_CLOEXEC) != 0) {
          ::close(c2h[0]);
          ::close(c2h[1]);
          TEST_CHECK(false);
        }
        auto state = std::make_shared<SharedChannelState>();
        state->helperReadFd = c2h[0];
        state->clientWriteFd = c2h[1];
        state->clientReadFd = h2c[0];
        state->helperWriteFd = h2c[1];
        state->helperThread = std::thread([state]() {
          (void)runAdminHelperSession(
              state->helperReadFd, state->helperWriteFd,
              AdminHelperSessionConfig{.helperPid = 5000, .invokingUid = 1000}
          );
          state->closeHelperWrite();
        });
        auto channel = std::make_unique<AdminTransportChannel>();
        channel->readFd = state->clientReadFd;
        channel->writeFd = state->clientWriteFd;
        channel->closeWrite = [state] { state->closeClientWrite(); };
        channel->abortTransport = [state] { state->abortTransport(); };
        channel->waitForExit = [state]() -> int {
          state->closeClientWrite();
          if (state->helperThread.joinable())
            state->helperThread.join();
          return 0;
        };
        return AdminTransportLaunchResult{.channel = std::move(channel)};
      };
    }
  };

} // namespace

class FileManagerTestAccess {
  struct DragScene {
    Flex root;
    Flex* sidebar = nullptr;
    ScrollView* placesScroll = nullptr;
    std::vector<Button*> placeRowButtons;
    VirtualGridView* grid = nullptr;
    std::unique_ptr<Adapter> adapter;
    StubRenderer renderer;

    std::vector<std::filesystem::path> placeDirs;

    void attach(FileManager& manager, const std::filesystem::path& rootDir, std::size_t entryCount = 20) {
      while (!root.children().empty())
        root.removeChild(root.children().front().get());
      auto& tab = manager.activeTab();
      tab.scrollState = {};
      auto& session = tab.session;
      session.m_entries.clear();
      for (std::size_t i = 0; i < entryCount; ++i) {
        const bool isFolder = (i == 6 || i == 7 || i == 12);
        const std::string name = isFolder ? "folder-" + std::to_string(i) : "file-" + std::to_string(i) + ".txt";
        const auto entryPath = rootDir / name;
        if (isFolder)
          std::filesystem::create_directories(entryPath);
        else
          std::ofstream(entryPath) << "fixture";
        session.m_entries.push_back({.name = name, .absPath = entryPath, .isDir = isFolder});
      }

      root.setDirection(FlexDirection::Horizontal);
      root.setAlign(FlexAlign::Stretch);
      root.setGap(0.0F);
      root.setPadding(0.0F);
      root.setSize(520.0F, 120.0F);

      auto sidebarNode = std::make_unique<Flex>();
      sidebarNode->setDirection(FlexDirection::Vertical);
      sidebarNode->setAlign(FlexAlign::Stretch);
      sidebarNode->setPadding(0.0F);
      sidebarNode->setGap(0.0F);
      sidebarNode->setMinWidth(180.0F);
      sidebarNode->setMaxWidth(180.0F);

      auto places = std::make_unique<ScrollView>();
      places->setFlexGrow(1.0F);
      places->setViewportPaddingH(0.0F);
      places->setViewportPaddingV(0.0F);
      places->setScrollbarVisible(false);
      places->content()->setPadding(0.0F);
      places->content()->setGap(0.0F);
      places->content()->setAlign(FlexAlign::Stretch);
      placesScroll = places.get();

      placeDirs.clear();
      placeRowButtons.clear();
      manager.m_places.m_placeRows.clear();
      for (std::size_t i = 0; i < 12; ++i) {
        const auto dir = rootDir.parent_path() / ("place-" + std::to_string(i));
        std::filesystem::create_directories(dir);
        placeDirs.push_back(dir);
        auto btn = std::make_unique<Button>();
        btn->setText("Place " + std::to_string(i));
        btn->setControlHeight(30.0F);
        btn->setMinHeight(30.0F);
        btn->setMaxHeight(30.0F);
        btn->setFillWidth(true);
        auto* btnPtr = static_cast<Button*>(placesScroll->content()->addChild(std::move(btn)));
        placeRowButtons.push_back(btnPtr);
        manager.m_places.m_placeRows.push_back(
            {btnPtr, {.label = "Place " + std::to_string(i), .identity = "saved:p" + std::to_string(i), .path = dir}}
        );
      }
      sidebarNode->addChild(std::move(places));
      sidebar = static_cast<Flex*>(root.addChild(std::move(sidebarNode)));

      adapter = std::make_unique<Adapter>(session.m_entries);
      auto gridNode = std::make_unique<VirtualGridView>();
      gridNode->setFlexGrow(1.0F);
      gridNode->setMinWidth(340.0F);
      gridNode->setAdapter(adapter.get());
      gridNode->setColumns(1);
      gridNode->setSquareCells(false);
      gridNode->setCellHeight(30.0F);
      gridNode->setRowGap(0.0F);
      gridNode->bindScrollState(&tab.scrollState);
      grid = static_cast<VirtualGridView*>(root.addChild(std::move(gridNode)));

      manager.m_places.m_scroll = placesScroll;
      manager.m_sidebar = sidebar;
      manager.m_grid = grid;
      manager.m_dispatcher.setSceneRoot(&root);
      if (!manager.m_surface)
        manager.m_surface = std::make_unique<ToplevelSurface>(manager.m_wayland);

      root.layout(renderer);
      TEST_CHECK(grid->scrollView().maxScrollOffset() > 100.0F);
      TEST_CHECK(placesScroll->maxScrollOffset() > 100.0F);
    }

    void detach(FileManager& manager) {
      manager.cancelFileGesture();
      manager.m_dispatcher.setSceneRoot(nullptr);
      if (grid != nullptr) {
        grid->bindScrollState(nullptr);
        grid->setAdapter(nullptr);
      }
      manager.m_grid = nullptr;
      manager.m_sidebar = nullptr;
      manager.m_places.m_placeRows.clear();
      manager.m_places.m_scroll = nullptr;
      while (!root.children().empty())
        root.removeChild(root.children().front().get());
      sidebar = nullptr;
      placesScroll = nullptr;
      grid = nullptr;
      placeRowButtons.clear();
    }
  };

public:
  static void waitReady(FileManager& manager, const std::filesystem::path& dir) {
    manager.navigate(dir, false);
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canAcceptDrop; });
    manager.activeSession().closeMonitor();
  }

  static void runFileViewEdgeAutoscroll(FileManager& manager, const std::filesystem::path& rootDir) {
    DragScene scene;
    scene.attach(manager, rootDir);
    const auto cleanup = nythera::test::makeScopeExit([&] { scene.detach(manager); });

    auto* wlSurface = manager.m_surface->wlSurface();
    constexpr auto kCopyMove = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

    // 1. Top edge at scrollOffset == 0 must NOT start the autoscroll timer.
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 10.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isProtocolDragInside());
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    TEST_CHECK(!manager.m_transfers.applyDragScroll());

    // 2. Bottom edge starts repeating timer, clamps delta to 18px/frame, and advances scroll offset.
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    tickDragScrollTimer(4);
    TEST_CHECK(std::abs(manager.m_transfers.m_dragScrollDelta - 18.0F) < 0.001F);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    scene.root.layout(scene.renderer);
    TEST_CHECK(scene.root.width() > 0.0F);
    TEST_CHECK(std::abs(scene.grid->scrollView().scrollOffset() - 18.0F) < 0.001F);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());

    // 3. Scroll down under a stationary pointer until folder-6 (y=180..210) and folder-7 (y=210..240) pass under y=105.
    // Pointer at y=105 with scrollOffset=90 hits content y=195 -> index 6 (folder-6).
    scene.grid->scrollView().setScrollOffset(90.0F);
    scene.root.layout(scene.renderer);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());
    TEST_CHECK(manager.dropDestination(260.0F, 105.0F, true) == rootDir / "folder-6");
    TEST_CHECK(scene.grid->indexAtSurface(260.0F, 105.0F) == 6);

    // Tick and apply one autoscroll step past y=210 (offset 90 + 18 = 108 -> content y=213 -> index 7 folder-7).
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 105.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    tickDragScrollTimer(3);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    scene.root.layout(scene.renderer);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());
    TEST_CHECK(std::abs(scene.grid->scrollView().scrollOffset() - 108.0F) < 0.001F);
    TEST_CHECK(manager.dropDestination(260.0F, 105.0F, true) == rootDir / "folder-7");
    TEST_CHECK(scene.grid->indexAtSurface(260.0F, 105.0F) == 7);

    // 4. Center dead zone stops timer and resets pending delta.
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 60.0F, kCopyMove);
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta == 0.0F);

    // 5. Top edge while scrolled down accumulates negative delta and scrolls up.
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 10.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    tickDragScrollTimer(1);
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta < 0.0F);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    scene.root.layout(scene.renderer);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());
    TEST_CHECK(scene.grid->scrollView().scrollOffset() < 108.0F);

    // 6. Clamping at bottom boundary automatically stops the autoscroll timer.
    const float maxOffset = scene.grid->scrollView().maxScrollOffset();
    scene.grid->scrollView().setScrollOffset(maxOffset - 3.0F);
    scene.root.layout(scene.renderer);
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    tickDragScrollTimer(1);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    scene.root.layout(scene.renderer);
    TEST_CHECK(std::abs(scene.grid->scrollView().scrollOffset() - maxOffset) < 0.001F);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());

    // 7. Simulated animated scroll change between frames triggers drop-target refresh.
    scene.grid->scrollView().setScrollOffset(90.0F);
    scene.root.layout(scene.renderer);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());
    TEST_CHECK(manager.dropDestination(260.0F, 105.0F, true) == rootDir / "folder-6");
    // Second call with no scroll change returns false.
    TEST_CHECK(!manager.m_transfers.refreshDropTargetAfterScroll());
  }

  static void runPlacesSidebarAndSwitching(FileManager& manager, const std::filesystem::path& rootDir) {
    DragScene scene;
    scene.attach(manager, rootDir);
    const auto cleanup = nythera::test::makeScopeExit([&] { scene.detach(manager); });

    auto* wlSurface = manager.m_surface->wlSurface();
    constexpr auto kCopyMove = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

    // 1. Places sidebar bottom edge autoscroll activates and updates drop destination as rows scroll in.
    manager.m_transfers.updateProtocolDrag(wlSurface, 90.0F, 105.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    TEST_CHECK(manager.m_transfers.m_dragScrollInPlaces);
    TEST_CHECK(manager.dropDestination(90.0F, 105.0F, true) == scene.placeDirs[3]);
    TEST_CHECK(manager.m_transfers.m_dropPlace == scene.placeRowButtons[3]);

    tickDragScrollTimer(2);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    scene.root.layout(scene.renderer);
    TEST_CHECK(scene.placesScroll->scrollOffset() > 0.0F);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());
    // Scroll further so place-7 (y=210..240) lands under y=105 (scrollOffset = 120 -> content y = 225 -> index 7).
    scene.placesScroll->setScrollOffset(120.0F);
    scene.root.layout(scene.renderer);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());
    TEST_CHECK(manager.dropDestination(90.0F, 105.0F, true) == scene.placeDirs[7]);
    TEST_CHECK(manager.m_places.rowIndexAt(90.0F, 105.0F) == 7);
    // Confirm scrolled-off row 0 is NOT selectable at negative coordinates / outside viewport.
    TEST_CHECK(!manager.m_places.rowIndexAt(90.0F, -10.0F).has_value());

    // 2. Places sidebar top edge scrolls back up.
    manager.m_transfers.updateProtocolDrag(wlSurface, 90.0F, 10.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    tickDragScrollTimer(1);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    scene.root.layout(scene.renderer);
    TEST_CHECK(scene.placesScroll->scrollOffset() < 120.0F);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());

    // 3. Switching pointer from grid edge to Places edge resets accumulated scroll delta.
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove);
    TEST_CHECK(!manager.m_transfers.m_dragScrollInPlaces);
    tickDragScrollTimer(2);
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta > 0.0F);
    // Move to Places center zone (not near edge -> speed 0) resets delta and switches m_dragScrollInPlaces.
    manager.m_transfers.updateProtocolDrag(wlSurface, 90.0F, 60.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.m_dragScrollInPlaces);
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta == 0.0F);
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());

    // Accumulate grid bottom edge delta, then switch immediately to Places top edge -> resets delta cleanly.
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove);
    tickDragScrollTimer(2);
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta > 0.0F);
    manager.m_transfers.updateProtocolDrag(wlSurface, 90.0F, 10.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.m_dragScrollInPlaces);
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta == 0.0F);

    // 4. Hidden Places sidebar suppresses drag autoscroll.
    scene.sidebar->setVisible(false);
    scene.root.layout(scene.renderer);
    manager.m_transfers.updateProtocolDrag(wlSurface, 90.0F, 10.0F, kCopyMove);
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta == 0.0F);
    scene.sidebar->setVisible(true);
    scene.root.layout(scene.renderer);
  }

  static void runWheelScrollingDuringDrag(FileManager& manager, const std::filesystem::path& rootDir) {
    DragScene scene;
    scene.attach(manager, rootDir);
    const auto cleanup = nythera::test::makeScopeExit([&] { scene.detach(manager); });

    auto* wlSurface = manager.m_surface->wlSurface();
    const float initialScale = manager.m_listHeader.viewScale();

    // 1. External protocol drag + Ctrl+wheel over file grid scrolls instead of zooming.
    constexpr auto kCopyMove = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 60.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isProtocolDragInside());
    const float gridBefore = scene.grid->scrollView().scrollOffset();
    manager.m_keyboardModifiers = KeyMod::Ctrl;
    manager.handlePointer({
        .type = PointerEvent::Type::Axis,
        .surface = wlSurface,
        .sx = 260.0F,
        .sy = 60.0F,
        .axis = WL_POINTER_AXIS_VERTICAL_SCROLL,
        .axisValue = 15.0,
        .axisDiscrete = 1,
        .axisValue120 = 120,
        .axisLines = 1.0F,
    });
    scene.root.layout(scene.renderer);
    TEST_CHECK(scene.grid->scrollView().scrollOffset() > gridBefore);
    TEST_CHECK(std::abs(manager.m_listHeader.viewScale() - initialScale) < 0.0001F);

    // External protocol drag moving to Places sidebar immediately re-targets wheel events to Places.
    manager.m_transfers.updateProtocolDrag(wlSurface, 90.0F, 60.0F, kCopyMove);
    const float placesBefore = scene.placesScroll->scrollOffset();
    manager.handlePointer({
        .type = PointerEvent::Type::Axis,
        .surface = wlSurface,
        .sx = 90.0F,
        .sy = 60.0F,
        .axis = WL_POINTER_AXIS_VERTICAL_SCROLL,
        .axisValue = 15.0,
        .axisDiscrete = 1,
        .axisValue120 = 120,
        .axisLines = 1.0F,
    });
    scene.root.layout(scene.renderer);
    TEST_CHECK(scene.placesScroll->scrollOffset() > placesBefore);
    manager.m_keyboardModifiers = 0;
    manager.cancelFileGesture();

    // 2. Local fallback internal drag: press on item 0, drag past threshold, Ctrl+wheel scrolls & updates dropIndex.
    scene.grid->scrollView().setScrollOffset(0.0F);
    scene.root.layout(scene.renderer);
    auto& tab = manager.activeTab();
    tab.selectedPaths = {(rootDir / "file-0.txt").string()};
    tab.selectedPath = rootDir / "file-0.txt";
    manager.m_transfers.m_dropValidationHook = [](const std::vector<std::string>& sources,
                                                  const std::filesystem::path& dest, auto done) {
      done(
          FileTransferController::DropValidation{
              .sources = sources, .rawDestination = dest, .target = dest, .copyByDefault = false
          }
      );
      return true;
    };
    const auto clearValidationHook =
        nythera::test::makeScopeExit([&] { manager.m_transfers.m_dropValidationHook = {}; });

    manager.handlePointer({
        .type = PointerEvent::Type::Button,
        .surface = wlSurface,
        .sx = 260.0F,
        .sy = 15.0F,
        .button = BTN_LEFT,
        .pressed = true,
    });
    TEST_CHECK(manager.m_transfers.isDragPending());
    manager.handlePointer({
        .type = PointerEvent::Type::Motion,
        .surface = wlSurface,
        .sx = 260.0F,
        .sy = 60.0F,
    });
    TEST_CHECK(manager.m_transfers.isDraggingFiles());
    TEST_CHECK(!manager.m_transfer.dragging());

    // Scroll down via Ctrl+wheel during local drag until folder-6 (y=180..210) moves under y=60 (scroll ~135..145).
    manager.m_keyboardModifiers = KeyMod::Ctrl;
    for (int i = 0; i < 3; ++i) {
      manager.handlePointer({
          .type = PointerEvent::Type::Axis,
          .surface = wlSurface,
          .sx = 260.0F,
          .sy = 60.0F,
          .axis = WL_POINTER_AXIS_VERTICAL_SCROLL,
          .axisValue = 15.0,
          .axisDiscrete = 1,
          .axisValue120 = 120,
          .axisLines = 1.0F,
      });
      scene.root.layout(scene.renderer);
    }
    TEST_CHECK(std::abs(manager.m_listHeader.viewScale() - initialScale) < 0.0001F);
    TEST_CHECK(scene.grid->scrollView().scrollOffset() > 120.0F);
    // Scroll offset is 3 * 56 = 168 -> at y=60, content y = 228 -> index 7 (folder-7).
    TEST_CHECK(manager.m_transfers.dropIndex() == 7);
    manager.m_keyboardModifiers = 0;

    // Local drag edge autoscroll near bottom edge also works.
    manager.handlePointer({
        .type = PointerEvent::Type::Motion,
        .surface = wlSurface,
        .sx = 260.0F,
        .sy = 110.0F,
    });
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    tickDragScrollTimer(1);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    scene.root.layout(scene.renderer);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());

    // Releasing mouse button drops/ends drag and cleans up scroll timer & state.
    manager.handlePointer({
        .type = PointerEvent::Type::Button,
        .surface = wlSurface,
        .sx = 260.0F,
        .sy = 110.0F,
        .button = BTN_LEFT,
        .pressed = false,
    });
    TEST_CHECK(!manager.m_transfers.isDraggingFiles());
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    TEST_CHECK(manager.m_transfers.m_dragScrollDelta == 0.0F);
    TEST_CHECK(!manager.m_transfers.m_refreshDropAfterLayout);
  }

  static void runLifecycleCleanup(FileManager& manager, const std::filesystem::path& rootDir) {
    DragScene scene;
    scene.attach(manager, rootDir);
    const auto cleanup = nythera::test::makeScopeExit([&] { scene.detach(manager); });

    auto* wlSurface = manager.m_surface->wlSurface();
    constexpr auto kCopyMove = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

    auto armScrollTimer = [&] {
      manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove);
      TEST_CHECK(manager.m_transfers.isDragScrollActive());
      tickDragScrollTimer(1);
      TEST_CHECK(manager.m_transfers.m_dragScrollDelta != 0.0F);
    };
    auto assertCleaned = [&] {
      TEST_CHECK(!manager.m_transfers.isDragScrollActive());
      TEST_CHECK(manager.m_transfers.m_dragScrollDelta == 0.0F);
      TEST_CHECK(!manager.m_transfers.m_refreshDropAfterLayout);
    };

    // 1. Protocol drop cleanup.
    armScrollTimer();
    manager.m_transfers.m_dragTargetValid = true;
    manager.m_transfers.m_protocolDropDestination = rootDir;
    (void)manager.m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    assertCleaned();

    // 2. Pointer leave cleanup.
    armScrollTimer();
    manager.m_transfers.handlePointerLeaveDuringDrag();
    assertCleaned();

    // 3. cancelFileGesture cleanup.
    armScrollTimer();
    manager.cancelFileGesture();
    assertCleaned();

    // 4. cancelLocalDragState cleanup.
    armScrollTimer();
    manager.m_transfers.cancelLocalDragState();
    assertCleaned();
    manager.cancelFileGesture();

    // 5. Navigation cleanup (onBeforePathChange on active tab).
    const auto subDir = rootDir / "folder-6";
    armScrollTimer();
    manager.navigate(subDir, false);
    assertCleaned();
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canAcceptDrop; });
    manager.activeSession().closeMonitor();
    scene.grid->setAdapter(nullptr);
    scene.detach(manager);
    scene.attach(manager, subDir, 16);

    // 6. Tab switch cleanup.
    const auto secondTab = manager.m_tabs.openTab(rootDir, manager.activeTab().viewState, false);
    TEST_CHECK(secondTab != kInvalidTabId);
    armScrollTimer();
    TEST_CHECK(manager.m_tabs.activateTab(secondTab));
    assertCleaned();
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canAcceptDrop; });
    manager.activeSession().closeMonitor();
    TEST_CHECK(manager.m_tabs.closeTab(secondTab));
    waitReady(manager, rootDir);

    // 7. UI detach & shutdown cleanup on a dedicated instance (shutdown sets m_alive=false).
    FileManager transient(rootDir.string());
    waitReady(transient, rootDir);
    DragScene transientScene;
    transientScene.attach(transient, rootDir);
    const auto transientCleanup = nythera::test::makeScopeExit([&] { transientScene.detach(transient); });
    auto* transientSurface = transient.m_surface->wlSurface();
    transient.m_transfers.updateProtocolDrag(transientSurface, 260.0F, 110.0F, kCopyMove);
    TEST_CHECK(transient.m_transfers.isDragScrollActive());
    tickDragScrollTimer(1);
    transient.m_transfers.detachUi();
    TEST_CHECK(!transient.m_transfers.isDragScrollActive());
    TEST_CHECK(transient.m_transfers.m_dragScrollDelta == 0.0F);
    TEST_CHECK(!transient.m_transfers.m_refreshDropAfterLayout);

    transient.m_transfers.updateProtocolDrag(transientSurface, 260.0F, 110.0F, kCopyMove);
    TEST_CHECK(transient.m_transfers.isDragScrollActive());
    tickDragScrollTimer(1);
    transient.m_transfers.shutdown();
    TEST_CHECK(!transient.m_transfers.isDragScrollActive());
    TEST_CHECK(transient.m_transfers.m_dragScrollDelta == 0.0F);
    TEST_CHECK(!transient.m_transfers.m_refreshDropAfterLayout);
  }

  static void runRestrictionsAndAdminMode(FileManager& manager, const std::filesystem::path& rootDir) {
    DragScene scene;
    scene.attach(manager, rootDir);
    const auto cleanup = nythera::test::makeScopeExit([&] { scene.detach(manager); });

    auto* wlSurface = manager.m_surface->wlSurface();
    constexpr auto kCopyMove = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    auto& tab = manager.activeTab();
    auto& session = tab.session;

    // 1. Wastebin suppresses drag autoscroll.
    session.m_browsingTrash = true;
    TEST_CHECK(manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove) == 0);
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    session.m_browsingTrash = false;

    // 2. Loading state suppresses drag autoscroll.
    session.m_loading = true;
    TEST_CHECK(manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove) == 0);
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    session.m_loading = false;

    // 3. Inline rename state suppresses drag autoscroll.
    manager.m_renamePath = rootDir / "file-0.txt";
    TEST_CHECK(manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove) == 0);
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    manager.m_renamePath.reset();

    // 4. Read-only / inactive Administrator Mode suppresses drag autoscroll.
    tab.administratorBrowsing = true;
    TEST_CHECK(!tab.capabilities().canAcceptDrop);
    TEST_CHECK(manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove) == 0);
    TEST_CHECK(!manager.m_transfers.isDragScrollActive());
    tab.administratorBrowsing = false;

    // 5. Active writable Administrator Mode allows drag autoscroll & wheel scroll while preserving Admin routing.
    scene.detach(manager);
    InProcessHelperFixture helper;
    manager.m_tabs.setAdminTransportLauncherForTesting(helper.makeLauncher());
    TEST_CHECK(manager.enterAdministratorMode(tab.id));
    pumpUntil([&] { return tab.hasActiveAdminSession() && !tab.session.loading(); });
    DragScene adminScene;
    adminScene.attach(manager, rootDir);
    const auto adminCleanup = nythera::test::makeScopeExit([&] { adminScene.detach(manager); });
    TEST_CHECK(tab.administratorBrowsing);
    TEST_CHECK(tab.capabilities().canAcceptDrop);

    manager.m_transfers.updateProtocolDrag(wlSurface, 260.0F, 110.0F, kCopyMove);
    TEST_CHECK(manager.m_transfers.isDragScrollActive());
    tickDragScrollTimer(1);
    TEST_CHECK(manager.m_transfers.applyDragScroll());
    adminScene.root.layout(adminScene.renderer);
    TEST_CHECK(adminScene.grid->scrollView().scrollOffset() > 0.0F);
    TEST_CHECK(manager.m_transfers.refreshDropTargetAfterScroll());

    // Admin Mode drop validation still runs via adminSession.statInfoAsync.
    const auto adminTarget = rootDir / "folder-6";
    const auto adminSrc = rootDir / "file-0.txt";
    std::optional<FileTransferController::DropValidation> validated;
    manager.m_transfers.validateDropAsync(
        tab.id, {adminSrc.string()}, adminTarget, tab.session.navigationGeneration(),
        manager.m_transfers.m_dragSessionGeneration,
        [&](FileTransferController::DropValidation result) { validated = std::move(result); }
    );
    pumpUntil([&] { return validated.has_value(); });
    TEST_CHECK(validated->target == adminTarget);

    manager.cancelFileGesture();
    tab.adminSession.stopSession(true);
    tab.leaveAdministratorMode();
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-drag-scroll"});
    const auto filesDir = env.work() / "files";
    std::filesystem::create_directory(filesDir);
    FileManager manager(filesDir.string());
    FileManagerTestAccess::waitReady(manager, filesDir);

    FileManagerTestAccess::runFileViewEdgeAutoscroll(manager, filesDir);
    FileManagerTestAccess::runPlacesSidebarAndSwitching(manager, filesDir);
    FileManagerTestAccess::runWheelScrollingDuringDrag(manager, filesDir);
    FileManagerTestAccess::runLifecycleCleanup(manager, filesDir);
    FileManagerTestAccess::runRestrictionsAndAdminMode(manager, filesDir);
  });
}
