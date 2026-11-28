#include "filemanager/settings/settings_window.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "ui/settings_layout.h"
#include "ui/style.h"
#include "ui/tooltip/tooltip_manager.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace filemanager {

  namespace {
    constexpr float kSidebarWidth = 210.0F;
    constexpr float kResizeEdgeThickness = 6.0F;
    constexpr float kCornerResizeSize = 12.0F;

    constexpr std::array<std::pair<std::uint32_t, wp_cursor_shape_device_v1_shape>, 8> kResizeEdges{{
        {XDG_TOPLEVEL_RESIZE_EDGE_TOP, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE},
        {XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE},
        {XDG_TOPLEVEL_RESIZE_EDGE_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE},
        {XDG_TOPLEVEL_RESIZE_EDGE_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE},
        {XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE},
        {XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE},
        {XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE},
        {XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE},
    }};
  } // namespace

  SettingsWindow::SettingsWindow(
      WaylandConnection& wayland, RenderContext& renderContext, ConfigService& shellConfig,
      FileManagerSettings& settings, Callbacks callbacks
  )
      : m_wayland(wayland), m_renderContext(renderContext), m_shellConfig(shellConfig), m_settings(settings),
        m_callbacks(std::move(callbacks)) {}

  SettingsWindow::~SettingsWindow() {
    *m_alive = false;
    close();
  }

  wl_surface* SettingsWindow::wlSurface() const noexcept { return m_surface ? m_surface->wlSurface() : nullptr; }

  bool SettingsWindow::ownsSurface(wl_surface* surface) const noexcept {
    if (surface == nullptr)
      return false;
    if (surface == wlSurface())
      return true;
    return m_dropdown && m_dropdown->isSelectDropdownOpen() && m_dropdown->wlSurface() == surface;
  }

  bool SettingsWindow::open(ToplevelSurface* parent) {
    if (m_surface) {
      m_wayland.activateSurface(m_surface->wlSurface());
      return true;
    }
    if (const auto saved = m_settings.loadSettingsWindowSize()) {
      m_preferredWidth = saved->first;
      m_preferredHeight = saved->second;
    }
    m_modifiedOnly = false;
    m_confirmingPageReset = false;
    m_confirmingGlobalReset = false;
    m_contentDirty = true;

    m_surface = std::make_unique<ToplevelSurface>(m_wayland);
    m_surface->setRenderContext(&m_renderContext);
    m_surface->setAnimationManager(&m_animations);
    m_surface->setHazeProfile("settings");
    m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
      if (m_surface)
        m_surface->requestLayout();
    });
    m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
    m_surface->setClosedCallback([this] { deferClose(); });

    const auto parentOutput = parent ? m_wayland.outputForSurface(parent->wlSurface()) : nullptr;
    const auto foundOutput = std::ranges::find(m_wayland.outputs(), parentOutput, &WaylandOutput::output);
    const auto* outputInfo = foundOutput != m_wayland.outputs().end() ? &*foundOutput
        : m_wayland.outputs().empty()                                 ? nullptr
                                                                      : &m_wayland.outputs().front();
    wl_output* output = outputInfo ? outputInfo->output : nullptr;
    const auto outputWidth = outputInfo && outputInfo->logicalWidth > 0 ? outputInfo->logicalWidth : 1280;
    const auto outputHeight = outputInfo && outputInfo->logicalHeight > 0 ? outputInfo->logicalHeight : 800;
    const auto minWidth = static_cast<std::uint32_t>(std::min(640, outputWidth));
    const auto minHeight = static_cast<std::uint32_t>(std::min(420, outputHeight));
    ToplevelSurfaceConfig config{
        .width = std::clamp(m_preferredWidth, minWidth, static_cast<std::uint32_t>(outputWidth)),
        .height = std::clamp(m_preferredHeight, minHeight, static_cast<std::uint32_t>(outputHeight)),
        .minWidth = minWidth,
        .minHeight = minHeight,
        .parent = parent ? parent->xdgToplevel() : nullptr,
        .title = "Preferences — Nythera Files",
        .appId = "dev.nythera.FileManager",
    };
    if (!m_surface->initialize(output, config)) {
      m_surface.reset();
      return false;
    }
    m_wayland.stopKeyRepeat();
    m_dropdown = std::make_unique<SelectDropdownPopup>(m_wayland, m_renderContext);
    m_dropdown->setShadowConfig(m_shellConfig.config().shell.shadow);
    m_dropdown->setParent(m_surface->xdgSurface(), m_surface->wlSurface(), output);
    m_input.setTextInputContext(m_surface->wlSurface(), m_wayland.textInputService());
    m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_wayland.setCursorShape(serial, static_cast<wp_cursor_shape_device_v1_shape>(shape));
    });
    m_input.setHoverChangeCallback([this](InputArea*, InputArea* next) {
      if (m_surface) {
        TooltipManager::instance().onHoverChange(
            next, m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
        );
      }
    });
    m_input.setFocusChangeCallback([this](InputArea*, InputArea* next) {
      if (next != nullptr && m_contentScroll != nullptr) {
        if (auto* scroll = findEnclosingScrollView(next))
          scrollNodeIntoScrollView(*scroll, nullptr, *next, Style::spaceSm);
      }
    });
    m_surface->requestLayout();
    return true;
  }

  void SettingsWindow::deferClose() {
    struct CloseRequest {
      SettingsWindow* self;
      std::weak_ptr<bool> alive;
    };
    g_idle_add(
        [](gpointer data) -> gboolean {
          std::unique_ptr<CloseRequest> request(static_cast<CloseRequest*>(data));
          const auto locked = request->alive.lock();
          if (locked && *locked)
            request->self->close();
          return G_SOURCE_REMOVE;
        },
        new CloseRequest{this, m_alive}
    );
  }

  void SettingsWindow::close() {
    ++m_openGeneration;
    m_thumbnailCacheUsage.clear();
    if (m_surface && m_surface->width() >= 640 && m_surface->height() >= 420)
      (void)m_settings.saveSettingsWindowSize(m_surface->width(), m_surface->height());
    if (m_dropdown)
      m_dropdown->closeSelectDropdown();
    m_dropdown.reset();
    m_input.setTextInputContext(nullptr, nullptr);
    m_input.setSceneRoot(nullptr);
    m_background = nullptr;
    m_sidebar = nullptr;
    m_sidebarNav = nullptr;
    m_contentScroll = nullptr;
    m_moveHandle = nullptr;
    m_headerRow = nullptr;
    m_modifiedFilter = nullptr;
    m_statusBanner = nullptr;
    m_statusMessageLabel = nullptr;
    m_resizeEdges.fill(nullptr);
    m_root.reset();
    if (m_surface) {
      m_surface->setSceneRoot(nullptr);
      m_surface.reset();
    }
  }

  void SettingsWindow::requestRebuild() {
    m_contentDirty = true;
    if (m_surface)
      m_surface->requestLayout();
  }

  void SettingsWindow::selectCategory(SettingsCategory category) {
    if (m_selectedCategory == category)
      return;
    m_selectedCategory = category;
    m_contentScrollState.offset = 0.0F;
    m_confirmingPageReset = false;
    requestRebuild();
  }

  void SettingsWindow::updateTheme() {
    if (!m_surface)
      return;
    if (m_dropdown)
      m_dropdown->setShadowConfig(m_shellConfig.config().shell.shadow);
    requestRebuild();
  }

  void SettingsWindow::onPreferencesReloaded() {
    if (m_suppressExternalReload)
      return;
    m_confirmingPageReset = false;
    m_confirmingGlobalReset = false;
    requestRebuild();
  }

  bool SettingsWindow::mutatePreferences(const std::function<bool(FileManagerSettings&)>& fn) {
    const auto previous = m_settings.preferences();
    m_suppressExternalReload = true;
    const bool ok = fn(m_settings);
    m_suppressExternalReload = false;
    if (ok) {
      if (m_statusIsError)
        dismissStatus();
      if (m_callbacks.onPreferencesChanged && m_settings.preferences() != previous)
        m_callbacks.onPreferencesChanged(previous);
      requestRebuild();
      return true;
    }
    const auto message = m_settings.lastSaveError().empty() ? std::string("Could not save preference change")
                                                            : m_settings.lastSaveError();
    showStatus(message, true);
    requestRebuild();
    return false;
  }

  bool SettingsWindow::mutateSearchPreferences(const std::function<bool(FileManagerSettings&)>& fn) {
    const auto previous = m_settings.searchSettings();
    m_suppressExternalReload = true;
    const bool ok = fn(m_settings);
    m_suppressExternalReload = false;
    if (ok) {
      if (m_statusIsError)
        dismissStatus();
      if (m_callbacks.onSearchSettingsChanged && m_settings.searchSettings() != previous)
        m_callbacks.onSearchSettingsChanged(previous);
      requestRebuild();
      return true;
    }
    const auto message = m_settings.lastSaveError().empty() ? std::string("Could not save search preference")
                                                            : m_settings.lastSaveError();
    showStatus(message, true);
    requestRebuild();
    return false;
  }

  bool SettingsWindow::currentPageHasOverrides() const {
    const auto keys = categoryResettableKeys(m_selectedCategory);
    return std::ranges::any_of(keys, [this](const auto& path) { return m_settings.hasPreferenceKey(path); });
  }

  bool SettingsWindow::anySettingsHasOverrides() const {
    const auto keys = allResettablePreferenceKeys();
    return std::ranges::any_of(keys, [this](const auto& path) { return m_settings.hasPreferenceKey(path); });
  }

  void SettingsWindow::resetCurrentPage() {
    if (!m_confirmingPageReset) {
      m_confirmingPageReset = true;
      requestRebuild();
      return;
    }
    m_confirmingPageReset = false;
    const auto keys = categoryResettableKeys(m_selectedCategory);
    if (mutatePreferences([&keys](FileManagerSettings& settings) { return settings.resetKeys(keys); })) {
      showStatus(
          std::string("Reset ") + std::string(categoryDescriptor(m_selectedCategory).label) + " to defaults", false
      );
    }
  }

  void SettingsWindow::resetAllPreferences() {
    if (!m_confirmingGlobalReset) {
      m_confirmingGlobalReset = true;
      requestRebuild();
      return;
    }
    m_confirmingGlobalReset = false;
    const auto keys = allResettablePreferenceKeys();
    if (mutatePreferences([&keys](FileManagerSettings& settings) { return settings.resetKeys(keys); })) {
      showStatus("Reset all preferences to defaults", false);
    }
  }

  void SettingsWindow::showStatus(std::string message, bool isError) {
    m_statusMessage = std::move(message);
    m_statusIsError = isError;
    if (m_statusBanner != nullptr && m_statusMessageLabel != nullptr) {
      ui::updateSettingsStatusBanner(*m_statusBanner, *m_statusMessageLabel, m_statusMessage, m_statusIsError);
    }
    if (m_surface)
      m_surface->requestLayout();
  }

  void SettingsWindow::dismissStatus() {
    m_statusMessage.clear();
    m_statusIsError = false;
    if (m_statusBanner != nullptr && m_statusMessageLabel != nullptr)
      ui::updateSettingsStatusBanner(*m_statusBanner, *m_statusMessageLabel, "", false);
    if (m_surface)
      m_surface->requestLayout();
  }

  SettingsPageContext SettingsWindow::makePageContext() {
    return {
        .settings = m_settings,
        .scale = 1.0F,
        .modifiedOnly = m_modifiedOnly,
        .mutate = [this](std::function<bool(FileManagerSettings&)> fn) { return mutatePreferences(fn); },
        .mutateSearch = [this](std::function<bool(FileManagerSettings&)> fn) { return mutateSearchPreferences(fn); },
        .manageCustomSearches = m_callbacks.onManageCustomSearches,
        .editCustomSearch = m_callbacks.onEditCustomSearch,
        .inspectThumbnailCache =
            [this] {
              if (!m_callbacks.onInspectThumbnailCache)
                return;
              m_callbacks.onInspectThumbnailCache([this, alive = std::weak_ptr<bool>(m_alive),
                                                   generation = m_openGeneration](std::string message) {
                const auto life = alive.lock();
                if (!life || !*life || generation != m_openGeneration || !isOpen())
                  return;
                m_thumbnailCacheUsage = std::move(message);
                requestRebuild();
              });
            },
        .thumbnailCacheUsage = m_thumbnailCacheUsage,
        .clearThumbnailCache = m_callbacks.onClearThumbnailCache,
        .chooseFile =
            [this](FileDialogOptions options, auto callback) {
              const std::weak_ptr<bool> alive = m_alive;
              if (!FileDialog::open(std::move(options), [this, alive, callback = std::move(callback)](auto result) {
                    const auto life = alive.lock();
                    if (life && *life && isOpen() && result)
                      callback(*result);
                  }))
                showStatus("Could not open the file picker", true);
            },
        .exportTheme = m_callbacks.onExportTheme,
        .pickColor =
            [this](std::string title, Color initial, std::function<void(Color)> apply) {
              if (!m_callbacks.onPickColor)
                return;
              const std::weak_ptr<bool> alive = m_alive;
              m_callbacks.onPickColor(std::move(title), initial, [this, alive, apply = std::move(apply)](Color color) {
                const auto life = alive.lock();
                if (life && *life && isOpen())
                  apply(color);
              });
            },
        .clearRecentHistory = m_callbacks.onClearRecentHistory,
        .showStatus = [this](std::string msg, bool err) { showStatus(std::move(msg), err); },
    };
  }

  void SettingsWindow::addResizeEdges(Flex& root) {
    for (std::size_t i = 0; i < kResizeEdges.size(); ++i) {
      const auto [edge, cursor] = kResizeEdges[i];
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setAcceptedButtons(BTN_LEFT);
      area->setCursorShape(cursor);
      area->setPropagateEvents(false);
      area->setOnPress([this, edge](const InputArea::PointerData& data) {
        if (data.pressed
            && m_surface
            && m_surface->xdgToplevel() != nullptr
            && m_wayland.seat() != nullptr
            && data.button == BTN_LEFT)
          xdg_toplevel_resize(m_surface->xdgToplevel(), m_wayland.seat(), data.serial, edge);
      });
      m_resizeEdges[i] = static_cast<InputArea*>(root.addChild(std::move(area)));
    }
  }

  void SettingsWindow::layoutResizeEdges(float width, float height) {
    const float e = kResizeEdgeThickness;
    const float c = kCornerResizeSize;
    const std::array<LayoutRect, 8> rects{{
        {c, 0.0F, std::max(0.0F, width - 2.0F * c), e},
        {c, std::max(0.0F, height - e), std::max(0.0F, width - 2.0F * c), e},
        {0.0F, c, e, std::max(0.0F, height - 2.0F * c)},
        {std::max(0.0F, width - e), c, e, std::max(0.0F, height - 2.0F * c)},
        {0.0F, 0.0F, c, c},
        {std::max(0.0F, width - c), 0.0F, c, c},
        {0.0F, std::max(0.0F, height - c), c, c},
        {std::max(0.0F, width - c), std::max(0.0F, height - c), c, c},
    }};
    for (std::size_t i = 0; i < m_resizeEdges.size(); ++i) {
      if (m_resizeEdges[i] != nullptr) {
        m_resizeEdges[i]->setPosition(rects[i].x, rects[i].y);
        m_resizeEdges[i]->setSize(rects[i].width, rects[i].height);
      }
    }
  }

  void SettingsWindow::buildSceneForTesting(Renderer& renderer, std::uint32_t width, std::uint32_t height) {
    buildScene(renderer, width, height);
  }

  void SettingsWindow::buildScene(Renderer& renderer, std::uint32_t width, std::uint32_t height) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    constexpr float scale = 1.0F;

    if (m_dropdown)
      m_dropdown->closeSelectDropdown();
    auto focus = m_input.captureTabFocus();
    m_input.setSceneRoot(nullptr);
    m_background = nullptr;
    m_sidebar = nullptr;
    m_sidebarNav = nullptr;
    m_contentScroll = nullptr;
    m_moveHandle = nullptr;
    m_headerRow = nullptr;
    m_modifiedFilter = nullptr;
    m_statusBanner = nullptr;
    m_statusMessageLabel = nullptr;
    m_resizeEdges.fill(nullptr);

    auto root = std::make_unique<Flex>();
    root->setDirection(FlexDirection::Horizontal);
    root->setAlign(FlexAlign::Stretch);
    root->setSize(w, h);
    root->setAnimationManager(&m_animations);
    if (m_dropdown)
      root->setPopupContext(m_dropdown.get());

    const auto mode = m_settings.transparencyMode();
    const float bgOpacity = detachedPanelBackgroundOpacityForTransparencyMode(mode);
    root->setFill(colorSpecFromRole(ColorRole::Surface, bgOpacity));
    root->setSurfaceBackground(mode != PanelTransparencyMode::Solid);
    root->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    root->setRadius(Style::scaledRadiusMd(scale));

    // Full-height left sidebar
    auto sidebar = ui::column({
        .out = &m_sidebar,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow, panelCardOpacityForTransparencyMode(mode, bgOpacity)),
        .minWidth = kSidebarWidth * scale,
        .maxWidth = kSidebarWidth * scale,
        .fillHeight = true,
        .width = kSidebarWidth * scale,
    });
    sidebar->addChild(
        ui::label({
            .text = "Preferences",
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    auto sidebarScroll = ui::scrollView({
        .state = &m_sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .flexGrow = 1.0F,
        .configure = [](ScrollView& scroll) {
          scroll.clearFill();
          scroll.clearBorder();
        },
    });
    auto nav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .scrollIntoView = {},
        .syncIndexFromSelection = {},
    });
    nav->setGap(2.0F * scale);
    nav->setTabFocusKey("settings-categories");
    m_sidebarNav = nav.get();
    for (const auto& descriptor : kSettingsCategories) {
      const bool selected = descriptor.category == m_selectedCategory;
      auto onClick = [this, category = descriptor.category] { selectCategory(category); };
      auto button =
          ui::makeSettingsSidebarNavButton(descriptor.glyph, std::string(descriptor.label), scale, selected, onClick);
      nav->registerItem(button.get(), std::move(onClick));
      nav->addChild(std::move(button));
    }
    sidebarScroll->content()->setDirection(FlexDirection::Vertical);
    sidebarScroll->content()->setAlign(FlexAlign::Stretch);
    sidebarScroll->content()->addChild(std::move(nav));
    sidebar->addChild(std::move(sidebarScroll));

    const bool hasAnyOverrides = anySettingsHasOverrides();
    sidebar->addChild(std::make_unique<Separator>());
    sidebar->addChild(
        ui::button({
            .text = m_confirmingGlobalReset ? "Confirm Reset All" : "Reset All Preferences",
            .glyph = "refresh",
            .fontSize = Style::fontSizeCaption * scale,
            .glyphSize = Style::fontSizeCaption * scale,
            .enabled = hasAnyOverrides,
            .variant = m_confirmingGlobalReset ? ButtonVariant::Destructive : ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceSm * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [this] { resetAllPreferences(); },
        })
    );
    root->addChild(std::move(sidebar));

    auto sidebarSeparator = std::make_unique<Separator>();
    sidebarSeparator->setOrientation(SeparatorOrientation::VerticalRule);
    root->addChild(std::move(sidebarSeparator));

    // Right content column: header stops at the sidebar boundary
    auto contentCol = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .fillHeight = true,
        .flexGrow = 1.0F,
    });

    const auto& currentDesc = categoryDescriptor(m_selectedCategory);
    auto headerRow = ui::row(
        {
            .out = &m_headerRow,
            .align = FlexAlign::Center,
            .gap = Style::spaceSm * scale,
        },
        ui::glyph({
            .glyph = std::string(currentDesc.glyph),
            .glyphSize = Style::fontSizeHeader * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        }),
        ui::label({
            .text = std::string(currentDesc.label),
            .fontSize = Style::fontSizeHeader * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::Primary),
            .flexGrow = 1.0F,
        })
    );
    headerRow->addChild(
        ui::row(
            {.out = &m_modifiedFilter, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
            ui::label({
                .text = "Modified",
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            }),
            ui::toggle({
                .checked = m_modifiedOnly,
                .scale = scale,
                .onChange =
                    [this](bool checked) {
                      m_modifiedOnly = checked;
                      m_confirmingPageReset = false;
                      m_confirmingGlobalReset = false;
                      m_contentScrollState.offset = 0.0F;
                      requestRebuild();
                    },
                .configure = [](Toggle& toggle) { toggle.setTabFocusKey("preferences:modified-only"); },
            })
        )
    );
    if (currentPageHasOverrides()) {
      headerRow->addChild(
          ui::button({
              .text = m_confirmingPageReset ? "Confirm Reset Page" : "Reset Page",
              .fontSize = Style::fontSizeCaption * scale,
              .variant = m_confirmingPageReset ? ButtonVariant::Destructive : ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceSm * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this] { resetCurrentPage(); },
          })
      );
    }
    headerRow->addChild(
        ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Ghost,
            .tooltip = "Close Preferences",
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [this] { deferClose(); },
        })
    );
    auto moveHandle = std::make_unique<InputArea>();
    moveHandle->setParticipatesInLayout(false);
    moveHandle->setAcceptedButtons(BTN_LEFT);
    moveHandle->setPropagateEvents(true);
    moveHandle->setOnPress([this](const InputArea::PointerData& data) {
      if (m_surface && data.pressed && data.button == BTN_LEFT)
        m_surface->beginMove(data.serial);
    });
    m_moveHandle = static_cast<InputArea*>(headerRow->addChild(std::move(moveHandle)));
    contentCol->addChild(std::move(headerRow));

    contentCol->addChild(
        ui::makeSettingsStatusBanner({
            .message = m_statusMessage,
            .error = m_statusIsError,
            .scale = scale,
            .onDismiss = [this] { dismissStatus(); },
            .out = &m_statusBanner,
            .messageOut = &m_statusMessageLabel,
        })
    );

    auto contentScroll = ui::scrollView({
        .out = &m_contentScroll,
        .state = &m_contentScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .flexGrow = 1.0F,
        .configure = [](ScrollView& scroll) {
          scroll.clearFill();
          scroll.clearBorder();
        },
    });
    auto* pageBody = contentScroll->content();
    pageBody->setDirection(FlexDirection::Vertical);
    pageBody->setAlign(FlexAlign::Stretch);
    pageBody->setGap(Style::spaceMd * scale);

    const auto ctx = makePageContext();
    switch (m_selectedCategory) {
    case SettingsCategory::General:
      buildGeneralSettingsPage(*pageBody, ctx);
      break;
    case SettingsCategory::Appearance:
      buildAppearanceSettingsPage(*pageBody, ctx);
      break;
    case SettingsCategory::Navigation:
      buildNavigationSettingsPage(*pageBody, ctx);
      break;
    case SettingsCategory::FilesAndFolders:
      buildFilesFoldersSettingsPage(*pageBody, ctx);
      break;
    case SettingsCategory::Search:
      buildSearchSettingsPage(*pageBody, ctx);
      break;
    case SettingsCategory::Integrations:
      buildIntegrationsSettingsPage(*pageBody, ctx);
      break;
    case SettingsCategory::Shortcuts:
      buildShortcutsSettingsPage(*pageBody, ctx);
      break;
    case SettingsCategory::Advanced:
      buildAdvancedSettingsPage(*pageBody, ctx);
      break;
    }

    if (m_modifiedOnly) {
      // Page builders append cards whose last child is the settings body.
      for (std::size_t i = pageBody->children().size(); i > 0; --i) {
        auto* card = pageBody->children()[i - 1].get();
        if (card->children().back()->children().empty())
          pageBody->removeChild(card);
      }
      if (pageBody->children().empty())
        pageBody->addChild(
            ui::label({
                .text = "No modified preferences on this page.",
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        );
    }

    contentCol->addChild(std::move(contentScroll));
    root->addChild(std::move(contentCol));

    addResizeEdges(*root);

    layoutScene(*root, renderer, w, h);
    m_root = std::move(root);
    m_input.setSceneRoot(m_root.get());
    m_input.restoreTabFocus(std::move(focus));
    if (m_surface)
      m_surface->setSceneRoot(m_root.get());
    m_contentDirty = false;
  }

  void SettingsWindow::layoutScene(Flex& root, Renderer& renderer, float width, float height) {
    root.arrange(renderer, {0.0F, 0.0F, width, height});
    if (m_moveHandle && m_headerRow) {
      m_moveHandle->setPosition(0.0F, 0.0F);
      m_moveHandle->setSize(m_modifiedFilter->x(), m_headerRow->height());
    }
    layoutResizeEdges(width, height);
  }

  void SettingsWindow::updateScene(Renderer& renderer, std::uint32_t width, std::uint32_t height) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    // The dispatcher must keep the same input areas across frames during a press.
    if (m_contentDirty || !m_root)
      buildScene(renderer, width, height);
    else
      layoutScene(*m_root, renderer, static_cast<float>(width), static_cast<float>(height));
  }

  void SettingsWindow::prepareFrame(bool update, bool layout) {
    if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0)
      return;
    m_renderContext.makeCurrent(m_surface->renderTarget());
    auto& renderer = m_surface->renderTarget().renderer();
    if (layout || update || m_contentDirty)
      updateScene(renderer, m_surface->width(), m_surface->height());
  }

  bool SettingsWindow::handlePointer(const PointerEvent& event) {
    if (m_dropdown && m_dropdown->onPointerEvent(event))
      return true;
    if (!m_surface || event.surface != m_surface->wlSurface())
      return false;
    const float x = static_cast<float>(event.sx);
    const float y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_input.pointerEnter(x, y, event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_input.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_input.pointerMotion(x, y, event.serial);
      break;
    case PointerEvent::Type::Button:
      m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      break;
    case PointerEvent::Type::Axis:
      m_input.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      break;
    }
    m_surface->requestUpdate();
    return true;
  }

  void SettingsWindow::handleKeyboard(const KeyboardEvent& event) {
    if (m_dropdown && m_dropdown->isSelectDropdownOpen()) {
      m_dropdown->onKeyboardEvent(event);
      return;
    }
    if (event.pressed
        && !event.repeat
        && event.sym == XKB_KEY_Escape
        && (event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super)) == 0U) {
      if (m_confirmingPageReset || m_confirmingGlobalReset) {
        m_confirmingPageReset = false;
        m_confirmingGlobalReset = false;
        requestRebuild();
        return;
      }
      deferClose();
      return;
    }
    m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
    if (m_surface)
      m_surface->requestUpdate();
  }

} // namespace filemanager
