#include "filemanager/places_sidebar.h"

#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/byte_size_format.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/menu_ids.h"
#include "filemanager/place_metadata.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <sstream>

namespace filemanager {

  namespace {
    constexpr std::string_view kRecentPlaceId = "personal:recent";
    constexpr float kPlaceRowSelectionOpacity = 0.42F;
    constexpr float kStorageMeterFillOpacity = 0.75F;
    constexpr float kStorageMeterTrackOpacity = 0.6F;

    constexpr float kToolbarHeight = 32.0F;

    std::string compactFreeSpace(std::uint64_t bytes) { return formatCompactBinaryByteSize(bytes) + " free"; }

    void useSubtleSelection(Button& button, bool muted = false) {
      auto palette = Button::defaultPalette(button.variant());
      palette.selected = Button::ButtonStateColors{
          .bg = colorSpecFromRole(ColorRole::PrimaryContainer, kPlaceRowSelectionOpacity),
          .border = clearColorSpec(),
          .label = colorSpecFromRole(ColorRole::OnPrimaryContainer),
      };
      if (muted) {
        for (auto* state : {&palette.normal, &palette.hover, &palette.pressed})
          state->label = colorSpecFromRole(ColorRole::OnSurfaceVariant);
      }
      button.setCustomPalette(std::move(palette));
    }

    std::unique_ptr<Button> makeButton(std::string_view text, std::function<void()> action, Button** out = nullptr) {
      auto button = std::make_unique<Button>();
      button->setText(text);
      button->setVariant(ButtonVariant::Ghost);
      button->setControlHeight(kToolbarHeight);
      button->setPadding(5.0F);
      button->setOnClick(std::move(action));
      if (out != nullptr)
        *out = button.get();
      return button;
    }

    struct CapacityQuery {
      PlacesSidebarController* owner;
      std::weak_ptr<int> alive;
      std::string identity;
      std::string requestKey;
      GCancellable* cancellable;
    };
  } // namespace

  PlacesSidebarController::PlacesSidebarController(
      FileManagerSettings& settings, FileOperationManager& operations, PathResolutionCache& pathCache,
      Callbacks callbacks
  )
      : m_settings(settings), m_operations(operations), m_pathCache(pathCache), m_callbacks(std::move(callbacks)) {}

  PlacesSidebarController::~PlacesSidebarController() { shutdown(); }

  void PlacesSidebarController::attachUi(ScrollView* scroll, Flex* content) {
    m_scroll = scroll;
    m_content = content;
    if (!m_model) {
      m_model = std::make_unique<PlacesModel>();
      m_model->setChangedCallback([this] { markDirty(true); });
    }
    rebuild();
  }

  void PlacesSidebarController::detachUi() {
    m_scroll = nullptr;
    m_content = nullptr;
    m_insertionLines.clear();
    m_placeRows.clear();
    m_placeButtons.clear();
    m_placeCapacities.clear();
  }

  void PlacesSidebarController::shutdown() {
    *m_alive = false;
    for (const auto& prompt : m_mountPrompts) {
      g_signal_handlers_disconnect_by_data(prompt.get(), this);
      g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
    }
    m_mountPrompts.clear();
    m_placesAlive.reset();
    cancelCapacityQueries();
    m_pathCache.cancel();
    m_model.reset();
    detachUi();
  }

  void PlacesSidebarController::reloadPreferences() {
    m_savedPlacesLoaded = false;
    markDirty(true);
  }

  void PlacesSidebarController::markDirty(bool refreshCapacity) {
    m_dirty = true;
    if (refreshCapacity)
      m_refreshCapacity = true;
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  void PlacesSidebarController::persistSavedPlaces() {
    if (!m_settings.saveSavedPlaces(m_savedPlaces) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save Places");
    markDirty(true);
  }

  void PlacesSidebarController::removeSavedPlace(std::string_view identity) {
    std::erase_if(m_savedPlaces, [&](const auto& saved) { return "saved:" + saved.id == identity; });
    persistSavedPlaces();
  }

  bool PlacesSidebarController::isPersonalPlaceSaved(const std::filesystem::path& path) const {
    return std::ranges::any_of(m_placeRows, [&](const auto& row) {
      return row.second.personal && m_pathCache.samePersonalPlace(row.second.path, path);
    });
  }

  void PlacesSidebarController::savePlace(const std::filesystem::path& requested, bool verifiedDirectory) {
    const bool browsingTrash = m_callbacks.isBrowsingTrash && m_callbacks.isBrowsingTrash();
    if ((!verifiedDirectory && browsingTrash) || requested.empty() || !requested.is_absolute())
      return;
    const auto path = requested.lexically_normal();
    if (isPersonalPlaceSaved(path) || std::ranges::any_of(m_savedPlaces, [&](const auto& saved) {
          return m_pathCache.samePersonalPlace(saved.path, path);
        })) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("This folder is already saved in Places");
      return;
    }
    auto save = [this, alive = std::weak_ptr<bool>(m_alive), path, verifiedDirectory](const PlacePathResolution& res) {
      const auto locked = alive.lock();
      if (!locked
          || !*locked
          || (m_callbacks.isQuitting && m_callbacks.isQuitting())
          || (!verifiedDirectory && m_callbacks.isBrowsingTrash && m_callbacks.isBrowsingTrash()))
        return;
      if (!res.isDirectory) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("This folder is unavailable");
        return;
      }
      if (isPersonalPlaceSaved(path) || std::ranges::any_of(m_savedPlaces, [&](const auto& saved) {
            return m_pathCache.samePersonalPlace(saved.path, path);
          })) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("This folder is already saved in Places");
        return;
      }
      const auto label = path.filename().empty() ? path.string() : path.filename().string();
      gchar* uuid = g_uuid_string_random();
      m_savedPlaces.push_back({uuid, path, label});
      g_free(uuid);
      persistSavedPlaces();
    };
    if (verifiedDirectory)
      save({.isDirectory = true});
    else
      m_pathCache.resolveAsync(path, std::move(save));
  }

  std::optional<std::string>
  PlacesSidebarController::placeInsertionAt(float x, float y, std::string_view group, bool edgesOnly) const {
    if (!m_scroll)
      return std::nullopt;
    float vx = 0, vy = 0;
    Node::absolutePosition(m_scroll, vx, vy);
    if (x < vx || x >= vx + m_scroll->width() || y < vy || y >= vy + m_scroll->height())
      return std::nullopt;
    // Lines exist before every row and after each group. Their small hit regions
    // distinguish bookmark insertion from dropping files onto a folder's centre.
    const float strip = edgesOnly ? 7.0F : 100000.0F;
    float distance = strip;
    std::optional<std::string> result;
    for (const auto& [id, line] : m_insertionLines) {
      bool visible = true;
      for (const Node* node = line; node; node = node->parent())
        if (!node->visible()) {
          visible = false;
          break;
        }
      if (!visible)
        continue;
      const auto row =
          std::ranges::find_if(m_placeRows, [&](const auto& entry) { return entry.second.identity == id; });
      if (id != "end:" + std::string(group) && (row == m_placeRows.end() || placeGroup(row->second) != group))
        continue;
      float lx = 0, ly = 0;
      Node::absolutePosition(line, lx, ly);
      // Reordering remains within its own group's bounds, including capacity text.
      if (x < lx || x >= lx + line->width())
        continue;
      const float delta = std::abs(y - (ly + line->height() * 0.5F));
      if (delta <= distance) {
        distance = delta;
        result = id;
      }
    }
    if (!edgesOnly && result) {
      float first = 100000.0F, last = -100000.0F;
      for (const auto& [id, line] : m_insertionLines) {
        const auto row =
            std::ranges::find_if(m_placeRows, [&](const auto& entry) { return entry.second.identity == id; });
        if (id == "end:" + std::string(group) || (row != m_placeRows.end() && placeGroup(row->second) == group)) {
          float lx = 0, ly = 0;
          Node::absolutePosition(line, lx, ly);
          first = std::min(first, ly);
          last = std::max(last, ly + line->height());
        }
      }
      if (y < first - 4.0F || y > last + 4.0F)
        return std::nullopt;
    }
    return result;
  }

  void PlacesSidebarController::showPlaceInsertion(const std::optional<std::string>& before) {
    for (const auto& [id, line] : m_insertionLines)
      line->setFill(before && *before == id ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec());
  }

  void PlacesSidebarController::reorderPlace(const std::string& identity, const std::string& before) {
    std::vector<Place> places;
    for (const auto& [row, place] : m_placeRows)
      places.push_back(place);
    auto order = m_settings.loadPlaceOrder();
    order = reorderedPlaces(places, std::move(order), identity, before);
    if (!m_settings.savePlaceOrder(order) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save Places order");
    markDirty(true);
  }

  void PlacesSidebarController::pinFolders(
      const std::vector<std::string>& paths, const std::string& before, bool verifiedDirectories
  ) {
    std::vector<Place> places;
    for (const auto& [row, place] : m_placeRows)
      places.push_back(place);
    if (before != "end:Personal"
        && std::ranges::none_of(places, [&](const auto& place) { return place.personal && place.identity == before; }))
      return;
    std::vector<std::filesystem::path> candidates;
    for (const auto& requested : paths) {
      const auto path = std::filesystem::path(requested).lexically_normal();
      if (path.is_absolute())
        candidates.push_back(path);
    }
    if (candidates.empty()) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("No new folders to pin");
      return;
    }
    struct PinBatch {
      std::string before;
      std::vector<std::filesystem::path> candidates;
      std::vector<PlacePathResolution> results;
      std::size_t remaining = 0;
    };
    auto batch = std::make_shared<PinBatch>(PinBatch{
        .before = before,
        .candidates = candidates,
        .results = std::vector<PlacePathResolution>(candidates.size()),
        .remaining = candidates.size(),
    });
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      auto onResolved = [this, alive = std::weak_ptr<bool>(m_alive), batch, i](const PlacePathResolution& res) {
        const auto locked = alive.lock();
        if (!locked || !*locked || (m_callbacks.isQuitting && m_callbacks.isQuitting()))
          return;
        batch->results[i] = res;
        if (--batch->remaining > 0)
          return;
        std::vector<Place> currentPlaces;
        for (const auto& [row, place] : m_placeRows)
          currentPlaces.push_back(place);
        if (batch->before != "end:Personal" && std::ranges::none_of(currentPlaces, [&](const auto& place) {
              return place.personal && place.identity == batch->before;
            }))
          return;
        auto order = m_settings.loadPlaceOrder();
        bool added = false;
        for (std::size_t idx = 0; idx < batch->candidates.size(); ++idx) {
          if (!batch->results[idx].isDirectory)
            continue;
          const auto& path = batch->candidates[idx];
          if (std::ranges::any_of(currentPlaces, [&](const auto& place) {
                return place.personal && m_pathCache.samePersonalPlace(place.path, path);
              }))
            continue;
          gchar* uuid = g_uuid_string_random();
          const std::string id(uuid);
          g_free(uuid);
          const std::string label = path.filename().empty() ? path.string() : path.filename().string();
          m_savedPlaces.push_back({id, path, label});
          currentPlaces.push_back({.label = label, .identity = "saved:" + id, .path = path, .personal = true});
          order = reorderedPlaces(currentPlaces, std::move(order), "saved:" + id, batch->before);
          added = true;
        }
        if (added) {
          const bool saved = m_settings.saveSavedPlaces(m_savedPlaces, &order);
          markDirty(true);
          if (m_callbacks.onStatus)
            m_callbacks.onStatus(saved ? "Folders pinned to Places" : "Unable to save Places");
        } else if (m_callbacks.onStatus) {
          m_callbacks.onStatus("No new folders to pin");
        }
      };
      if (verifiedDirectories)
        onResolved({.isDirectory = true});
      else
        m_pathCache.resolveAsync(candidates[i], std::move(onResolved));
    }
  }

  void PlacesSidebarController::openPlaceMenu(const Place& place, float x, float y) {
    m_placeMenuTarget = place;
    std::vector<ContextMenuControlEntry> entries;
    if (place.mounted)
      entries.push_back(
          {.id = menu_id::kPlaceOpen,
           .label = "Open",
           .actionIconNames = {"folder-open-symbolic", "folder-open"},
           .actionGlyph = "folder-open"}
      );
    if (place.mounted && !place.path.empty())
      entries.push_back(
          {.id = menu_id::kPlaceOpenInNewTab,
           .label = "Open in New Tab",
           .actionIconNames = {"tab-new-symbolic", "tab-new"},
           .actionGlyph = "plus"}
      );
    if (place.identity == "personal:trash")
      entries.push_back({.id = menu_id::kEmptyWastebin, .label = "Empty Wastebin…"});
    if (place.identity != "personal:trash") {
      entries.push_back(
          {.id = menu_id::kPlaceProperties,
           .label = "Properties",
           .actionIconNames = {"document-properties-symbolic", "document-properties"},
           .actionGlyph = "file-info"}
      );
      if (place.mounted && !place.path.empty())
        entries.push_back(
            {.id = menu_id::kPlaceOpenNewWindow,
             .label = "Open in New Window",
             .actionIconNames = {"window-new-symbolic", "window-new"},
             .actionGlyph = "app-window"}
        );
    }
    if (place.identity.starts_with("saved:"))
      entries.push_back(
          {.id = menu_id::kPlaceRemove,
           .label = "Remove from Places",
           .actionIconNames = {"bookmark-remove-symbolic", "bookmark-remove"},
           .actionGlyph = "bookmark-minus"}
      );
    else if (place.mounted && !place.personal && !place.path.empty())
      entries.push_back(
          {.id = menu_id::kPlacePin,
           .label = "Add to Places",
           .actionIconNames = {"bookmark-new-symbolic", "bookmark-new"},
           .actionGlyph = "bookmark-plus"}
      );
    if (!m_deviceOperations.contains(place.identity)) {
      if (place.canMount)
        entries.push_back({.id = menu_id::kPlaceMount, .label = "Mount"});
      if (place.canUnmount)
        entries.push_back({.id = menu_id::kPlaceUnmount, .label = "Unmount"});
      if (place.canEject)
        entries.push_back({.id = menu_id::kPlaceEject, .label = "Eject"});
    }
    const auto hidden = m_settings.loadHiddenMounts();
    if (!place.personal) {
      const bool isHidden = std::ranges::find(hidden, place.identity) != hidden.end();
      entries.push_back(
          {.id = isHidden ? menu_id::kPlaceUnhide : menu_id::kPlaceHide,
           .label = isHidden ? "Show in Places" : "Hide from Places"}
      );
    }
    if (!hidden.empty() || m_showHiddenMounts)
      entries.push_back(
          {.id = menu_id::kPlaceToggleShowHidden,
           .label = "Show hidden mounts",
           .checkmark = true,
           .toggleState = m_showHiddenMounts ? 1 : 0}
      );
    groupFileMenuEntries(entries, FileMenuContext::Places);
    if (m_callbacks.onShowMenu)
      m_callbacks.onShowMenu(std::move(entries), x, y);
  }

  void PlacesSidebarController::setPlaceHidden(const Place& place, bool hidden) {
    if (place.personal || place.identity.empty())
      return;
    auto identities = m_settings.loadHiddenMounts();
    std::erase(identities, place.identity);
    if (hidden)
      identities.push_back(place.identity);
    if (!m_settings.saveHiddenMounts(identities)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("Unable to save hidden mounts");
      return;
    }
    if (m_callbacks.onStatus)
      m_callbacks.onStatus(hidden ? "Hidden from Places" : "Shown in Places");
    markDirty(false);
  }

  void PlacesSidebarController::toggleShowHiddenMounts() {
    m_showHiddenMounts = !m_showHiddenMounts;
    markDirty(false);
  }

  void PlacesSidebarController::openPlacesVisibilityMenu(float x, float y) {
    m_placeMenuTarget.reset();
    if (m_callbacks.onShowMenu)
      m_callbacks.onShowMenu(
          {{.id = menu_id::kPlaceToggleShowHidden,
            .label = "Show hidden mounts",
            .checkmark = true,
            .toggleState = m_showHiddenMounts ? 1 : 0}},
          x, y
      );
  }

  void PlacesSidebarController::mountPassword(
      GMountOperation* operation, GAskPasswordFlags flags, std::string user, std::string domain, unsigned step
  ) {
    auto prompt = std::shared_ptr<GMountOperation>(G_MOUNT_OPERATION(g_object_ref(operation)), g_object_unref);
    if (step == 0 && (flags & G_ASK_PASSWORD_NEED_USERNAME)) {
      m_callbacks.onTextPrompt(
          "Mount authentication", "Username (leave blank to use " + user + ")", false,
          [this, prompt, flags, user, domain](auto value) {
            if (!value) {
              g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
              return;
            }
            mountPassword(prompt.get(), flags, value->empty() ? user : *value, domain, 1);
          }
      );
      return;
    }
    if (step <= 1 && (flags & G_ASK_PASSWORD_NEED_DOMAIN)) {
      m_callbacks.onTextPrompt(
          "Mount authentication", "Domain (leave blank to use " + domain + ")", false,
          [this, prompt, flags, user, domain](auto value) {
            if (!value) {
              g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
              return;
            }
            mountPassword(prompt.get(), flags, user, value->empty() ? domain : *value, 2);
          }
      );
      return;
    }
    g_mount_operation_set_username(operation, user.c_str());
    g_mount_operation_set_domain(operation, domain.c_str());
    g_mount_operation_set_password_save(operation, G_PASSWORD_SAVE_NEVER);
    if (flags & G_ASK_PASSWORD_NEED_PASSWORD) {
      m_callbacks.onTextPrompt("Mount authentication", "Password", true, [prompt](auto value) {
        if (!value) {
          g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_ABORTED);
          return;
        }
        g_mount_operation_set_password(prompt.get(), value->c_str());
        std::fill(value->begin(), value->end(), '\0');
        g_mount_operation_reply(prompt.get(), G_MOUNT_OPERATION_HANDLED);
      });
    } else
      g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
  }

  void PlacesSidebarController::operatePlace(const Place& place, PlaceOperation operation) {
    if (m_operations.replaying()) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("Wait for Undo or Redo to finish");
      return;
    }
    if (!m_deviceOperations.empty()
        || (m_callbacks.hasPendingClipboardOrRename && m_callbacks.hasPendingClipboardOrRename())) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("Wait for the current device or clipboard operation");
      return;
    }
    // Eject may affect sibling volumes on the same physical device.
    if (operation != PlaceOperation::Mount
        && ((operation == PlaceOperation::Eject && m_operations.active())
            || (!place.path.empty() && m_operations.usesMount(place.path)))) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("Wait for transfers on this device to finish before removing it");
      return;
    }
    m_deviceOperations.insert(place.identity);
    if (m_callbacks.onStatus)
      m_callbacks.onStatus("Working on " + place.label + "…");
    GMountOperation* prompt = g_mount_operation_new();
    m_mountPrompts.emplace_back(G_MOUNT_OPERATION(g_object_ref(prompt)), g_object_unref);
    g_signal_connect(
        prompt, "ask-password",
        G_CALLBACK((+[](GMountOperation* op, const char*, const char* user, const char* domain, GAskPasswordFlags flags,
                        gpointer data) {
          static_cast<PlacesSidebarController*>(data)->mountPassword(
              op, flags, user ? user : "", domain ? domain : "", 0
          );
        })),
        this
    );
    g_signal_connect(
        prompt, "ask-question",
        G_CALLBACK((+[](GMountOperation* op, const char* message, const char* const* choices, gpointer data) {
          auto* self = static_cast<PlacesSidebarController*>(data);
          auto retained = std::shared_ptr<GMountOperation>(G_MOUNT_OPERATION(g_object_ref(op)), g_object_unref);
          std::vector<std::pair<std::string, int>> actions;
          for (int i = 0; choices && choices[i]; ++i)
            actions.emplace_back(choices[i], i);
          self->m_callbacks.onShowDialog(
              "Device operation", message ? message : "Choose an action", std::move(actions), [retained](int choice) {
                if (choice < 0)
                  g_mount_operation_reply(retained.get(), G_MOUNT_OPERATION_ABORTED);
                else {
                  g_mount_operation_set_choice(retained.get(), choice);
                  g_mount_operation_reply(retained.get(), G_MOUNT_OPERATION_HANDLED);
                }
              }
          );
        })),
        this
    );
    // Busy-device requests never offer to force removal or terminate processes.
    g_signal_connect(
        prompt, "show-processes",
        G_CALLBACK((+[](GMountOperation* op, const char*, GArray*, const char* const*, gpointer) {
          g_object_set_data(G_OBJECT(op), "nythera-busy", GINT_TO_POINTER(1));
          g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
        })),
        nullptr
    );
    std::weak_ptr<bool> alive = m_alive;
    m_model->operateDevice(
        place, operation, prompt, nullptr, [this, alive, prompt, identity = place.identity](std::string error) {
          const auto locked = alive.lock();
          if (!locked || !*locked)
            return;
          if (g_object_get_data(G_OBJECT(prompt), "nythera-busy"))
            error = "This device is busy; close files and wait for other applications before trying again";
          std::erase_if(m_mountPrompts, [prompt](const auto& retained) { return retained.get() == prompt; });
          m_deviceOperations.erase(identity);
          if (m_callbacks.onStatus)
            m_callbacks.onStatus(error.empty() ? "Device operation completed" : error);
          markDirty(true);
        }
    );
    g_object_unref(prompt);
  }

  std::unique_ptr<Flex> PlacesSidebarController::buildRecentRow(const Place& place) {
    auto recentRow = std::make_unique<Flex>();
    recentRow->setDirection(FlexDirection::Horizontal);
    recentRow->setAlign(FlexAlign::Center);
    recentRow->setGap(2.0F);

    auto recentBtn = makeButton("Recent", [this, path = place.path] {
      if (m_callbacks.onNavigate)
        m_callbacks.onNavigate(path, true);
    });
    recentBtn->setContentAlign(ButtonContentAlign::Start);
    recentBtn->setFocusHighlightEnabled(false);
    recentBtn->setFlexGrow(1.0F);
    useSubtleSelection(*recentBtn, false);
    recentBtn->setTooltip("Recent Files and Locations");
    auto icon = std::make_unique<FileIcon>();
    icon->setThemedIconNames({"document-open-recent", "document-open-recent-symbolic", "clock"});
    icon->setGlyphSize(20.0F);
    recentBtn->setLeadingIcon(std::move(icon), 20.0F);
    if (auto* area = recentBtn->inputArea())
      area->setTabFocusKey("place:" + place.identity);
    auto* recentBtnPtr = static_cast<Button*>(recentRow->addChild(std::move(recentBtn)));
    m_placeButtons.emplace_back(recentBtnPtr, place.path);
    m_placeRows.emplace_back(recentBtnPtr, place);

    if (m_settings.showFrequentLocations() && m_callbacks.frequentLocations) {
      auto arrowPtr = std::make_shared<Button*>(nullptr);
      auto arrowBtn = std::make_unique<Button>();
      *arrowPtr = arrowBtn.get();
      arrowBtn->setGlyph("chevron-right");
      arrowBtn->setGlyphSize(14.0F);
      arrowBtn->setVariant(ButtonVariant::Ghost);
      arrowBtn->setControlHeight(kToolbarHeight);
      arrowBtn->setPadding(0.0F, 6.0F);
      arrowBtn->setFocusHighlightEnabled(false);
      arrowBtn->setTooltip("Frequent Locations");
      arrowBtn->setOnClick([this, arrowPtr] {
        if (!m_callbacks.onShowFrequentMenu || !m_callbacks.frequentLocations || *arrowPtr == nullptr)
          return;
        auto frequent = m_callbacks.frequentLocations();
        std::vector<ContextMenuControlEntry> entries;
        if (frequent.empty())
          entries.push_back({.label = "No frequent locations yet", .enabled = false});
        for (std::size_t i = 0; i < frequent.size(); ++i) {
          const auto& record = frequent[i];
          auto label = record.path.string();
          if (record.pinned)
            label += " (Pinned)";
          const int id = static_cast<int>(i) * 4;
          entries.push_back({
              .id = id,
              .label = std::move(label),
              .hasSubmenu = true,
              .submenuEntries =
                  {{.id = id, .label = "Open"},
                   {.id = id + 1, .label = record.pinned ? "Unpin" : "Pin"},
                   {.id = id + 2, .label = "Remove"},
                   {.id = id + 3, .label = "Exclude from Frequent"}},
              .splitSubmenu = true,
              .activateMainSection = true,
          });
        }
        float x = 0.0F, y = 0.0F;
        Node::absolutePosition(*arrowPtr, x, y);
        m_callbacks.onShowFrequentMenu(
            std::move(entries), x + (*arrowPtr)->width(), y, [this, frequent = std::move(frequent)](int id) {
              const auto& record = frequent.at(static_cast<std::size_t>(id / 4));
              switch (id % 4) {
              case 0:
                if (m_callbacks.onNavigate)
                  m_callbacks.onNavigate(record.path, true);
                break;
              case 1:
                if (m_callbacks.onTogglePinFrequentLocation)
                  m_callbacks.onTogglePinFrequentLocation(record.path, !record.pinned);
                break;
              case 2:
                if (m_callbacks.onRemoveFrequentLocation)
                  m_callbacks.onRemoveFrequentLocation(record.path);
                break;
              case 3:
                if (m_callbacks.onExcludeFrequentLocation)
                  m_callbacks.onExcludeFrequentLocation(record.path);
                break;
              }
            }
        );
      });
      recentRow->addChild(std::move(arrowBtn));
    }
    return recentRow;
  }

  void PlacesSidebarController::rebuild() {
    m_dirty = false;
    if (m_content == nullptr || m_model == nullptr)
      return;
    const float scrollOffset = m_scroll ? m_scroll->scrollOffset() : 0.0F;
    const auto currentPath = m_callbacks.currentPath ? m_callbacks.currentPath() : std::filesystem::path{};
    const float sidebarWidth = m_callbacks.sidebarWidth ? m_callbacks.sidebarWidth() : 220.0F;
    auto places = m_model->discover(false);
    // Filter XDG personal directories asynchronously without blocking the UI thread:
    // a slow or unavailable directory will never prevent other Places from loading.
    {
      std::unordered_set<std::string> seenPersonalTargets;
      std::vector<Place> filtered;
      filtered.reserve(places.size());
      for (auto& place : places) {
        if (place.personal && place.identity.starts_with("personal:") && place.identity != "personal:trash") {
          const auto norm = place.path.lexically_normal();
          const std::string key = norm.string();
          const bool isHome = place.identity.starts_with("personal:home:");
          if (const auto cached = m_pathCache.findCached(norm)) {
            if (!isHome && !cached->isDirectory)
              continue;
            const std::string targetKey = cached->canonicalPath.empty() ? key : cached->canonicalPath.string();
            if (!isHome && !seenPersonalTargets.insert(targetKey).second)
              continue;
            if (isHome)
              seenPersonalTargets.insert(targetKey);
          } else {
            seenPersonalTargets.insert(key);
            m_pathCache.resolveAsync(norm, [this, alive = std::weak_ptr<bool>(m_alive)](const PlacePathResolution&) {
              const auto locked = alive.lock();
              if (!locked || !*locked)
                return;
              markDirty(false);
            });
          }
        }
        filtered.push_back(std::move(place));
      }
      places = std::move(filtered);
    }
    if (!m_savedPlacesLoaded) {
      m_savedPlacesLoaded = true;
      m_savedPlaces = m_settings.loadSavedPlaces();
    }
    places.push_back(
        {.label = "Wastebin",
         .identity = "personal:trash",
         .path = "trash:/",
         .iconNames = {"user-trash", "user-trash-symbolic"},
         .personal = true}
    );
    std::unordered_set<std::string> savedPaths;
    for (const auto& saved : m_savedPlaces) {
      const auto norm = saved.path.lexically_normal();
      if (!savedPaths.insert(norm.string()).second)
        continue;
      // Cache hits call their completion inline. Only new resolutions should
      // invalidate the sidebar, or each rebuild queues another layout.
      if (!m_pathCache.findCached(norm)) {
        m_pathCache.resolveAsync(norm, [this, alive = std::weak_ptr<bool>(m_alive)](const PlacePathResolution&) {
          const auto locked = alive.lock();
          if (!locked || !*locked)
            return;
          markDirty(false);
        });
      }
      places.push_back(
          {.label = saved.label,
           .identity = "saved:" + saved.id,
           .path = saved.path,
           .iconNames = {"folder", "folder-symbolic"},
           .personal = true}
      );
    }
    if (m_settings.recentActivityEnabled() && m_settings.showRecentSidebar()) {
      places.insert(
          places.begin(),
          Place{.label = "Recent", .identity = std::string(kRecentPlaceId), .path = "recent:/", .personal = true}
      );
    }
    orderPlaces(places, m_settings.loadPlaceOrder());
    bool currentMountRemoved = false;
    std::vector<std::filesystem::path> removedMounts;
    std::unordered_set<std::string> seenRemovedMounts;
    for (const auto& previous : m_placeCapacities) {
      const bool stillMounted = std::ranges::any_of(places, [&](const auto& place) {
        return place.mounted && !place.personal && place.path == previous.mountPath;
      });
      if (!stillMounted && !previous.mountPath.empty()) {
        if (seenRemovedMounts.insert(previous.mountPath.string()).second)
          removedMounts.push_back(previous.mountPath);
      }
      const auto relative = currentPath.lexically_relative(previous.mountPath);
      if (!stillMounted && !relative.empty() && *relative.begin() != "..")
        currentMountRemoved = true;
    }
    std::unordered_set<std::string> liveCapacityKeys;
    for (const auto& place : places)
      if (place.mounted && !place.personal)
        liveCapacityKeys.insert(place.capacityIdentity.empty() ? place.identity : place.capacityIdentity);
    for (auto it = m_capacityQueries.begin(); it != m_capacityQueries.end();) {
      if (!liveCapacityKeys.contains(it->first)) {
        g_cancellable_cancel(it->second);
        g_object_unref(it->second);
        it = m_capacityQueries.erase(it);
      } else
        ++it;
    }
    while (!m_content->children().empty())
      m_content->removeChild(m_content->children().front().get());
    if (m_callbacks.onClearDropPlace)
      m_callbacks.onClearDropPlace();
    m_insertionLines.clear();
    m_placeRows.clear();
    m_placeButtons.clear();
    m_placeCapacities.clear();

    const std::array<std::pair<const char*, const char*>, 3> groups{
        {{"Personal", "personal_expanded"}, {"Devices", "devices_expanded"}, {"Network", "network_expanded"}}
    };
    const auto hidden = m_settings.loadHiddenMounts();
    const bool showCapacity = m_settings.showPlacesCapacity();
    for (const auto& [nameRaw, keyRaw] : groups) {
      const std::string name(nameRaw), key(keyRaw);
      if ((name == "Personal" && !m_settings.showPlacesPersonal())
          || (name == "Devices" && !m_settings.showPlacesDevices())
          || (name == "Network" && !m_settings.showPlacesNetwork()))
        continue;
      const bool expanded = m_settings.placesSectionExpanded(key);
      auto group = std::make_unique<Flex>();
      Flex* groupPtr = group.get();
      auto headingPtr = std::make_shared<Button*>(nullptr);
      auto heading = makeButton(
          name,
          [this, key, groupPtr, headingPtr] {
            const bool expand = !m_settings.placesSectionExpanded(key);
            (void)m_settings.setPlacesSectionExpanded(key, expand);
            groupPtr->setVisible(expand);
            groupPtr->setParticipatesInLayout(expand);
            if (*headingPtr)
              (*headingPtr)->setGlyph(expand ? "chevron-down" : "chevron-right");
            if (m_callbacks.onRequestLayout)
              m_callbacks.onRequestLayout();
          },
          headingPtr.get()
      );
      *headingPtr = static_cast<Button*>(heading.get());
      if (auto* area = (*headingPtr)->inputArea())
        area->setTabFocusKey("place-group:" + key);
      (*headingPtr)->setGlyph(expanded ? "chevron-down" : "chevron-right");
      (*headingPtr)->setVariant(ButtonVariant::Ghost);
      (*headingPtr)->setContentAlign(ButtonContentAlign::Start);
      (*headingPtr)->setFontSize(Style::fontSizeBody * 1.02F);
      (*headingPtr)->setControlHeight(30.0F);
      if (name != "Personal")
        (*headingPtr)->setOnRightClickWithPointer([this](float x, float y, std::uint32_t serial, std::uint32_t) {
          if (m_callbacks.onInputSerial)
            m_callbacks.onInputSerial(serial);
          openPlacesVisibilityMenu(x, y);
        });
      m_content->addChild(std::move(heading));
      group->setDirection(FlexDirection::Vertical);
      group->setAlign(FlexAlign::Stretch);
      group->setGap(2.0F);
      group->setVisible(expanded);
      group->setParticipatesInLayout(expanded);
      for (const auto& place : places) {
        if ((name == "Personal" && !place.personal)
            || (name == "Devices" && (place.personal || place.network))
            || (name == "Network" && !place.network))
          continue;
        const bool isHidden = !place.personal && std::ranges::find(hidden, place.identity) != hidden.end();
        if (isHidden && !m_showHiddenMounts)
          continue;
        std::string caption = place.label;
        if (isHidden)
          caption += " (hidden)";
        {
          auto line = std::make_unique<Flex>();
          line->setMinHeight(2);
          line->setMaxHeight(2);
          line->setFill(clearColorSpec());
          m_insertionLines.emplace_back(place.identity, line.get());
          group->addChild(std::move(line));
        }
        if (place.identity == kRecentPlaceId) {
          group->addChild(buildRecentRow(place));
          continue;
        }
        auto row = makeButton(caption, [this, place] {
          if (place.mounted && m_callbacks.onNavigate)
            m_callbacks.onNavigate(place.path, true);
        });
        row->setOnRightClickWithPointer([this, place](float x, float y, std::uint32_t serial, std::uint32_t) {
          if (m_callbacks.onInputSerial)
            m_callbacks.onInputSerial(serial);
          openPlaceMenu(place, x, y);
        });
        if (place.mounted && !place.path.empty()) {
          row->setOnMiddleClick([this, place] {
            if (m_callbacks.onMiddleClickPlace)
              m_callbacks.onMiddleClickPlace(place);
          });
        }
        row->setContentAlign(ButtonContentAlign::Start);
        row->setFocusHighlightEnabled(false);
        useSubtleSelection(*row, !place.mounted);
        row->setTooltip(placeTooltip(place));
        if (!place.mounted) {
          row->label()->setFlexGrow(1.0F);
          auto disconnected = std::make_unique<FileIcon>();
          disconnected->setThemedIconNames({"network-offline-symbolic", "network-offline"});
          disconnected->setGlyphSize(14.0F);
          row->addChild(std::move(disconnected));
        }
        if (!place.iconNames.empty()) {
          auto icon = std::make_unique<FileIcon>();
          icon->setFile(place.path, true);
          icon->setThemedIconNames(place.iconNames);
          icon->setGlyphSize(22.0F);
          row->setLeadingIcon(std::move(icon), 22.0F);
        }
        if (place.mounted)
          m_placeButtons.emplace_back(row.get(), place.path);
        auto* rowPtr = static_cast<Button*>(group->addChild(std::move(row)));
        if (auto* area = rowPtr->inputArea())
          area->setTabFocusKey("place:" + place.identity);
        m_placeRows.emplace_back(rowPtr, place);
        if (showCapacity && place.mounted && !place.personal) {
          rowPtr->setClipChildren(true);
          rowPtr->setMirrorInRtl(false);
          rowPtr->label()->setFlexGrow(1.0F);
          rowPtr->label()->setMaxLines(1);
          rowPtr->label()->setEllipsize(TextEllipsize::End);
          auto label = std::make_unique<Label>();
          label->setFontSize(Style::fontSizeCaption);
          label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
          label->setMaxLines(1);
          label->setEllipsize(TextEllipsize::End);
          label->setTextAlign(Style::rtl() ? TextAlign::Start : TextAlign::End);
          auto* labelPtr = label.get();
          // The button applies its state foreground to direct label children.
          auto secondary = std::make_unique<Flex>();
          secondary->addChild(std::move(label));
          rowPtr->addChild(std::move(secondary));

          auto bar = std::make_unique<ProgressBar>();
          bar->setFillColor(colorSpecFromRole(ColorRole::Primary, kStorageMeterFillOpacity));
          bar->setTrackColor(colorSpecFromRole(ColorRole::SurfaceContainerHighest, kStorageMeterTrackOpacity));
          bar->setVisible(false);
          bar->setParticipatesInLayout(false);
          auto* barPtr = static_cast<ProgressBar*>(group->addChild(std::move(bar)));
          const std::string capacityKey = place.capacityIdentity.empty() ? place.identity : place.capacityIdentity;
          m_placeCapacities.push_back({place.identity, capacityKey, place.path, labelPtr, barPtr, rowPtr});
          requestPlaceCapacity(m_placeCapacities.back(), m_refreshCapacity);
        }
      }
      {
        auto line = std::make_unique<Flex>();
        line->setMinHeight(2);
        line->setMaxHeight(2);
        line->setFill(clearColorSpec());
        m_insertionLines.emplace_back("end:" + name, line.get());
        group->addChild(std::move(line));
      }
      m_content->addChild(std::move(group));
    }
    updateCapacityBarWidths(sidebarWidth);
    m_refreshCapacity = false;
    if (m_callbacks.onUpdateNavigationControls)
      m_callbacks.onUpdateNavigationControls();
    if (m_scroll)
      m_scroll->setScrollOffset(scrollOffset);
    if (!removedMounts.empty() && m_callbacks.onMountRemoved) {
      for (const auto& mountPath : removedMounts)
        m_callbacks.onMountRemoved(mountPath);
    } else if (currentMountRemoved && m_callbacks.onNavigate) {
      m_callbacks.onNavigate(std::filesystem::path(g_get_home_dir()), false);
    }
  }

  void PlacesSidebarController::requestPlaceCapacity(const PlaceCapacityView& place, bool forceRefresh) {
    if (place.capacityIdentity.empty() || !place.freeSpaceLabel)
      return;
    const auto filesystemId = m_placeFilesystemIds.find(place.capacityIdentity);
    const std::string cacheKey = filesystemId != m_placeFilesystemIds.end() && !filesystemId->second.empty()
        ? "filesystem:" + filesystemId->second
        : place.capacityIdentity;
    auto cached = m_capacityCache.find(cacheKey);
    const bool fresh = cached != m_capacityCache.end()
        && std::chrono::steady_clock::now() - cached->second.updatedAt < std::chrono::seconds(30);
    auto render = [&](const CapacityInfo& info) {
      const auto entry =
          std::ranges::find_if(m_placeRows, [&](const auto& item) { return item.second.identity == place.identity; });
      if (entry != m_placeRows.end()) {
        const auto tooltip = placeTooltip(entry->second, info.filesystemType, info.free, info.total);
        place.row->setTooltip(tooltip);
      }
      if (info.total == 0) {
        place.freeSpaceLabel->setText("Unavailable");
        place.bar->setVisible(false);
        place.bar->setParticipatesInLayout(false);
        return;
      }
      place.freeSpaceLabel->setText(compactFreeSpace(info.free));
      place.bar->setProgress(
          static_cast<float>(info.total - std::min(info.free, info.total)) / static_cast<float>(info.total)
      );
      place.bar->setVisible(true);
      place.bar->setParticipatesInLayout(true);
    };
    if (cached != m_capacityCache.end())
      render(cached->second);
    else
      place.freeSpaceLabel->setText("Checking…");
    if (!forceRefresh && fresh)
      return;
    if (m_capacityQueries.contains(place.capacityIdentity))
      return;
    GCancellable* cancellable = g_cancellable_new();
    m_capacityQueries.emplace(place.capacityIdentity, cancellable);
    auto* query = new CapacityQuery{
        this, m_placesAlive, place.identity, place.capacityIdentity, G_CANCELLABLE(g_object_ref(cancellable))
    };
    PlacesModel::queryCapacity(place.mountPath, cancellable, placeCapacityReady, query);
  }

  void PlacesSidebarController::refreshCapacities() {
    for (const auto& place : m_placeCapacities)
      requestPlaceCapacity(place, true);
  }

  void PlacesSidebarController::invalidateMountCapacitiesForPath(const std::filesystem::path&) {
    for (const auto& capacity : m_placeCapacities)
      requestPlaceCapacity(capacity, false);
  }

  void PlacesSidebarController::cancelCapacityQueries() {
    for (auto& [key, cancellable] : m_capacityQueries) {
      (void)key;
      g_cancellable_cancel(cancellable);
      g_object_unref(cancellable);
    }
    m_capacityQueries.clear();
  }

  void PlacesSidebarController::updateCapacityBarWidths(float sidebarWidth) {
    const float width = std::max(80.0F, sidebarWidth - 28.0F);
    for (const auto& capacity : m_placeCapacities) {
      capacity.bar->setSize(width, 4.0F);
      // Reserve space for the icon and an ellipsis before constraining the capacity label.
      capacity.freeSpaceLabel->setMaxWidth(width - 32.0F);
    }
  }

  void PlacesSidebarController::updateActiveHighlight(const std::filesystem::path& currentPath, bool) {
    bool selected = false;
    for (const auto& [button, path] : m_placeButtons) {
      const bool match = !selected && path == currentPath;
      button->setSelected(match);
      selected = selected || match;
    }
  }

  void PlacesSidebarController::placeCapacityReady(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<CapacityQuery> query(static_cast<CapacityQuery*>(data));
    GError* error = nullptr;
    GFileInfo* info = g_file_query_filesystem_info_finish(G_FILE(source), result, &error);
    const bool cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (error)
      g_error_free(error);
    if (query->alive.expired()) {
      if (query->cancellable)
        g_object_unref(query->cancellable);
      if (info)
        g_object_unref(info);
      return;
    }
    query->owner->completePlaceCapacityQuery(query->identity, query->requestKey, query->cancellable, info, cancelled);
    if (query->cancellable)
      g_object_unref(query->cancellable);
    if (info)
      g_object_unref(info);
  }

  void PlacesSidebarController::completePlaceCapacityQuery(
      const std::string& identity, const std::string& requestKey, GCancellable* cancellable, GFileInfo* info,
      bool cancelled
  ) {
    auto pending = m_capacityQueries.find(requestKey);
    if (pending == m_capacityQueries.end() || pending->second != cancellable)
      return;
    g_object_unref(pending->second);
    m_capacityQueries.erase(pending);
    const bool hasView = std::ranges::any_of(m_placeCapacities, [&](const auto& item) {
      return item.identity == identity && item.capacityIdentity == requestKey;
    });
    if (!hasView || cancelled)
      return;
    CapacityInfo value;
    if (info != nullptr) {
      if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)
          && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
        value.total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        value.free = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
      }
      const char* type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);
      const char* fsid = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
      if (type)
        value.filesystemType = type;
      if (fsid)
        value.filesystemId = fsid;
    }
    value.updatedAt = std::chrono::steady_clock::now();
    m_placeFilesystemIds[requestKey] = value.filesystemId;
    m_capacityCache[requestKey] = value;
    if (!value.filesystemId.empty())
      m_capacityCache["filesystem:" + value.filesystemId] = value;
    for (const auto& item : m_placeCapacities)
      if (item.capacityIdentity == requestKey)
        requestPlaceCapacity(item, false);
  }

  std::optional<std::filesystem::path> PlacesSidebarController::dropDestinationAt(float x, float y) const {
    if (!m_scroll)
      return std::nullopt;
    float vx = 0, vy = 0;
    Node::absolutePosition(m_scroll, vx, vy);
    if (x < vx || x >= vx + m_scroll->width() || y < vy || y >= vy + m_scroll->height())
      return std::nullopt;
    for (const auto& [row, place] : m_placeRows) {
      if (!place.mounted || place.identity == "personal:trash" || place.identity == kRecentPlaceId)
        continue;
      bool visible = true;
      for (Node* node = row; node; node = node->parent())
        if (!node->visible()) {
          visible = false;
          break;
        }
      if (!visible)
        continue;
      float rx = 0, ry = 0;
      Node::absolutePosition(row, rx, ry);
      if (x >= rx && x < rx + row->width() && y >= ry && y < ry + row->height())
        return place.path;
    }
    return std::nullopt;
  }

  std::optional<std::size_t> PlacesSidebarController::rowIndexAt(float x, float y) const {
    if (!m_scroll)
      return std::nullopt;
    float vx = 0, vy = 0;
    Node::absolutePosition(m_scroll, vx, vy);
    if (x < vx || y < vy || x >= vx + m_scroll->width() || y >= vy + m_scroll->height())
      return std::nullopt;
    for (std::size_t i = 0; i < m_placeRows.size(); ++i) {
      const auto& [row, place] = m_placeRows[i];
      bool visible = true;
      for (Node* node = row; node; node = node->parent())
        if (!node->visible()) {
          visible = false;
          break;
        }
      if (!visible)
        continue;
      float rx = 0, ry = 0;
      Node::absolutePosition(row, rx, ry);
      if (x >= rx && x < rx + row->width() && y >= ry && y < ry + row->height())
        return i;
    }
    return std::nullopt;
  }

  void PlacesSidebarController::cancelGesture() {
    m_reorderId.reset();
    m_reorderBefore.reset();
    m_reordering = false;
    showPlaceInsertion(std::nullopt);
  }

  bool PlacesSidebarController::handleActiveReorderPointer(
      const PointerEvent& event, float x, float y, std::uint32_t pointerSerial
  ) {
    if (!m_reorderId)
      return false;
    const auto source =
        std::ranges::find_if(m_placeRows, [this](const auto& pair) { return pair.second.identity == *m_reorderId; });
    if (event.type == PointerEvent::Type::Axis && m_reordering) {
      if (m_callbacks.onPointerAxis)
        m_callbacks.onPointerAxis(event, x, y);
      if (source != m_placeRows.end()) {
        const auto& place = source->second;
        m_reorderBefore = placeInsertionAt(
            x, y,
            place.personal      ? "Personal"
                : place.network ? "Network"
                                : "Devices",
            false
        );
        showPlaceInsertion(m_reorderBefore);
        if (m_callbacks.onRequestRedraw)
          m_callbacks.onRequestRedraw();
      }
      return true;
    }
    if (event.type == PointerEvent::Type::Motion) {
      if (std::hypot(x - m_dragStartX, y - m_dragStartY) > 5.0F)
        m_reordering = true;
      m_reorderBefore.reset();
      if (m_reordering && source != m_placeRows.end()) {
        const auto& place = source->second;
        m_reorderBefore = placeInsertionAt(
            x, y,
            place.personal      ? "Personal"
                : place.network ? "Network"
                                : "Devices",
            false
        );
      }
      showPlaceInsertion(m_reorderBefore);
      if (pointerSerial && m_reordering && m_callbacks.onSetCursorShape)
        m_callbacks.onSetCursorShape(
            pointerSerial,
            m_reorderBefore ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
        );
      if (m_callbacks.onRequestRedraw)
        m_callbacks.onRequestRedraw();
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      if (m_reordering && m_reorderBefore)
        reorderPlace(*m_reorderId, *m_reorderBefore);
      else if (!m_reordering && source != m_placeRows.end()) {
        const auto place = source->second;
        if (place.mounted && m_callbacks.onNavigate)
          m_callbacks.onNavigate(place.path, true);
      }
      cancelGesture();
      if (m_callbacks.onCancelPointerCapture)
        m_callbacks.onCancelPointerCapture();
      if (pointerSerial && m_callbacks.onSetCursorShape)
        m_callbacks.onSetCursorShape(pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      if (m_callbacks.onRequestRedraw)
        m_callbacks.onRequestRedraw();
      return true;
    }
    return true;
  }

  bool PlacesSidebarController::tryBeginReorderPress(const PointerEvent& event, float x, float y) {
    if (event.type != PointerEvent::Type::Button || event.button != BTN_LEFT || !event.pressed)
      return false;
    const auto row = rowIndexAt(x, y);
    if (!row)
      return false;
    const auto& place = m_placeRows[*row].second;
    m_reorderId = place.identity;
    m_dragStartX = x;
    m_dragStartY = y;
    m_reordering = false;
    if (m_callbacks.onCancelPointerCapture)
      m_callbacks.onCancelPointerCapture();
    return true;
  }

} // namespace filemanager
