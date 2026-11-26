#include "core/files/display_text.h"
#include "filemanager/file_manager.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/tooltip/tooltip_manager.h"

#include <algorithm>
#include <cmath>
#include <glib.h>
#include <utility>

namespace {
  using filemanager::CustomSearch;
  using filemanager::sanitizeFilenameForDisplay;

  Label* addLabel(Flex& parent, std::string_view text, float size) {
    auto label = std::make_unique<Label>();
    label->setText(text);
    label->setFontSize(size);
    return static_cast<Label*>(parent.addChild(std::move(label)));
  }

  std::string countLabel(std::size_t count, std::string_view singular, std::string_view plural) {
    return std::to_string(count) + " " + std::string(count == 1 ? singular : plural);
  }

  std::string profileSummary(const CustomSearch& profile) {
    auto summary = countLabel(profile.excluded.size(), "additional exclusion", "additional exclusions");
    if (filemanager::explicitlyIncludesRoot(profile.included))
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
    if (filemanager::explicitlyIncludesRoot(profile.included))
      rows.push_back({"Root mounts", profile.includeMounts ? "Included" : "Excluded"});
    rows.push_back({"Hidden items", profile.options.showHidden ? "Shown" : "Hidden"});
    rows.push_back({"Follow links", profile.options.followSymlinks ? "Yes" : "No"});
    return rows;
  }

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
        auto* path =
            addLabel(*locations, sanitizeFilenameForDisplay(profile.included.front().native()), Style::fontSizeCaption);
        path->setMaxLines(1);
        path->setEllipsize(TextEllipsize::Middle);
        path->setFlexGrow(1.0F);
        m_secondary.push_back(path);
        if (profile.included.size() > 1) {
          auto* remaining =
              addLabel(*locations, "+" + std::to_string(profile.included.size() - 1) + " more", Style::fontSizeCaption);
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

void FileManager::manageCustomSearches() {
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
                {{"Cancel", 0}, {"Delete", 1}},
                [this, profile](int answer) {
                  if (answer != 1)
                    return;
                  if (!m_settings.editCustomSearch(profile, std::nullopt))
                    setStatus("Could not delete the custom search.");
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

  auto content = filemanager::ModalDialogHost::CustomContent{
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
  m_modalDialog.showDialog("Manage Custom Searches", "", {{"Close", 0}}, {}, nullptr, false, false, std::move(content));
  if (!m_modalDialog.isOpen())
    m_refreshSearchProfileList = {};
  refresh();
}
