#include "filemanager/dialogs/modal_dialog_host.h"

#include "config/config_types.h"
#include "core/files/display_text.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/dialogs/conflict_dialog_controller.h"
#include "filemanager/dialogs/symlink_dialog_controller.h"
#include "filemanager/file_manager_settings.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/controls/separator.h"
#include "ui/dialogs/dialog_background_artwork.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "ui/style.h"
#include "ui/tooltip/tooltip_manager.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <utility>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace filemanager {

  namespace {
    // SVG alpha masks use the existing Image tint and scale handling.
    constexpr char kDeleteWarning[] = R"SVG(data:image/svg+xml,
<svg xmlns="http://www.w3.org/2000/svg" width="72" height="64" viewBox="0 0 72 64">
  <path fill="none" stroke="white" stroke-width="3.6" stroke-linejoin="bevel"
        d="M34.3 5 H33 L13.3 41.4 M11.5 44.5 L4.9 56.6 7 59 H32
           M35.5 59 H38 M41.5 59 H65 L67.1 56.6 54.8 34
           M53.2 31 L51.8 28.4 M50.2 25.4 L48.8 22.8 M47.2 19.8 L39 5 H37.7"/>
  <path fill="white" d="M32 20 H40 L38 42 H34 Z M33 46 H39 V52 H33 Z"/>
</svg>)SVG";

  } // namespace

  ModalDialogHost::ModalDialogHost(
      WaylandConnection& wayland, RenderContext& renderContext, FileManagerSettings& settings, Callbacks callbacks
  )
      : m_wayland(wayland), m_renderContext(renderContext), m_settings(settings), m_callbacks(std::move(callbacks)) {
    m_conflictController = std::make_unique<ConflictDialogController>(*this, m_callbacks, m_alive);
    m_symlinkController = std::make_unique<SymlinkDialogController>(*this);
    m_dispatcher.setFocusChangeCallback([](InputArea*, InputArea* next) {
      if (next == nullptr)
        return;
      if (auto* scroll = findEnclosingScrollView(next))
        scrollNodeIntoScrollView(*scroll, nullptr, *next, Style::spaceSm);
    });
  }

  ModalDialogHost::~ModalDialogHost() {
    *m_alive = false;
    forceCloseImmediate();
  }

  std::string ModalDialogHost::sanitizeConflictDisplayText(std::string_view text) {
    return sanitizeFilenameForDisplay(text);
  }

  void ModalDialogHost::forceCloseImmediate() {
    if (const auto source = std::exchange(m_closeSource, 0U))
      g_source_remove(source);
    m_response = {};
    if (auto cancel = std::exchange(m_cancelArchivePassword, nullptr))
      cancel();
    if (hoverTooltipsEnabled())
      TooltipManager::instance().forceDestroy();
    savePendingDialogSize();
    cancelInitialLayout();
    m_dialogHeightAnimation.cancelAll();
    m_configuredDialogSize.reset();
    m_wayland.stopKeyRepeat();
    m_dispatcher.setTextInputContext(nullptr, nullptr);
    if (m_selectDropdown)
      m_selectDropdown->closeSelectDropdown();
    if (m_customContent && m_customContent->onDismiss)
      m_customContent->onDismiss();
    m_customContent.reset();
    m_dispatcher.setSceneRoot(nullptr);
    if (m_surface)
      m_surface->setSceneRoot(nullptr);
    m_cards.clear();
    m_deleteWarning = nullptr;
    m_topAccent = nullptr;
    m_cornerAccent = nullptr;
    m_root.reset();
    m_selectDropdown.reset();
    m_surface.reset();
    m_modalHint.reset();
    m_promptInput = nullptr;
    m_closeQueued = false;
    m_conflictController->cancelPresentation();
  }

  void ModalDialogHost::requestLayout() {
    if (m_surface)
      m_surface->requestLayout();
    else if (m_pendingPresentation && !m_closeQueued && m_initialLayoutSource == 0)
      m_initialLayoutSource = g_idle_add(
          [](gpointer data) -> gboolean {
            auto& host = *static_cast<ModalDialogHost*>(data);
            host.m_initialLayoutSource = 0;
            host.prepareInitialLayout();
            return G_SOURCE_REMOVE;
          },
          this
      );
  }

  void ModalDialogHost::cancelInitialLayout() {
    if (const auto source = std::exchange(m_initialLayoutSource, 0U))
      g_source_remove(source);
    m_pendingPresentation.reset();
  }

  void ModalDialogHost::setFocus(InputArea* area) { m_dispatcher.setFocus(area); }

  void ModalDialogHost::registerCard(Flex* card) {
    if (card != nullptr)
      m_cards.push_back(card);
  }

  void ModalDialogHost::textPrompt(
      std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
  ) {
    if (m_surface) {
      response(std::nullopt);
      return;
    }
    m_passwordPrompt = password;
    showDialog(
        std::move(title), std::move(message), {{"Cancel", 0, DialogChoiceStyle::Cancel}, {"Continue", 1}},
        [this, response = std::move(response)](int choice) mutable {
          auto value = m_promptValue;
          m_promptValue.clear();
          m_passwordPrompt = false;
          if (choice == 1)
            response(std::move(value));
          else
            response(std::nullopt);
        },
        nullptr, std::nullopt, DialogStyle::TextPrompt
    );
  }

  std::function<std::optional<std::string>(const std::string&, bool, GCancellable*)>
  ModalDialogHost::makeArchivePasswordResolver() {
    return [this, alive = std::weak_ptr<bool>(m_alive), mutex = m_conflictController->serialMutex()](
               const std::string& source, bool retry, GCancellable* cancellable
           ) -> std::optional<std::string> {
      const auto active = [&] {
        const auto life = alive.lock();
        return life && *life && !g_cancellable_is_cancelled(cancellable);
      };
      std::unique_lock lock(*mutex, std::defer_lock);
      while (active() && !lock.try_lock_for(std::chrono::milliseconds(20))) {
      }
      if (!active())
        return std::nullopt;
      struct Prompt {
        ModalDialogHost* host;
        std::weak_ptr<bool> alive;
        std::shared_ptr<GCancellable> cancellable;
        std::promise<std::optional<std::string>> reply;
        std::string name;
        bool retry;
        bool shown = false;
        bool finished = false;
      };
      auto pending = std::make_shared<Prompt>(Prompt{
          this,
          alive,
          {G_CANCELLABLE(g_object_ref(cancellable)), g_object_unref},
          {},
          sanitizeFilenameForDisplay(std::filesystem::path(source).filename().native()),
          retry
      });
      auto result = pending->reply.get_future();
      g_timeout_add_full(
          G_PRIORITY_DEFAULT, 20,
          [](gpointer data) -> gboolean {
            const auto request = *static_cast<std::shared_ptr<Prompt>*>(data);
            auto* self = request->host;
            const auto life = request->alive.lock();
            if (!life || !*life || request->finished)
              return G_SOURCE_REMOVE;
            if (g_cancellable_is_cancelled(request->cancellable.get())) {
              if (request->shown && self->m_style == DialogStyle::TextPrompt && self->isOpen())
                self->closeDialog(0);
              return G_SOURCE_REMOVE;
            }
            if (request->shown || self->isOpen())
              return G_SOURCE_CONTINUE;
            request->shown = true;
            if (self->m_callbacks.isMainWindowOpen && !self->m_callbacks.isMainWindowOpen()) {
              self->m_backgroundArchivePasswordPending = true;
              if (self->m_callbacks.syncProgressWindow)
                self->m_callbacks.syncProgressWindow();
            }
            self->m_cancelArchivePassword = [request] {
              request->finished = true;
              request->host->m_passwordPrompt = false;
              request->host->m_backgroundArchivePasswordPending = false;
              request->reply.set_value(std::nullopt);
            };
            self->textPrompt(
                "Archive password for " + request->name,
                request->retry ? "The password was rejected or the encrypted data is damaged. Try again."
                               : "Enter the password for this archive.",
                true, [request](std::optional<std::string> password) {
                  if (request->finished)
                    return;
                  request->finished = true;
                  auto* host = request->host;
                  host->m_cancelArchivePassword = {};
                  host->m_backgroundArchivePasswordPending = false;
                  if (host->m_callbacks.syncProgressWindow)
                    host->m_callbacks.syncProgressWindow();
                  request->reply.set_value(std::move(password));
                }
            );
            return G_SOURCE_CONTINUE;
          },
          new std::shared_ptr<Prompt>(pending),
          [](gpointer data) { delete static_cast<std::shared_ptr<Prompt>*>(data); }
      );
      while (active()) {
        if (result.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready) {
          auto password = result.get();
          if (!password || !password->empty())
            return password;
          // An empty confirmation is treated as dismissal by the archive backend.
          return std::nullopt;
        }
      }
      return std::nullopt;
    };
  }

  bool ModalDialogHost::backgroundConflictPending() const noexcept {
    return m_backgroundArchivePasswordPending || m_conflictController->backgroundPending();
  }

  FileOperationManager::ConflictResolver ModalDialogHost::makeConflictResolver(ConflictPathAccess pathAccess) {
    return m_conflictController->makeResolver(pathAccess);
  }

  void ModalDialogHost::showSymlinkDialog(
      std::function<std::optional<SymlinkError>(const std::string&, const std::string&)> onSubmit,
      std::function<void(const std::string&)> onBrowseTarget, std::function<void()> onDismiss
  ) {
    m_symlinkController->show(std::move(onSubmit), std::move(onBrowseTarget), std::move(onDismiss));
  }

  void ModalDialogHost::setSymlinkTargetPath(std::string_view path) { m_symlinkController->setTargetPath(path); }

  void ModalDialogHost::showSymlinkTargetError(std::string_view message) {
    m_symlinkController->showTargetError(message);
  }

  std::uint32_t ModalDialogHost::initialExtent(
      std::uint32_t saved, std::uint32_t fallback, std::uint32_t minimum, std::uint32_t available
  ) {
    return std::clamp(saved > 0 ? saved : fallback, std::min(minimum, available), available);
  }

  void ModalDialogHost::recordConfiguredSize(std::uint32_t width, std::uint32_t height) {
    if (!m_resizable || width == 0 || height == 0)
      return;
    const DialogSize size{width, height};
    const auto previous = std::exchange(m_configuredDialogSize, size);
    if (m_normalDialogSize) {
      if (m_applyingDialogSize || m_compactDialogHeight || m_dialogHeightAnimation.hasActive())
        return;
      if (std::exchange(*m_normalDialogSize, size) == size)
        return;
    }
    if (m_customContent && !m_customContent->sizeStateKey.empty() && previous && *previous != size) {
      m_pendingDialogSize = size;
      m_dialogSizeSave.start(std::chrono::milliseconds(250), [this] { savePendingDialogSize(); });
    }
  }

  std::uint32_t ModalDialogHost::normalHeight() const noexcept {
    return m_normalDialogSize ? m_normalDialogSize->second : 0U;
  }

  std::optional<std::uint32_t> ModalDialogHost::compactExtent(std::optional<float> height) const {
    const auto normal = normalHeight();
    return height
        ? std::optional{static_cast<std::uint32_t>(std::ceil(
              std::clamp(*height, static_cast<float>(std::min(m_dialogMinHeight, normal)), static_cast<float>(normal))
          ))}
        : std::nullopt;
  }

  void ModalDialogHost::setCompactHeight(std::optional<float> height) {
    if (!m_surface || !m_normalDialogSize || m_closeQueued)
      return;
    const auto [width, normalHeight] = *m_normalDialogSize;
    const auto compact = compactExtent(height);
    if (compact == m_compactDialogHeight)
      return;
    m_compactDialogHeight = compact;
    m_dialogHeightAnimation.cancelAll();
    const auto target = compact.value_or(normalHeight);
    if (m_surface->height() == target && m_surface->width() == width)
      return;
    m_dialogHeightAnimation.animate(
        static_cast<float>(m_surface->height()), static_cast<float>(target), 175.0F, Easing::EaseInOutCubic,
        [this, width](float value) {
          m_applyingDialogSize = true;
          m_surface->resizeClient(width, static_cast<std::uint32_t>(std::lround(value)));
          m_applyingDialogSize = false;
        }
    );
    m_surface->requestFrameTick();
  }

  void ModalDialogHost::savePendingDialogSize() {
    m_dialogSizeSave.stop();
    if (const auto size = std::exchange(m_pendingDialogSize, std::nullopt)) {
      if (!m_settings.saveDialogSize(m_sizeStateKey, size->first, size->second) && m_callbacks.setStatus)
        m_callbacks.setStatus("Unable to save dialog size");
    }
  }

  void ModalDialogHost::showDialog(
      std::string title, std::string message, std::vector<DialogChoice> choices, std::function<void(int)> response,
      ToplevelSurface* parent, std::optional<CustomContent> customContent, DialogStyle style
  ) {
    if (isOpen() || (!m_wayland.isConnected() && !m_presentToplevelForTest)) {
      if (response)
        response(-1);
      return;
    }
    m_customContent = std::move(customContent);
    if (parent == nullptr && m_callbacks.defaultParent)
      parent = m_callbacks.defaultParent();
    m_title = std::move(title);
    m_style = style;
    m_resizable = true;
    m_resizeEdges.fill(nullptr);
    m_moveHandle = nullptr;
    m_titleRow = nullptr;
    m_titleLabel = nullptr;
    m_spacer = nullptr;
    m_footer = nullptr;
    m_divider = nullptr;
    m_preferredWidth = 0;
    m_preferredHeight = 0;
    m_configuredDialogSize.reset();
    m_normalDialogSize.reset();
    m_compactDialogHeight.reset();
    m_sizeStateKey = m_customContent && !m_customContent->sizeStateKey.empty()
        ? m_customContent->sizeStateKey
        : FileManagerSettings::dialogSizeStateKey(m_title, style == DialogStyle::Conflict);
    if (m_resizable && (!m_customContent || m_customContent->rememberSize)) {
      if (const auto saved = m_settings.loadDialogSize(m_sizeStateKey)) {
        m_preferredWidth = saved->first;
        m_preferredHeight = saved->second;
      }
    }
    m_message = std::move(message);
    m_choices = std::move(choices);
    m_promptValue.clear();
    m_promptInput = nullptr;
    m_response = std::move(response);
    m_closeQueued = false;
    const bool usePreferredInitialHeight = m_customContent && m_customContent->usePreferredInitialHeight;
    const auto& outputs = m_wayland.outputs();
    const auto* parentOutput = parent ? m_wayland.outputForSurface(parent->wlSurface()) : nullptr;
    const auto onParent = std::ranges::find(outputs, parentOutput, &WaylandOutput::output);
    const auto* outputInfo = onParent != outputs.end() ? &*onParent : outputs.empty() ? nullptr : &outputs.front();
    const auto displayLimit = [&](int logical, int margin, int legacyLimit) {
      if (logical <= 0)
        return usePreferredInitialHeight ? UINT32_MAX : static_cast<std::uint32_t>(legacyLimit);
      const int available = std::max(1, logical - margin);
      return static_cast<std::uint32_t>(usePreferredInitialHeight ? available : std::min(legacyLimit, available));
    };
    const auto maxWidth = displayLimit(outputInfo ? outputInfo->effectiveLogicalWidth() : 0, 40, 720);
    const auto maxHeight = displayLimit(outputInfo ? outputInfo->effectiveLogicalHeight() : 0, 80, 900);
    const auto requestedMinWidth = (m_customContent && m_customContent->minWidth > 0)
        ? m_customContent->minWidth
        : (style == DialogStyle::Conflict ? 520U : 420U);
    const auto requestedMinHeight = (m_customContent && m_customContent->minHeight > 0)
        ? m_customContent->minHeight
        : (style == DialogStyle::Conflict ? 260U : 180U);
    const auto minWidth = std::min(requestedMinWidth, maxWidth);
    const auto defaultWidth = m_customContent && m_customContent->preferredWidth > 0 ? m_customContent->preferredWidth
        : (style == DialogStyle::Conflict || style == DialogStyle::TextPrompt)       ? 720U
                                                                                     : 520U;
    const auto width = usePreferredInitialHeight ? initialExtent(m_preferredWidth, defaultWidth, minWidth, maxWidth)
        : m_preferredWidth > 0                   ? std::max(m_preferredWidth, minWidth)
                                                 : std::min(maxWidth, defaultWidth);
    const auto preferredHeight = usePreferredInitialHeight
        ? initialExtent(m_preferredHeight, m_customContent->preferredHeight, requestedMinHeight, maxHeight)
        : 0U;
    if (m_customContent && m_customContent->compactHeightMode) {
      m_normalDialogSize = DialogSize{width, preferredHeight};
      m_dialogMinHeight = std::min(requestedMinHeight, maxHeight);
    }
    buildDialog(static_cast<float>(width));
    if (style == DialogStyle::Conflict)
      wrapDialogBodyForScrolling();
    if (m_customContent && m_customContent->onInitialLayout) {
      m_pendingPresentation = PendingPresentation{
          parent,
          ToplevelSurfaceConfig{
              .width = width,
              .height = preferredHeight,
              .minWidth = minWidth,
              .minHeight = std::min(requestedMinHeight, maxHeight)
          }
      };
      requestLayout();
      return;
    }
    if (m_presentToplevelForTest && (style != DialogStyle::Conflict || !m_initialLayoutRendererForTest)) {
      presentDialog(parent, ToplevelSurfaceConfig{.width = width, .height = preferredHeight});
      return;
    }
    ScaledRenderer measureRenderer(m_renderContext, outputInfo == nullptr ? 1.0F : outputInfo->configuredScale());
    LayoutConstraints constraints;
    constraints.setExactWidth(static_cast<float>(width));
    const auto measured = m_root->measure(
        m_initialLayoutRendererForTest ? *m_initialLayoutRendererForTest : measureRenderer, constraints
    );
    const auto measuredHeight =
        std::clamp(static_cast<std::uint32_t>(std::ceil(measured.height)), std::min(180U, maxHeight), maxHeight);
    const auto minHeight = m_customContent && m_customContent->useMeasuredMinHeight
        ? measuredHeight
        : std::min(requestedMinHeight, maxHeight);
    const auto defaultHeight = m_customContent && m_customContent->preferredHeight > 0
        ? std::clamp(std::min(measuredHeight, m_customContent->preferredHeight), minHeight, maxHeight)
        : measuredHeight;
    const auto savedHeight = std::max(m_preferredHeight, minHeight);
    const auto height = usePreferredInitialHeight ? preferredHeight
        : m_preferredHeight == 0                  ? defaultHeight
        : style == DialogStyle::Conflict          ? std::min(savedHeight, std::max(defaultHeight, minHeight))
                                                  : savedHeight;
    if (style != DialogStyle::Conflict
        && measured.height > static_cast<float>(height)
        && !(m_customContent && m_customContent->fillContent)) {
      wrapDialogBodyForScrolling();
    }
    ToplevelSurfaceConfig config{
        .width = width,
        .height = height,
        .minWidth = minWidth,
        .minHeight = minHeight,
        .maxWidth = m_resizable ? 0U : width,
        .maxHeight = m_resizable ? 0U : height,
    };
    presentDialog(parent, config);
  }

  void ModalDialogHost::wrapDialogBodyForScrolling() {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    const auto focus = m_dispatcher.captureTabFocus();
    m_dispatcher.setSceneRoot(nullptr);
    auto footer = m_root->removeChild(m_footer);
    auto spacer = m_root->removeChild(m_spacer);
    m_spacer->setFlexGrow(0.0F);
    auto scroll = std::make_unique<ScrollView>();
    scroll->setFlexGrow(1.0F);
    if (m_style == DialogStyle::Conflict) {
      scroll->setViewportPaddingH(0.0F);
      scroll->setViewportPaddingV(0.0F);
    }
    scroll->content()->setDirection(FlexDirection::Vertical);
    scroll->content()->setAlign(FlexAlign::Stretch);
    scroll->content()->setGap(10.0F);
    if (m_style == DialogStyle::Warning) {
      const auto& children = m_root->children();
      const auto content =
          std::ranges::find_if(children, [](const auto& child) { return child->participatesInLayout(); });
      const auto index = static_cast<std::size_t>(content - children.begin());
      scroll->content()->addChild(m_root->removeChild(content->get()));
      m_root->insertChildAt(index, std::move(scroll));
    } else {
      const auto& children = m_root->children();
      const auto content =
          std::ranges::find_if(children, [](const auto& child) { return child->participatesInLayout(); });
      const auto scrollIndex = static_cast<std::size_t>(content - children.begin());
      auto index = scrollIndex;
      while (index < m_root->children().size()) {
        auto* child = m_root->children()[index].get();
        if (child->participatesInLayout())
          scroll->content()->addChild(m_root->removeChild(child));
        else
          ++index;
      }
      m_root->insertChildAt(scrollIndex, std::move(scroll));
    }
    m_root->addChild(std::move(spacer));
    m_root->addChild(std::move(footer));
    m_dispatcher.setSceneRoot(m_root.get());
    m_dispatcher.restoreTabFocus(focus);
  }

  void ModalDialogHost::prepareInitialLayout() {
    if (!m_pendingPresentation || m_closeQueued)
      return;
    auto [parent, config] = *m_pendingPresentation;
    auto* renderer = m_initialLayoutRendererForTest;
    if (!renderer) {
      if (!parent || !m_renderContext.makeCurrent(parent->renderTarget())) {
        closeDialog(-1);
        return;
      }
      renderer = &parent->renderTarget().renderer();
    }
    std::optional<float> compactHeight;
    {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      m_root->setSize(static_cast<float>(config.width), static_cast<float>(config.height));
      m_root->layout(*renderer);
      if (!m_customContent->onInitialLayout(*renderer, static_cast<float>(config.height), compactHeight))
        return;
    }
    m_compactDialogHeight = compactExtent(compactHeight);
    config.height = m_compactDialogHeight.value_or(config.height);
    m_pendingPresentation.reset();
    presentDialog(parent, config);
  }

  void ModalDialogHost::presentDialog(ToplevelSurface* parent, const ToplevelSurfaceConfig& requested) {
    auto surface = std::make_unique<ToplevelSurface>(m_wayland);
    surface->setRenderContext(&m_renderContext);
    surface->setConfigureCallback([this](std::uint32_t width, std::uint32_t height) {
      recordConfiguredSize(width, height);
      if (m_surface)
        m_surface->requestLayout();
    });
    surface->setClosedCallback([this] { closeDialog(-1); });
    surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
    if (m_normalDialogSize)
      surface->setAnimationManager(&m_dialogHeightAnimation);
    if (m_presentToplevelForTest) {
      m_surface = std::move(surface);
      m_presentToplevelForTest(*m_surface, parent, {requested.width, requested.height});
      return;
    }
    auto config = requested;
    config.parent = parent ? parent->xdgToplevel() : nullptr;
    config.title = m_title;
    config.appId = "dev.nythera.FileManager.Dialog";
    auto* output = parent ? m_wayland.outputForSurface(parent->wlSurface()) : nullptr;
    if (!output && !m_wayland.outputs().empty())
      output = m_wayland.outputs().front().output;
    if (surface->initialize(output, config)) {
      if (m_normalDialogSize && config.parent) {
        if (!m_modalManager)
          m_modalManager = ToplevelSurface::bindModalManager(m_wayland);
        m_modalHint = surface->createModalHint(m_modalManager->get());
      }
      m_wayland.stopKeyRepeat();
      m_surface = std::move(surface);
      m_surface->setSceneRoot(m_root.get());
      if (m_customContent && m_customContent->selectDropdowns) {
        m_selectDropdown = std::make_unique<SelectDropdownPopup>(m_wayland, m_renderContext);
        m_selectDropdown->setParent(m_surface->xdgSurface(), m_surface->wlSurface(), output);
        m_root->setPopupContext(m_selectDropdown.get());
      }
      m_dispatcher.setTextInputContext(m_surface->wlSurface(), m_wayland.textInputService());
    } else {
      auto failed = std::move(m_response);
      forceCloseImmediate();
      if (failed)
        failed(-1);
    }
  }

  void ModalDialogHost::setTitle(std::string title) {
    if (title == m_title)
      return;
    m_title = std::move(title);
    if (m_titleLabel)
      m_titleLabel->setText(m_title);
    if (m_surface)
      m_surface->setTitle(m_title);
    requestLayout();
  }

  void ModalDialogHost::closeDialog(int response) {
    if (m_closeQueued)
      return;
    if (m_customContent) {
      if (m_customContent->interceptResponse && m_customContent->interceptResponse(response))
        return;
      if (m_customContent->onBeforeClose)
        m_customContent->onBeforeClose();
    }
    if (hoverTooltipsEnabled())
      TooltipManager::instance().forceDestroy();
    m_closeQueued = true;
    if (m_selectDropdown)
      m_selectDropdown->closeSelectDropdown();
    m_wayland.stopKeyRepeat();
    m_dispatcher.setTextInputContext(nullptr, nullptr);
    struct DialogCloseRequest {
      ModalDialogHost* self;
      std::weak_ptr<bool> alive;
    };
    m_resultPending = response;
    m_closeSource = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer context) -> gboolean {
          const auto* request = static_cast<DialogCloseRequest*>(context);
          const auto locked = request->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          auto* self = request->self;
          self->m_closeSource = 0;
          self->cancelInitialLayout();
          auto callback = std::move(self->m_response);
          self->m_promptValue = self->m_promptInput == nullptr ? std::string{} : self->m_promptInput->value();
          const int result = self->m_resultPending;
          self->savePendingDialogSize();
          self->m_dialogHeightAnimation.cancelAll();
          if (self->m_resizable
              && (!self->m_customContent
                  || (self->m_customContent->rememberSize && self->m_customContent->sizeStateKey.empty()))
              && self->m_surface
              && self->m_surface->width() >= 360
              && self->m_surface->height() >= 180) {
            self->m_preferredWidth = self->m_surface->width();
            self->m_preferredHeight = self->m_surface->height();
            if (!self->m_settings.saveDialogSize(self->m_sizeStateKey, self->m_preferredWidth, self->m_preferredHeight)
                && self->m_callbacks.setStatus)
              self->m_callbacks.setStatus("Unable to save conflict dialog size");
          }
          // The manifest picker and symlink target picker can be xdg_popup children of this dialogue.
          if (self->m_customContent && self->m_customContent->onDismiss)
            self->m_customContent->onDismiss();
          self->m_customContent.reset();
          self->m_dispatcher.setSceneRoot(nullptr);
          if (self->m_surface)
            self->m_surface->setSceneRoot(nullptr);
          self->m_cards.clear();
          self->m_deleteWarning = nullptr;
          self->m_topAccent = nullptr;
          self->m_cornerAccent = nullptr;
          self->m_root.reset();
          self->m_selectDropdown.reset();
          self->m_surface.reset();
          self->m_modalHint.reset();
          self->m_promptInput = nullptr;
          self->m_closeQueued = false;
          if (callback)
            callback(result);
          return G_SOURCE_REMOVE;
        },
        new DialogCloseRequest{this, m_alive},
        [](gpointer context) { delete static_cast<DialogCloseRequest*>(context); }
    );
  }

  bool ModalDialogHost::ownsSurface(wl_surface* surface) const noexcept {
    return surface && (surface == wlSurface() || (m_selectDropdown && surface == m_selectDropdown->wlSurface()));
  }

  void ModalDialogHost::handlePointer(const PointerEvent& event) {
    if (!m_surface || m_closeQueued)
      return;
    if (m_selectDropdown && m_selectDropdown->isSelectDropdownOpen()) {
      if (m_selectDropdown->onPointerEvent(event))
        return;
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        m_selectDropdown->closeSelectDropdown();
        return;
      }
    }
    if (event.surface != m_surface->wlSurface())
      return;
    const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
    auto& tooltip = TooltipManager::instance();
    const bool tooltips = hoverTooltipsEnabled();
    if (tooltips
        && (event.type == PointerEvent::Type::Leave
            || event.type == PointerEvent::Type::Axis
            || (event.type == PointerEvent::Type::Button && event.pressed)))
      tooltip.forceDestroy();
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_dispatcher.pointerEnter(x, y, event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_dispatcher.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_dispatcher.pointerMotion(x, y, event.serial);
      break;
    case PointerEvent::Type::Button:
      if (event.button == BTN_LEFT)
        m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      break;
    case PointerEvent::Type::Axis:
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      break;
    }
    if (tooltips && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
      tooltip.setPointerPosition(x, y);
      tooltip.onHoverChange(
          m_dispatcher.hoveredArea(), m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
      );
    }
  }

  void ModalDialogHost::handleKeyboard(const KeyboardEvent& event) {
    if (m_closeQueued)
      return;
    if (m_pendingPresentation) {
      if (event.pressed
          && !event.repeat
          && event.modifiers == 0
          && (event.sym == XKB_KEY_Escape || event.sym == XKB_KEY_space))
        closeDialog(0);
      return;
    }
    if (!m_surface)
      return;
    if (m_selectDropdown && m_selectDropdown->isSelectDropdownOpen()) {
      m_selectDropdown->onKeyboardEvent(event);
      return;
    }
    if (!event.pressed) {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, false, event.preedit, false);
      return;
    }
    if (event.preedit) {
      if (!event.repeat)
        m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, true, true, false);
      return;
    }
    if (event.sym == XKB_KEY_Escape && event.modifiers == 0U && !event.repeat) {
      closeDialog(-1);
      return;
    }
    if (m_customContent && m_customContent->onKeyEvent && m_customContent->onKeyEvent(event))
      return;
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
  }

  void ModalDialogHost::buildDialog(float dialogWidth) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_cards.clear();
    m_deleteWarning = nullptr;
    m_topAccent = nullptr;
    m_cornerAccent = nullptr;
    m_root = std::make_unique<Flex>();
    m_root->setDirection(FlexDirection::Vertical);
    m_root->setAlign(FlexAlign::Stretch);
    m_root->setGap(10.0F);
    m_root->setPadding(16.0F);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    const bool compositorBorder = m_customContent && m_customContent->compositorBorder;
    if (!compositorBorder)
      m_root->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
    m_root->setRadius(Style::scaledRadiusMd(1.0F));
    if (m_customContent && m_customContent->roundedCorners)
      m_root->setCornerShapes({});
    if (compositorBorder)
      m_root->setClipChildren(true);
    if (m_style == DialogStyle::Warning) {
      auto topAccent = std::make_unique<DialogBackgroundArtwork>("top-left-diagonals.svg", 126.0F, 78.27F);
      m_topAccent = topAccent.get();
      m_root->addChild(std::move(topAccent));
      auto cornerAccent = std::make_unique<DialogBackgroundArtwork>("bottom-right-contour.svg", 78.0F, 46.0F);
      m_cornerAccent = cornerAccent.get();
      m_root->addChild(std::move(cornerAccent));

      auto content = std::make_unique<Flex>();
      content->setDirection(FlexDirection::Horizontal);
      content->setAlign(FlexAlign::Center);
      content->setGap(18.0F);
      // Keep the warning at its natural height. Only the footer spacer grows.
      auto warning = std::make_unique<Image>();
      warning->setHitTestVisible(false);
      warning->setForegroundTint(colorSpecFromRole(ColorRole::Error));
      warning->setFrameSize(72.0F, 64.0F);
      m_deleteWarning = warning.get();
      content->addChild(std::move(warning));
      auto text = std::make_unique<Flex>();
      text->setDirection(FlexDirection::Vertical);
      text->setAlign(FlexAlign::Stretch);
      text->setGap(7.0F);
      text->setFlexGrow(1.0F);
      auto heading = std::make_unique<Label>();
      heading->setText(m_title);
      heading->setFontSize(Style::fontSizeTitle);
      heading->setFontWeight(FontWeight::SemiBold);
      heading->setColor(colorSpecFromRole(ColorRole::Primary));
      heading->setMaxLines(2);
      text->addChild(std::move(heading));
      auto message = std::make_unique<Label>();
      message->setText(m_message);
      message->setMaxWidth(std::max(1.0F, dialogWidth - 122.0F));
      message->setMaxLines(6);
      text->addChild(std::move(message));
      if (m_resizable) {
        auto moveHandle = std::make_unique<InputArea>();
        moveHandle->setParticipatesInLayout(false);
        moveHandle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        moveHandle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
        moveHandle->setOnPress([this](const InputArea::PointerData& data) {
          if (data.pressed && m_surface)
            xdg_toplevel_move(m_surface->xdgToplevel(), m_wayland.seat(), data.serial);
        });
        m_moveHandle = moveHandle.get();
        m_titleRow = text.get();
        text->addChild(std::move(moveHandle));
      }
      content->addChild(std::move(text));
      m_root->addChild(std::move(content));
    } else {
      auto topAccent = std::make_unique<DialogBackgroundArtwork>("top-left-diagonals.svg", 126.0F, 78.27F);
      m_topAccent = topAccent.get();
      m_root->addChild(std::move(topAccent));
      auto cornerAccent = std::make_unique<DialogBackgroundArtwork>("bottom-right-contour.svg", 78.0F, 46.0F);
      m_cornerAccent = cornerAccent.get();
      m_root->addChild(std::move(cornerAccent));
      auto heading = std::make_unique<Label>();
      const bool headerClose = m_customContent && m_customContent->headerCloseButton;
      m_titleLabel = heading.get();
      heading->setText(m_title);
      heading->setUseMarkup(false);
      heading->setMaxWidth(dialogWidth - (headerClose ? 80.0F : 48.0F));
      heading->setMaxLines(headerClose ? 1 : 2);
      if (headerClose)
        heading->setEllipsize(TextEllipsize::End);
      heading->setFontWeight(FontWeight::SemiBold);
      heading->setColor(colorSpecFromRole(ColorRole::Primary));
      if (m_resizable || headerClose) {
        auto titleRow = std::make_unique<Flex>();
        titleRow->setDirection(FlexDirection::Horizontal);
        titleRow->setAlign(FlexAlign::Center);
        heading->setFlexGrow(1.0F);
        titleRow->addChild(std::move(heading));
        if (m_resizable) {
          auto moveHandle = std::make_unique<InputArea>();
          moveHandle->setParticipatesInLayout(false);
          moveHandle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
          moveHandle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
          moveHandle->setOnPress([this](const InputArea::PointerData& data) {
            if (data.pressed && m_surface)
              xdg_toplevel_move(m_surface->xdgToplevel(), m_wayland.seat(), data.serial);
          });
          m_moveHandle = moveHandle.get();
          titleRow->addChild(std::move(moveHandle));
        }
        if (headerClose) {
          titleRow->setGap(Style::spaceSm);
          auto close = std::make_unique<Button>();
          close->setGlyph("x");
          close->setVariant(ButtonVariant::Ghost);
          close->setControlHeight(Style::controlHeightSm);
          close->setTooltip("Close Quick Look");
          close->setOnClick([this] { closeDialog(0); });
          titleRow->addChild(std::move(close));
        }
        m_titleRow = titleRow.get();
        m_root->addChild(std::move(titleRow));
      } else
        m_root->addChild(std::move(heading));
    }
    if (m_style != DialogStyle::Warning && !m_message.empty()) {
      auto message = std::make_unique<Label>();
      message->setText(m_message);
      message->setMaxWidth(dialogWidth - 48.0F);
      message->setMaxLines(14);
      m_root->addChild(std::move(message));
    }
    if (m_style == DialogStyle::TextPrompt) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(8.0F);
      auto input = std::make_unique<Input>();
      m_promptInput = input.get();
      input->setPasswordMode(m_passwordPrompt);
      input->setControlHeight(34.0F);
      input->setFlexGrow(1.0F);
      input->setOnSubmit([this](const std::string&) { closeDialog(1); });
      input->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t mods) {
        if (mods == 0U && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
          closeDialog(1);
          return true;
        }
        return false;
      });
      row->addChild(std::move(input));
      m_root->addChild(std::move(row));
    }
    if (m_customContent && m_customContent->buildFields)
      m_customContent->buildFields(*m_root, dialogWidth);
    auto row = std::make_unique<Flex>();
    row->setAlign(FlexAlign::Center);
    row->setJustify(FlexJustify::End);
    row->setGap(8.0F);
    row->setWrap(true);
    InputArea* safeFocus = nullptr;
    for (const auto& [label, result, style] : m_choices) {
      auto button = std::make_unique<Button>();
      button->setText(label);
      button->setVariant(style == DialogChoiceStyle::Destructive ? ButtonVariant::Destructive : ButtonVariant::Outline);
      button->setOnClick([this, result] { closeDialog(result); });
      if (m_customContent && m_customContent->bindChoiceButton)
        m_customContent->bindChoiceButton(result, button.get());
      if (style == DialogChoiceStyle::Cancel)
        safeFocus = button->inputArea();
      row->addChild(std::move(button));
    }
    auto spacer = std::make_unique<Flex>();
    if (m_style == DialogStyle::Conflict || (m_customContent && m_customContent->fillContent)) {
      spacer->setFlexGrow(0.0F);
      spacer->setVisible(false);
      spacer->setParticipatesInLayout(false);
    } else {
      spacer->setFlexGrow(1.0F);
    }
    m_spacer = spacer.get();
    m_root->addChild(std::move(spacer));
    auto footer = std::make_unique<Flex>();
    footer->setDirection(FlexDirection::Vertical);
    footer->setAlign(FlexAlign::Stretch);
    footer->setGap(8.0F);
    auto divider = std::make_unique<Separator>();
    divider->setOrientation(SeparatorOrientation::HorizontalRule);
    divider->setColor(colorSpecFromRole(ColorRole::OutlineVariant));
    m_divider = footer->addChild(std::move(divider));
    footer->addChild(std::move(row));
    m_footer = footer.get();
    m_root->addChild(std::move(footer));
    if (m_resizable) {
      const std::array<std::uint32_t, 8> edges{
          XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
          XDG_TOPLEVEL_RESIZE_EDGE_LEFT,        XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
          XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,    XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
          XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
      };
      const std::array<std::uint32_t, 8> cursors{
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE
      };
      for (std::size_t i = 0; i < edges.size(); ++i) {
        auto edge = std::make_unique<InputArea>();
        edge->setParticipatesInLayout(false);
        edge->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        edge->setCursorShape(cursors[i]);
        edge->setOnPress([this, side = edges[i]](const InputArea::PointerData& data) {
          if (data.pressed && m_surface && !m_compactDialogHeight && !m_dialogHeightAnimation.hasActive())
            xdg_toplevel_resize(m_surface->xdgToplevel(), m_wayland.seat(), data.serial, side);
        });
        m_resizeEdges[i] = edge.get();
        m_root->addChild(std::move(edge));
      }
      m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_wayland.setCursorShape(serial, shape);
      });
    }
    if (m_customContent && m_customContent->afterBuild)
      m_customContent->afterBuild();
    updateTheme();
    m_dispatcher.setSceneRoot(m_root.get());
    if (m_customContent && m_customContent->initialFocus && m_customContent->initialFocus() != nullptr)
      m_dispatcher.setFocus(m_customContent->initialFocus());
    else if (m_promptInput)
      m_dispatcher.setFocus(m_promptInput->inputArea());
    else if (safeFocus != nullptr)
      m_dispatcher.setFocus(safeFocus);
    else if (auto* first = m_dispatcher.firstTabFocusUnder(m_root.get()))
      m_dispatcher.setFocus(first);
  }

  void ModalDialogHost::updateTheme() {
    if (!m_root)
      return;
    const auto mode = m_settings.transparencyMode();
    const float background = detachedPanelBackgroundOpacityForTransparencyMode(mode);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, background));
    m_root->setSurfaceBackground(mode != PanelTransparencyMode::Solid);
    for (auto* card : m_cards)
      card->setFill(
          colorSpecFromRole(ColorRole::SurfaceContainer, panelCardOpacityForTransparencyMode(mode, background))
      );
    if (m_customContent && m_customContent->onThemeUpdated)
      m_customContent->onThemeUpdated();
  }

  void ModalDialogHost::prepareFrame(bool, bool needsLayout) {
    if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0)
      return;
    m_renderContext.makeCurrent(m_surface->renderTarget());
    auto& renderer = m_surface->renderTarget().renderer();
    if (needsLayout) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      if (m_customContent && m_customContent->compositorBorder)
        m_root->setRadius(Style::scaledRadiusMd(1.0F));
      m_root->setPosition(0.0F, 0.0F);
      m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
      m_root->layout(renderer);
      if (m_moveHandle && m_titleRow) {
        m_moveHandle->setPosition(0.0F, 0.0F);
        const bool headerClose = m_customContent && m_customContent->headerCloseButton;
        m_moveHandle->setSize(
            headerClose && m_titleLabel ? m_titleLabel->width() : m_titleRow->width(), m_titleRow->height()
        );
        m_moveHandle->layout(renderer);
      }
      if (m_style == DialogStyle::Warning)
        m_deleteWarning->setSourceFile(renderer, kDeleteWarning, 72);
      if (m_customContent && m_customContent->onPrepareLayout)
        m_customContent->onPrepareLayout(renderer);
      m_topAccent->setPosition(2.0F, 2.0F);
      m_topAccent->load(renderer);
      const float cornerX = m_root->width() - m_cornerAccent->width() - 7.0F;
      float dividerX = 0.0F;
      float dividerY = 0.0F;
      Node::absolutePosition(m_divider, dividerX, dividerY);
      const float cornerY = dividerY - m_root->y() - m_cornerAccent->height() - 5.0F;
      m_cornerAccent->setVisible(cornerX >= 2.0F && cornerY >= 2.0F);
      m_cornerAccent->setPosition(cornerX, cornerY);
      m_cornerAccent->load(renderer);
      if (m_resizable) {
        const float width = m_root->width();
        const float height = m_root->height();
        const std::array<std::array<float, 4>, 8> bounds{
            {{10.0F, 0.0F, width - 20.0F, 10.0F},
             {10.0F, height - 10.0F, width - 20.0F, 10.0F},
             {0.0F, 10.0F, 10.0F, height - 20.0F},
             {width - 10.0F, 10.0F, 10.0F, height - 20.0F},
             {0.0F, 0.0F, 10.0F, 10.0F},
             {width - 10.0F, 0.0F, 10.0F, 10.0F},
             {0.0F, height - 10.0F, 10.0F, 10.0F},
             {width - 10.0F, height - 10.0F, 10.0F, 10.0F}}
        };
        for (std::size_t i = 0; i < bounds.size(); ++i) {
          m_resizeEdges[i]->setPosition(bounds[i][0], bounds[i][1]);
          m_resizeEdges[i]->setSize(bounds[i][2], bounds[i][3]);
          m_resizeEdges[i]->layout(renderer);
        }
      }
    }
  }

} // namespace filemanager
