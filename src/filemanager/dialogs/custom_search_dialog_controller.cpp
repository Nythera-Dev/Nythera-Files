#include "filemanager/dialogs/custom_search_dialog_controller.h"

#include "config/config_service.h"
#include "core/files/display_text.h"
#include "core/input/key_symbols.h"
#include "filemanager/custom_search_editor_state.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/tooltip/tooltip_manager.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <glib.h>
#include <memory>
#include <utility>
#include <vector>

namespace filemanager {

  namespace {

    Label* addLabel(Flex& parent, std::string_view text, float size) {
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setFontSize(size);
      return static_cast<Label*>(parent.addChild(std::move(label)));
    }

    std::string locationText(const SearchLocations& paths) {
      std::string text;
      for (const auto& path : paths) {
        if (!text.empty())
          text += '\n';
        text += path.native();
      }
      return text;
    }

    std::string countLabel(std::size_t count, std::string_view singular, std::string_view plural) {
      return std::to_string(count) + " " + std::string(count == 1 ? singular : plural);
    }

    std::string profileSummary(const CustomSearch& profile) {
      auto summary = countLabel(profile.excluded.size(), "additional exclusion", "additional exclusions");
      if (explicitlyIncludesRoot(profile.included))
        summary += profile.includeMounts ? " · Mounted filesystems" : " · Skip mounts from /";
      if (profile.options.showHidden)
        summary += " · Hidden items";
      if (profile.options.followSymlinks)
        summary += " · Follow symbolic links";
      return summary;
    }

    // The shared key/value tooltip draws one line per value. Break long paths into
    // continuation rows so the tooltip can show every component without ellipsizing it.
    void addTooltipValue(std::vector<TooltipRow>& rows, std::string key, std::string_view value, int maxColumns) {
      const auto display = sanitizeFilenameForDisplay(value);
      g_autofree gchar* utf8 = g_utf8_make_valid(display.data(), static_cast<gssize>(display.size()));
      const char* start = utf8;
      const char* cursor = start;
      int columns = 0;
      while (*cursor) {
        const auto character = g_utf8_get_char(cursor);
        const int width = g_unichar_iszerowidth(character) ? 0 : g_unichar_iswide(character) ? 2 : 1;
        if (columns + width > maxColumns) {
          rows.push_back({std::exchange(key, {}), std::string(start, cursor)});
          start = cursor;
          columns = 0;
        }
        columns += width;
        cursor = g_utf8_next_char(cursor);
      }
      rows.push_back({std::move(key), std::string(start, cursor)});
    }

    std::vector<TooltipRow> profileTooltip(const CustomSearch& profile, float uiScale) {
      const int columns = static_cast<int>(20.0F / std::clamp(uiScale, 1.0F, 4.0F));
      std::vector<TooltipRow> rows;
      addTooltipValue(rows, "Profile", profile.name, columns);
      for (std::size_t i = 0; i < profile.included.size(); ++i)
        addTooltipValue(rows, "Location " + std::to_string(i + 1), profile.included[i].native(), columns);
      rows.push_back({"Exclusions", std::to_string(profile.excluded.size())});
      for (std::size_t i = 0; i < profile.excluded.size(); ++i)
        addTooltipValue(rows, "Exclusion " + std::to_string(i + 1), profile.excluded[i].native(), columns);
      if (explicitlyIncludesRoot(profile.included))
        rows.push_back({"Root mounts", profile.includeMounts ? "Included" : "Excluded"});
      rows.push_back({"Hidden items", profile.options.showHidden ? "Shown" : "Hidden"});
      rows.push_back({"Follow links", profile.options.followSymlinks ? "Yes" : "No"});
      return rows;
    }

    struct Editor {
      CustomSearchEditorState locations;
      bool completingLine = false;
      Input* name = nullptr;
      Input* included = nullptr;
      Input* excluded = nullptr;
      Flex* rootOption = nullptr;
      Checkbox* mounts = nullptr;
      Checkbox* showHidden = nullptr;
      Checkbox* followLinks = nullptr;
      Label* warning = nullptr;
      Label* error = nullptr;
      Button* save = nullptr;
      bool alive = true;
      bool picker = false;
      void setError(std::string_view message) {
        error->setText(message);
        error->setVisible(!message.empty());
        error->setParticipatesInLayout(!message.empty());
      }
      std::string savedName;
    };

    class SearchProfileRow final : public Flex {
    public:
      SearchProfileRow(
          const CustomSearch& profile, float uiScale, std::function<void(SearchProfileRow&)> select,
          std::function<void()> edit, std::function<void()> remove
      ) {
        setAlign(FlexAlign::Center);
        setGap(Style::spaceMd);
        setPadding(Style::spaceSm, Style::spaceMd);
        setRadius(Style::radiusSm);

        auto hit = std::make_unique<InputArea>();
        hit->setPropagateEvents(true);
        hit->setParticipatesInLayout(false);
        hit->setTooltip(profileTooltip(profile, uiScale));
        hit->setOnEnter([this](const auto&) {
          m_hovered = true;
          updateColors();
        });
        hit->setOnLeave([this] {
          m_hovered = false;
          m_lastClick.reset();
          updateColors();
        });
        hit->setOnClick([this, select = std::move(select), edit](const InputArea::PointerData& click) {
          select(*this);
          constexpr std::uint32_t doubleClickMs = 400;
          constexpr float doubleClickDistance = 6.0F;
          if (m_lastClick
              && click.time - m_lastClick->time <= doubleClickMs
              && std::hypot(click.localX - m_lastClick->localX, click.localY - m_lastClick->localY)
                  <= doubleClickDistance) {
            m_lastClick.reset();
            edit();
          } else {
            m_lastClick = click;
          }
        });
        m_hit = static_cast<InputArea*>(addChild(std::move(hit)));

        auto icon = std::make_unique<Glyph>();
        icon->setGlyph("folder-search");
        icon->setGlyphSize(Style::fontSizeTitle);
        m_icon = static_cast<Glyph*>(addChild(std::move(icon)));

        auto details = std::make_unique<Flex>();
        details->setDirection(FlexDirection::Vertical);
        details->setAlign(FlexAlign::Stretch);
        details->setGap(Style::spaceXs);
        details->setFlexGrow(1.0F);
        m_title = addLabel(*details, sanitizeFilenameForDisplay(profile.name), Style::fontSizeBody);
        m_title->setFontWeight(FontWeight::Medium);
        m_title->setMaxLines(1);
        m_title->setEllipsize(TextEllipsize::End);

        if (!profile.included.empty()) {
          auto locations = std::make_unique<Flex>();
          locations->setAlign(FlexAlign::Center);
          locations->setGap(Style::spaceXs);
          auto* path = addLabel(
              *locations, sanitizeFilenameForDisplay(profile.included.front().native()), Style::fontSizeCaption
          );
          path->setMaxLines(1);
          path->setEllipsize(TextEllipsize::Middle);
          path->setFlexGrow(1.0F);
          m_secondary.push_back(path);
          if (profile.included.size() > 1) {
            auto* remaining = addLabel(
                *locations, "+" + std::to_string(profile.included.size() - 1) + " more", Style::fontSizeCaption
            );
            m_secondary.push_back(remaining);
          }
          details->addChild(std::move(locations));
        }

        auto* summary = addLabel(*details, profileSummary(profile), Style::fontSizeCaption);
        summary->setMaxLines(1);
        summary->setEllipsize(TextEllipsize::End);
        m_secondary.push_back(summary);
        addChild(std::move(details));

        addAction("pencil", "Edit search profile", std::move(edit));
        addAction("trash", "Delete search profile", std::move(remove));
        updateColors();
      }

      void setSelected(bool selected) {
        m_selected = selected;
        updateColors();
      }

    private:
      void addAction(std::string_view icon, std::string_view tooltip, std::function<void()> action) {
        auto button = std::make_unique<Button>();
        button->setVariant(ButtonVariant::Ghost);
        button->setGlyph(icon);
        button->setTooltip(tooltip);
        button->setOnClick(std::move(action));
        addChild(std::move(button));
      }

      void updateColors() {
        setFill(colorSpecFromRole(
            m_hovered        ? ColorRole::Hover
                : m_selected ? ColorRole::PrimaryContainer
                             : ColorRole::SurfaceContainerLow
        ));
        m_title->setColor(colorSpecFromRole(
            m_hovered        ? ColorRole::OnHover
                : m_selected ? ColorRole::OnPrimaryContainer
                             : ColorRole::OnSurface
        ));
        for (auto* label : m_secondary)
          label->setColor(colorSpecFromRole(
              m_hovered        ? ColorRole::OnHover
                  : m_selected ? ColorRole::OnPrimaryContainer
                               : ColorRole::OnSurfaceVariant
          ));
        m_icon->setColor(colorSpecFromRole(
            m_hovered        ? ColorRole::OnHover
                : m_selected ? ColorRole::OnPrimaryContainer
                             : ColorRole::Primary
        ));
      }

      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        Flex::doArrange(renderer, rect);
        m_hit->setFrameSize(width(), height());
      }

      InputArea* m_hit = nullptr;
      Glyph* m_icon = nullptr;
      Label* m_title = nullptr;
      std::vector<Label*> m_secondary;
      std::optional<InputArea::PointerData> m_lastClick;
      bool m_selected = false;
      bool m_hovered = false;
    };

    struct ProfileList {
      ScrollView* scroll = nullptr;
      Label* count = nullptr;
      SearchProfileRow* selected = nullptr;
    };

  } // namespace

  CustomSearchDialogController::CustomSearchDialogController(
      WaylandConnection& wayland, RenderContext& renderContext, ConfigService& configService,
      FileManagerSettings& settings, ModalDialogHost& modalDialog, Callbacks callbacks
  )
      : m_configService(configService), m_settings(settings), m_modalDialog(modalDialog),
        m_customSearchChild(wayland, renderContext, settings, {}), m_callbacks(std::move(callbacks)) {}

  void CustomSearchDialogController::forceCloseImmediate() { m_customSearchChild.forceCloseImmediate(); }

  void CustomSearchDialogController::updateTheme() { m_customSearchChild.updateTheme(); }

  void CustomSearchDialogController::requestLayoutIfOpen() {
    if (m_customSearchChild.isOpen())
      m_customSearchChild.requestLayout();
  }

  bool CustomSearchDialogController::isChildOpen() const { return m_customSearchChild.isOpen(); }

  wl_surface* CustomSearchDialogController::childSurface() const { return m_customSearchChild.wlSurface(); }

  void CustomSearchDialogController::handleChildPointer(const PointerEvent& event) {
    m_customSearchChild.handlePointer(event);
  }

  void CustomSearchDialogController::handleChildKeyboard(const KeyboardEvent& event) {
    m_customSearchChild.handleKeyboard(event);
  }

  void CustomSearchDialogController::editCustomSearch(
      std::optional<CustomSearch> profile, std::function<void()> refreshParent
  ) {
    auto& host = refreshParent ? m_customSearchChild : m_modalDialog;
    auto* parent = refreshParent ? m_modalDialog.surface() : nullptr;
    const auto origin = m_callbacks.activeTabId ? m_callbacks.activeTabId() : kInvalidTabId;
    const auto activeProfile = m_callbacks.activeCustomProfile ? m_callbacks.activeCustomProfile() : std::nullopt;
    const bool selectSaved = !profile || activeProfile == profile->name;
    const auto editor = std::make_shared<Editor>();
    const auto refresh = [&host, editor] {
      const auto included = searchLocationLines(editor->included->value());
      const auto excluded = searchLocationLines(editor->excluded->value());
      const bool root = editor->locations.hasCompletedRoot();
      if (!root)
        editor->mounts->setChecked(false);
      editor->rootOption->setVisible(root);
      editor->rootOption->setParticipatesInLayout(root);
      editor->warning->setText(
          editor->mounts->checked()
              ? "Searching the root filesystem and mounted local and network storage may take significantly longer."
              : "Searching the root filesystem may take significantly longer."
      );
      editor->save->setEnabled(
          validCustomSearchName(trimSearchText(editor->name->value()))
          && !included.empty()
          && std::ranges::all_of(included, validSearchLocation)
          && std::ranges::all_of(excluded, validSearchLocation)
      );
      editor->setError("");
      host.requestLayout();
    };

    const auto startDir = m_callbacks.currentPath ? m_callbacks.currentPath() : std::filesystem::path{};
    const auto browse = [&host, editor, refresh, startDir](bool exclusions) {
      if (editor->picker || host.closeQueued())
        return;
      editor->picker = true;
      FileDialogOptions options{
          .mode = FileDialogMode::SelectFolder,
          .startDirectory = startDir,
          .title = exclusions ? "Choose an Excluded Folder" : "Choose an Included Folder"
      };
      if (!FileDialog::open(std::move(options), [&host, editor, refresh, exclusions](auto path) {
            editor->picker = false;
            if (!editor->alive || host.closeQueued() || !path)
              return;
            if (path->native().find('\n') != std::string::npos) {
              editor->setError("The folder path contains a line break.");
              host.requestLayout();
              return;
            }
            auto* field = exclusions ? editor->excluded : editor->included;
            auto value = field->value();
            if (!value.empty() && !value.ends_with('\n'))
              value += '\n';
            value += path->native();
            if (!exclusions)
              value += '\n';
            field->setValue(value);
            if (!exclusions) {
              const auto completedLine = static_cast<std::size_t>(std::ranges::count(value, '\n') - 1);
              editor->locations.edit(value, value.size(), completedLine);
            }
            refresh();
          })) {
        editor->picker = false;
        editor->setError("Could not open the folder picker.");
        host.requestLayout();
      }
    };

    ModalDialogHost::CustomContent content{
        .buildFields =
            [editor, profile, browse, refresh](Flex& dialog, float) {
              auto scroll = std::make_unique<ScrollView>();
              scroll->setFlexGrow(1.0F);
              auto& root = *scroll->content();
              root.setAlign(FlexAlign::Stretch);
              root.setGap(Style::spaceSm);
              const auto field = [&root](std::string_view label) {
                auto container = std::make_unique<Flex>();
                container->setDirection(FlexDirection::Vertical);
                container->setAlign(FlexAlign::Stretch);
                container->setGap(Style::spaceXs);
                addLabel(*container, label, Style::fontSizeBody);
                return static_cast<Flex*>(root.addChild(std::move(container)));
              };
              auto* nameField = field("Search name");
              auto name = std::make_unique<Input>();
              editor->name = name.get();
              if (profile)
                name->setValue(profile->name);
              name->setOnChange([refresh](const auto&) { refresh(); });
              nameField->addChild(std::move(name));
              const auto addPaths = [&](std::string_view label, bool exclusions) {
                auto* pathsField = field(label);
                auto row = std::make_unique<Flex>();
                row->setGap(Style::spaceSm);
                row->setAlign(FlexAlign::Start);
                auto input = std::make_unique<Input>();
                input->setMultiline(true);
                input->setSize(0.0F, 155.0F);
                input->setFlexGrow(1.0F);
                input->setPlaceholder("One absolute folder path per line");
                if (profile)
                  input->setValue(locationText(exclusions ? profile->excluded : profile->included));
                if (exclusions) {
                  input->setOnChange([refresh](const auto&) { refresh(); });
                } else {
                  editor->locations.load(input->value());
                  input->setOnKeyEvent([editor](std::uint32_t key, std::uint32_t modifiers) {
                    editor->completingLine = KeySymbol::isEnter(key) && !(modifiers & KeyMod::Ctrl);
                    return false;
                  });
                  input->setOnChange([editor, refresh](const std::string& value) {
                    std::optional<std::size_t> completedLine;
                    const auto cursor = static_cast<std::size_t>(editor->included->textInputState().cursor);
                    if (std::exchange(editor->completingLine, false)) {
                      const auto before = std::string_view(value).substr(0, cursor);
                      if (before.ends_with('\n'))
                        completedLine = static_cast<std::size_t>(std::ranges::count(before, '\n') - 1);
                    }
                    editor->locations.edit(value, cursor, completedLine);
                    refresh();
                  });
                }
                (exclusions ? editor->excluded : editor->included) = input.get();
                row->addChild(std::move(input));
                auto button = std::make_unique<Button>();
                button->setVariant(ButtonVariant::Outline);
                button->setText("Browse…");
                button->setOnClick([browse, exclusions] { browse(exclusions); });
                row->addChild(std::move(button));
                pathsField->addChild(std::move(row));
              };
              addPaths("Included locations", false);
              auto preferences = std::make_unique<Flex>();
              preferences->setGap(Style::spaceMd);
              preferences->setWrap(true);
              const auto addCheckbox = [&](std::string_view label, bool checked) {
                auto row = std::make_unique<Flex>();
                row->setAlign(FlexAlign::Center);
                row->setGap(Style::spaceSm);
                auto checkbox = std::make_unique<Checkbox>();
                checkbox->setChecked(checked);
                auto* control = static_cast<Checkbox*>(row->addChild(std::move(checkbox)));
                addLabel(*row, label, Style::fontSizeBody);
                preferences->addChild(std::move(row));
                return control;
              };
              editor->showHidden = addCheckbox("Show hidden items", profile && profile->options.showHidden);
              editor->followLinks = addCheckbox("Follow symbolic links", profile && profile->options.followSymlinks);
              root.addChild(std::move(preferences));
              auto option = std::make_unique<Flex>();
              option->setDirection(FlexDirection::Vertical);
              option->setGap(Style::spaceSm);
              option->setAlign(FlexAlign::Stretch);
              editor->rootOption = option.get();
              auto row = std::make_unique<Flex>();
              row->setAlign(FlexAlign::Center);
              row->setGap(Style::spaceSm);
              auto checkbox = std::make_unique<Checkbox>();
              editor->mounts = checkbox.get();
              checkbox->setChecked(profile && profile->includeMounts);
              checkbox->setOnChange([refresh](bool) { refresh(); });
              row->addChild(std::move(checkbox));
              addLabel(*row, "Include mounted filesystems", Style::fontSizeBody);
              option->addChild(std::move(row));
              auto notice = std::make_unique<Flex>();
              notice->setAlign(FlexAlign::Start);
              notice->setGap(Style::spaceSm);
              const auto warningColor = colorSpecFromRole(ColorRole::Error);
              auto icon = std::make_unique<Glyph>();
              icon->setGlyph("alert-triangle");
              icon->setGlyphSize(Style::fontSizeTitle);
              icon->setColor(warningColor);
              notice->addChild(std::move(icon));
              editor->warning = addLabel(*notice, "", Style::fontSizeBody);
              editor->warning->setFlexGrow(1.0F);
              editor->warning->setColor(warningColor);
              option->addChild(std::move(notice));
              root.addChild(std::move(option));
              addPaths("Additional exclusions", true);
              addLabel(root, "Global search exclusions also apply.", Style::fontSizeBody);
              editor->error = addLabel(root, "", Style::fontSizeBody);
              editor->error->setColor(colorSpecFromRole(ColorRole::Error));
              editor->error->setMaxLines(2);
              dialog.addChild(std::move(scroll));
            },
        .bindChoiceButton =
            [editor](int result, Button* button) {
              if (result == 1)
                editor->save = button;
            },
        .afterBuild = refresh,
        .initialFocus = [editor] { return editor->name->inputArea(); },
        .interceptResponse =
            [this, &host, editor, profile](int result) {
              if (result != 1)
                return false;
              if (!editor->save || !editor->save->enabled())
                return true;
              CustomSearch replacement{
                  trimSearchText(editor->name->value()),
                  searchLocationLines(editor->included->value()),
                  searchLocationLines(editor->excluded->value()),
                  false,
                  {editor->showHidden->checked(), editor->followLinks->checked()},
              };
              replacement.includeMounts =
                  CustomSearchEditorState::saveWithMounts(replacement.included, editor->mounts->checked());
              if (!m_settings.editCustomSearch(profile, replacement)) {
                editor->setError(
                    "Could not save the search. Check that its name is unique and the configuration is writable."
                );
                host.requestLayout();
                return true;
              }
              editor->savedName = std::move(replacement.name);
              return false;
            },
        .onDismiss =
            [this, editor] {
              editor->alive = false;
              if (std::exchange(editor->picker, false)) {
                if (m_callbacks.closeDestinationPicker)
                  m_callbacks.closeDestinationPicker();
              }
            },
        .fillContent = true,
        .usePreferredInitialHeight = true,
        .preferredWidth = 690,
        .preferredHeight = 680,
        .minWidth = 420,
        .minHeight = 320,
        .sizeStateKey = "custom_search_editor",
    };
    host.showDialog(
        profile ? "Edit Custom Search" : "Define Custom Search", "",
        {{"Cancel", 0, DialogChoiceStyle::Cancel}, {"Save", 1}},
        [this, editor, origin, selectSaved, refreshParent = std::move(refreshParent)](int result) {
          if (result == 1 && refreshParent)
            refreshParent();
          const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
          const auto activeId = m_callbacks.activeTabId ? m_callbacks.activeTabId() : kInvalidTabId;
          if (result == 1 && selectSaved && windowOpen && activeId == origin && m_callbacks.selectCustomSearch)
            m_callbacks.selectCustomSearch(std::move(editor->savedName));
        },
        parent, std::move(content)
    );
  }

  void CustomSearchDialogController::manageCustomSearches() {
    if (m_modalDialog.isOpen())
      return;

    auto list = std::make_shared<ProfileList>();
    const auto refresh = [this, list] {
      if (!list->scroll || !m_modalDialog.isOpen())
        return;
      TooltipManager::instance().forceDestroy();
      auto& content = *list->scroll->content();
      m_modalDialog.setFocus(nullptr);
      list->selected = nullptr;
      while (!content.children().empty())
        content.removeChild(content.children().back().get());

      const auto& profiles = m_settings.searchSettings().profiles;
      list->count->setText(countLabel(profiles.size(), "saved profile", "saved profiles"));
      if (profiles.empty())
        addLabel(content, "There are no saved custom searches.", Style::fontSizeBody)
            ->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      for (const auto& profile : profiles) {
        auto row = std::make_unique<SearchProfileRow>(
            profile, m_configService.config().accessibility.uiScale,
            [model = list.get()](SearchProfileRow& chosen) {
              if (model->selected && model->selected != &chosen)
                model->selected->setSelected(false);
              model->selected = &chosen;
              chosen.setSelected(true);
            },
            [this, profile] {
              TooltipManager::instance().forceDestroy();
              if (m_refreshSearchProfileList)
                editCustomSearch(profile, m_refreshSearchProfileList);
            },
            [this, profile] {
              TooltipManager::instance().forceDestroy();
              m_customSearchChild.showDialog(
                  "Delete Custom Search?", "Delete the saved search “" + profile.name + "”?",
                  {{"Cancel", 0, DialogChoiceStyle::Cancel}, {"Delete", 1, DialogChoiceStyle::Destructive}},
                  [this, profile](int answer) {
                    if (answer != 1)
                      return;
                    if (!m_settings.editCustomSearch(profile, std::nullopt) && m_callbacks.onStatus)
                      m_callbacks.onStatus("Could not delete the custom search.");
                    if (m_refreshSearchProfileList)
                      m_refreshSearchProfileList();
                  },
                  m_modalDialog.surface()
              );
            }
        );
        content.addChild(std::move(row));
      }
      m_modalDialog.requestLayout();
    };
    m_refreshSearchProfileList = refresh;

    auto content = ModalDialogHost::CustomContent{
        .buildFields =
            [this, list](Flex& root, float) {
              auto heading = std::make_unique<Flex>();
              heading->setAlign(FlexAlign::Center);
              heading->setGap(Style::spaceSm);
              auto* label = addLabel(*heading, "Saved search profiles", Style::fontSizeBody);
              label->setFontWeight(FontWeight::Medium);
              label->setFlexGrow(1.0F);
              auto create = std::make_unique<Button>();
              create->setVariant(ButtonVariant::Outline);
              create->setGlyph("plus");
              create->setText("New search");
              create->setOnClick([this] {
                TooltipManager::instance().forceDestroy();
                if (m_refreshSearchProfileList)
                  editCustomSearch(std::nullopt, m_refreshSearchProfileList);
              });
              heading->addChild(std::move(create));
              root.addChild(std::move(heading));

              auto scroll = std::make_unique<ScrollView>();
              scroll->setFlexGrow(1.0F);
              scroll->setViewportPaddingH(Style::spaceSm);
              scroll->setViewportPaddingV(Style::spaceSm);
              scroll->setRadius(Style::radiusMd);
              scroll->setFill(colorSpecFromRole(ColorRole::Surface));
              scroll->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
              scroll->content()->setAlign(FlexAlign::Stretch);
              scroll->content()->setGap(Style::spaceXs);
              list->scroll = scroll.get();
              root.addChild(std::move(scroll));

              list->count = addLabel(root, "", Style::fontSizeCaption);
              list->count->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
            },
        .interceptResponse = [this](int) { return m_customSearchChild.isOpen(); },
        .onDismiss =
            [this, list] {
              *list = {};
              m_refreshSearchProfileList = {};
            },
        .fillContent = true,
        .usePreferredInitialHeight = true,
        .hoverTooltips = true,
        .preferredWidth = 620,
        .preferredHeight = 470,
        .minHeight = 240,
        .sizeStateKey = "custom_search_management",
    };
    m_modalDialog.showDialog("Manage Custom Searches", "", {{"Close", 0}}, {}, nullptr, std::move(content));
    if (!m_modalDialog.isOpen())
      m_refreshSearchProfileList = {};
    refresh();
  }

} // namespace filemanager
