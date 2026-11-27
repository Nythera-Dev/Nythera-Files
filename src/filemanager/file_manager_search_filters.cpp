#include "filemanager/file_manager.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/calendar_view.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <format>
#include <span>

namespace {
  using namespace filemanager;

  enum Choice : std::size_t { SizeOp, SizeFirstUnit, SizeSecondUnit, DateOp, RelativeUnit, TextMode };
  enum Value : std::size_t { SizeFirst, SizeSecond, RelativeAmount, ContentText };
  enum Check : std::size_t { CaseSensitive, Regex };

  constexpr float kNumericWidth = 98.0F;
  constexpr float kContentInputMinWidth = 140.0F;
  constexpr float kMetadataSeparatorGap = Style::spaceLg;

  constexpr int kFirstFilterChoice = 30300;
  constexpr int kChoiceStride = 10;
  constexpr std::array<std::string_view, 4> kSizeOps{"Larger than", "Smaller than", "Exactly", "Between"};
  constexpr std::array<std::string_view, 9> kSizeUnits{"Bytes", "KB", "MB", "GB", "TB", "KiB", "MiB", "GiB", "TiB"};
  constexpr std::array<std::string_view, 5> kDateOps{"Within the last", "Older than", "Before", "After", "Between"};
  constexpr std::array<std::string_view, 5> kDateUnits{"Hours", "Days", "Weeks", "Months", "Years"};
  constexpr std::array<std::string_view, 3> kContentModes{
      "Plain text only", "Extracted text only", "All supported files"
  };
  constexpr std::array<std::span<const std::string_view>, 6> kChoices{kSizeOps, kSizeUnits, kSizeUnits,
                                                                      kDateOps, kDateUnits, kContentModes};

  std::array<std::size_t, kChoices.size()> selection(const SearchFilterDraft& draft) {
    return {static_cast<std::size_t>(draft.sizeComparison), static_cast<std::size_t>(draft.sizeFirstUnit),
            static_cast<std::size_t>(draft.sizeSecondUnit), static_cast<std::size_t>(draft.dateComparison),
            static_cast<std::size_t>(draft.dateUnit),       static_cast<std::size_t>(draft.content.mode)};
  }

  Flex& row(Flex& parent, float gap = Style::spaceXs) {
    auto node = std::make_unique<Flex>();
    node->setAlign(FlexAlign::Center);
    node->setGap(gap);
    return *static_cast<Flex*>(parent.addChild(std::move(node)));
  }

  void show(Node& node, bool visible) {
    node.setVisible(visible);
    node.setParticipatesInLayout(visible);
  }

  std::string dateLabel(SearchDate date, std::string_view empty) {
    return date.year ? std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day) : std::string(empty);
  }
} // namespace

void FileManager::buildSearchFilters(Flex& pane) {
  auto& ui = m_searchFilterUi;
  auto panel = std::make_unique<Flex>();
  panel->setDirection(FlexDirection::Vertical);
  panel->setAlign(FlexAlign::Stretch);
  panel->setGap(Style::spaceXs);
  panel->setPadding(Style::spaceSm);
  panel->setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
  panel->setRadius(Style::scaledRadiusSm(1.0F));
  ui.panel = static_cast<Flex*>(pane.addChild(std::move(panel)));

  const auto caption = [](Flex& parent, std::string_view text) {
    auto label = std::make_unique<Label>();
    label->setText(text);
    label->setFontSize(Style::fontSizeCaption);
    label->setColor(colorSpecFromRole(ColorRole::OnSurface));
    return static_cast<Label*>(parent.addChild(std::move(label)));
  };
  const auto checkbox = [&](Flex& parent, Check field, std::string_view text, std::string_view tooltip,
                            bool ContentFilter::* member) {
    auto& group = row(parent, Style::spaceSm);
    const auto change = [this, member](bool checked) {
      activeSession().searchFilters().content.*member = checked;
      syncSearchFilters();
    };
    auto control = std::make_unique<Checkbox>();
    ui.checks[field] = control.get();
    control->setOnChange(change);
    group.addChild(std::move(control));

    auto label = std::make_unique<Button>();
    label->setVariant(ButtonVariant::Ghost);
    label->setText(text);
    label->setFontSize(Style::fontSizeCaption);
    label->setContentAlign(ButtonContentAlign::Start);
    label->setPadding(Style::spaceXs);
    label->setTooltip(tooltip);
    label->setOnClick([this, field, change] {
      const auto& toggle = *m_searchFilterUi.checks[field];
      if (toggle.enabled())
        change(!toggle.checked());
    });
    group.addChild(std::move(label));
  };
  const auto choose = [&](Flex& parent, Choice field, float width) {
    auto control = std::make_unique<Button>();
    ui.choices[field] = control.get();
    control->setMinWidth(width);
    control->setMaxWidth(width);
    control->setFontSize(Style::fontSizeCaption);
    control->setPadding(Style::spaceSm);
    control->setVariant(ButtonVariant::Outline);
    control->setTrailingGlyph("chevron-down");
    control->setOnClick([this, field] { openSearchFilterMenu(field); });
    parent.addChild(std::move(control));
  };
  const auto input = [&](Flex& parent, Value field, std::string_view placeholder, auto member) {
    auto control = std::make_unique<Input>();
    ui.values[field] = control.get();
    control->setFontSize(Style::fontSizeCaption);
    control->setMinLayoutWidth(field == ContentText ? kContentInputMinWidth : kNumericWidth);
    if (field == ContentText)
      control->setFlexGrow(1.0F);
    else
      control->setHorizontalPadding(Style::spaceSm);
    control->setPlaceholder(placeholder);
    control->setOnChange([this, field, member](const std::string& text) {
      auto& session = activeSession();
      if (field != ContentText) {
        changeSearchMetadata([&](auto& draft) { member(draft) = text; });
        return;
      }
      auto& draft = session.searchFilters();
      member(draft) = text;
      if (text.empty() && draft.submitted) {
        draft.submitted.reset();
        refreshSearchQuery(session.search().query());
      } else {
        syncSearchFilters();
      }
    });
    control->setOnSubmit([this, field](const std::string&) {
      if (field == ContentText)
        submitSearchFilters();
      else
        activeSession().submitSearch();
    });
    parent.addChild(std::move(control));
  };

  auto& metadata = row(*ui.panel);
  ui.metadata = &metadata;
  auto& size = row(metadata);
  ui.size = &size;
  size.setWrap(true);
  auto& sizeHeading = row(size, Style::spaceSm);
  ui.sizeCaption = caption(sizeHeading, "File size");
  choose(sizeHeading, SizeOp, 112.0F);
  auto& firstSize = row(size);
  input(firstSize, SizeFirst, "Size", [](auto& draft) -> auto& { return draft.sizeFirst; });
  choose(firstSize, SizeFirstUnit, 70.0F);
  auto& secondSize = row(size);
  input(secondSize, SizeSecond, "Maximum", [](auto& draft) -> auto& { return draft.sizeSecond; });
  choose(secondSize, SizeSecondUnit, 70.0F);

  auto separator = std::make_unique<Flex>();
  separator->setSize(1.0F, Style::controlHeightSm);
  separator->setFill(colorSpecFromRole(ColorRole::OutlineVariant));
  ui.separator = static_cast<Flex*>(metadata.addChild(std::move(separator)));

  auto& date = row(metadata);
  ui.date = &date;
  date.setWrap(true);
  auto& dateHeading = row(date, Style::spaceSm);
  caption(dateHeading, "Date modified");
  choose(dateHeading, DateOp, 124.0F);
  auto& relative = row(date);
  input(relative, RelativeAmount, "Amount", [](auto& draft) -> auto& { return draft.dateAmount; });
  choose(relative, RelativeUnit, 84.0F);
  for (std::size_t index = 0; index < ui.dates.size(); ++index) {
    auto button = std::make_unique<Button>();
    button->setVariant(ButtonVariant::Outline);
    button->setGlyph("calendar");
    button->setFontSize(Style::fontSizeCaption);
    button->setPadding(Style::spaceSm);
    button->setOnClick([this, index] { openSearchDatePicker(index != 0); });
    ui.dates[index] = button.get();
    date.addChild(std::move(button));
  }

  auto& content = row(*ui.panel);
  ui.content = &content;
  auto& contentsQuery = row(content, Style::spaceSm);
  contentsQuery.setFlexGrow(1.0F);
  caption(contentsQuery, "File contents");
  input(contentsQuery, ContentText, "Search file contents", [](auto& draft) -> auto& { return draft.content.query; });
  auto& options = row(content);
  ui.options = &options;
  options.setWrap(true);
  choose(options, TextMode, 164.0F);
  checkbox(options, CaseSensitive, "Case sensitive", "", &ContentFilter::caseSensitive);
  checkbox(options, Regex, "Regex", "Regular expression", &ContentFilter::regex);
  auto submit = std::make_unique<Button>();
  submit->setText("Search");
  submit->setOnClick([this] { submitSearchFilters(); });
  ui.submit = submit.get();
  options.addChild(std::move(submit));

  show(*ui.panel, false);
}

void FileManager::updateSearchFilterWidth(float paneWidth, Renderer& renderer) {
  auto& ui = m_searchFilterUi;
  if (!ui.panel || !ui.panel->visible())
    return;
  const float width = paneWidth - ui.panel->paddingLeft() - ui.panel->paddingRight();
  auto* query = static_cast<Flex*>(ui.content->children().front().get());
  const float labelWidth = query->children().front()->measure(renderer, {}).width;
  ui.sizeCaption->setMinWidth(labelWidth);

  const bool metadataInline =
      ui.size->measure(renderer, {}).width + ui.date->measure(renderer, {}).width + 2.0F * kMetadataSeparatorGap + 1.0F
      <= width;
  ui.metadata->setGap(metadataInline ? kMetadataSeparatorGap : Style::spaceSm);
  ui.metadata->setDirection(metadataInline ? FlexDirection::Horizontal : FlexDirection::Vertical);
  ui.metadata->setAlign(metadataInline ? FlexAlign::Center : FlexAlign::Stretch);
  show(*ui.separator, metadataInline);

  const bool contentInline =
      labelWidth + query->gap() + kContentInputMinWidth + ui.content->gap() + ui.options->measure(renderer, {}).width
      <= width;
  ui.content->setDirection(contentInline ? FlexDirection::Horizontal : FlexDirection::Vertical);
  ui.content->setAlign(contentInline ? FlexAlign::Center : FlexAlign::Stretch);
  // Vertical layout gives the query the full width without using it to absorb height.
  query->setFlexGrow(contentInline ? 1.0F : 0.0F);
}

void FileManager::changeSearchMetadata(const std::function<void(SearchFilterDraft&)>& change) {
  auto& session = activeSession();
  auto& draft = session.searchFilters();
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  const auto size = resolveSizeFilter(draft);
  const auto modified = resolveModifiedFilter(draft, now);
  change(draft);
  if (size != resolveSizeFilter(draft) || modified != resolveModifiedFilter(draft, now))
    refreshSearchQuery(session.search().query());
  else
    syncSearchFilters();
}

void FileManager::syncSearchFilters() {
  using namespace filemanager;
  auto& ui = m_searchFilterUi;
  auto& session = activeSession();
  const auto& draft = session.searchFilters();
  const bool expanded = session.searchOpen() && session.searchFiltersExpanded();
  if (ui.panel->visible() != expanded) {
    if (!expanded) {
      m_menus.closeContextMenu();
      for (const Node* node = m_dispatcher.focusedArea(); node; node = node->parent()) {
        if (node == ui.panel) {
          m_dispatcher.setFocus(
              session.searchOpen() ? m_searchInput->inputArea() : (m_grid ? m_grid->focusArea() : nullptr)
          );
          break;
        }
      }
    }
    show(*ui.panel, expanded);
    if (m_surface)
      m_surface->requestLayout();
  }

  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  const auto filters = resolveSearchFilters(draft, now);
  const int active = static_cast<int>(filters.size.has_value())
      + static_cast<int>(filters.modified.has_value())
      + static_cast<int>(filters.content.has_value());
  show(*ui.clear, expanded);
  ui.toggle->setSelected(expanded);
  ui.toggle->setText(std::to_string(active));
  ui.toggle->setTooltip(std::format("Search filters · {} active", active));
  if (!expanded)
    return;

  const auto selected = selection(draft);
  for (std::size_t index = 0; index < selected.size(); ++index)
    ui.choices[index]->setText(kChoices[index][selected[index]]);
  const std::array<std::string_view, 4> values{
      draft.sizeFirst, draft.sizeSecond, draft.dateAmount, draft.content.query
  };
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (ui.values[index]->value() != values[index])
      ui.values[index]->setValue(values[index]);
  }
  const std::array checks{draft.content.caseSensitive, draft.content.regex};
  for (std::size_t index = 0; index < checks.size(); ++index)
    ui.checks[index]->setChecked(checks[index]);

  const bool sizeBetween = draft.sizeComparison == SizeComparison::Between;
  show(*ui.values[SizeSecond]->parent(), sizeBetween);
  const bool invalidSizes = !draft.sizeFirst.empty() && (!sizeBetween || !draft.sizeSecond.empty()) && !filters.size;
  ui.values[SizeFirst]->setInvalid(invalidSizes);
  ui.values[SizeSecond]->setInvalid(invalidSizes);

  const bool relative = draft.dateComparison == DateComparison::Within || draft.dateComparison == DateComparison::Older;
  const bool dateBetween = draft.dateComparison == DateComparison::Between;
  show(*ui.values[RelativeAmount]->parent(), relative);
  ui.values[RelativeAmount]->setInvalid(!draft.dateAmount.empty() && !filters.modified);
  show(*ui.dates[0], !relative);
  show(*ui.dates[1], dateBetween);
  ui.dates[0]->setText(dateLabel(draft.dateFirst, dateBetween ? "Start date" : "Choose date"));
  ui.dates[1]->setText(dateLabel(draft.dateSecond, "End date"));

  const bool rg = ui.backends.supports(ContentMode::Plain);
  const bool rga = ui.backends.supports(ContentMode::Extracted);
  ui.checks[CaseSensitive]->setEnabled(rg);
  ui.checks[Regex]->setEnabled(rg);
  ui.choices[TextMode]->setEnabled(rg);
  ui.values[ContentText]->setEnabled(rg);
  const bool queryValid = validContentQuery(draft.content.query);
  ui.values[ContentText]->setInvalid(!draft.content.query.empty() && !queryValid);
  ui.submit->setEnabled(queryValid && ui.backends.supports(draft.content.mode));

  ui.values[SizeFirst]->inputArea()->setTooltip(
      invalidSizes      ? "Enter valid sizes in ascending order"
          : sizeBetween ? "Minimum size"
                        : "File size"
  );
  ui.values[SizeSecond]->inputArea()->setTooltip(
      invalidSizes ? "Enter valid sizes in ascending order" : "Maximum size"
  );
  ui.values[RelativeAmount]->inputArea()->setTooltip("Enter a valid positive time period");
  const bool invalidDates =
      !relative && draft.dateFirst.year && (!dateBetween || draft.dateSecond.year) && !filters.modified;
  ui.dates[0]->setTooltip(invalidDates ? "Select valid dates in ascending order" : "Select the modified date");
  ui.dates[1]->setTooltip(invalidDates ? "Select valid dates in ascending order" : "Select the end date");
  ui.choices[TextMode]->setTooltip(
      !rg ? "Content search requires ripgrep (rg)"
          : !rga ? "Extracted text requires ripgrep-all (rga). Plain-text search is available."
                 : "Extraction uses available adapters and searches up to three archive levels"
  );
  ui.values[ContentText]->inputArea()->setTooltip(
      !draft.content.query.empty() && !queryValid               ? "Use one line of up to 8,192 bytes"
          : draft.submitted && draft.submitted != draft.content ? "Search or Enter applies changes to the content query"
                                                                : "Press Search or Enter to apply the content query"
  );
}

void FileManager::submitSearchFilters() {
  auto& session = activeSession();
  auto& draft = session.searchFilters();
  if (filemanager::validContentQuery(draft.content.query) && m_searchFilterUi.backends.supports(draft.content.mode)) {
    draft.submitted = draft.content;
    refreshSearchQuery(session.search().query());
  }
  session.submitSearch();
}

void FileManager::openSearchFilterMenu(std::size_t field) {
  const auto selected = selection(activeSession().searchFilters())[field];
  std::vector<ContextMenuControlEntry> entries;
  for (std::size_t index = 0; index < kChoices[field].size(); ++index) {
    entries.push_back({
        .id = kFirstFilterChoice + static_cast<int>(field) * kChoiceStride + static_cast<int>(index),
        .label = std::string(kChoices[field][index]),
        .enabled = field != TextMode || m_searchFilterUi.backends.supports(static_cast<ContentMode>(index)),
        .radio = true,
        .toggleState = index == selected ? 1 : 0,
    });
  }
  m_menus.openSearchDropdown(
      std::move(entries), *m_searchFilterUi.choices[field],
      m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard
  );
}

bool FileManager::activateSearchFilterEntry(const ContextMenuControlEntry& entry) {
  const auto id = entry.id - kFirstFilterChoice;
  if (id < 0 || id >= static_cast<int>(kChoices.size()) * kChoiceStride)
    return false;
  const auto field = static_cast<std::size_t>(id / kChoiceStride);
  const auto value = static_cast<std::size_t>(id % kChoiceStride);
  auto& session = activeSession();
  if (!session.searchOpen() || !session.searchFiltersExpanded() || value >= kChoices[field].size())
    return true;
  if (field == TextMode) {
    if (m_searchFilterUi.backends.supports(static_cast<ContentMode>(value)))
      session.searchFilters().content.mode = static_cast<ContentMode>(value);
    syncSearchFilters();
  } else {
    changeSearchMetadata([&](auto& draft) {
      switch (field) {
      case SizeOp:
        draft.sizeComparison = static_cast<SizeComparison>(value);
        break;
      case SizeFirstUnit:
        draft.sizeFirstUnit = static_cast<SizeUnit>(value);
        break;
      case SizeSecondUnit:
        draft.sizeSecondUnit = static_cast<SizeUnit>(value);
        break;
      case DateOp:
        draft.dateComparison = static_cast<DateComparison>(value);
        break;
      case RelativeUnit:
        draft.dateUnit = static_cast<DateUnit>(value);
        break;
      }
    });
  }
  return true;
}

void FileManager::openSearchDatePicker(bool second) {
  auto& host = m_modalDialog;
  if (host.isOpen())
    return;
  struct Picker {
    SearchDate selected;
    int offset = 0;
    Flex* days = nullptr;
    Label* month = nullptr;
  };
  const auto& draft = activeSession().searchFilters();
  auto picker = std::make_shared<Picker>();
  picker->selected = second ? draft.dateSecond : draft.dateFirst;
  const auto today = calendar_view::stateForOffset(0).current;
  if (picker->selected.year)
    picker->offset = (picker->selected.year - today.year) * 12 + picker->selected.month - today.month - 1;
  const auto origin = activeTab().id;
  const auto rebuild = [picker, &host] {
    calendar_view::rebuildMonth({
        .grid = *picker->days,
        .monthLabel = *picker->month,
        .selected = {picker->selected.year, picker->selected.month - 1, picker->selected.day},
        .monthOffset = picker->offset,
        .layout =
            {.width = 320.0F,
             .weekdayHeight = 24.0F,
             .dayCellHeight = 34.0F,
             .dayButtonSize = 32.0F,
             .gap = Style::spaceXs},
        .onDateSelected = [picker, &host](calendar_view::Date day, int) {
          picker->selected = {day.year, day.month + 1, day.day};
          host.closeDialog(1);
        },
    });
    host.requestLayout();
  };
  ModalDialogHost::CustomContent content{
      .buildFields =
          [picker, rebuild, today](Flex& dialog, float) {
            auto& navigation = row(dialog);
            const auto navigate = [&](std::string_view glyph, std::string_view title, int delta) {
              auto button = std::make_unique<Button>();
              button->setVariant(ButtonVariant::Ghost);
              button->setGlyph(glyph);
              button->setTooltip(title);
              button->setOnClick([picker, rebuild, today, delta] {
                const auto minimum = (1 - today.year) * 12 - today.month;
                const auto maximum = (9999 - today.year) * 12 + 11 - today.month;
                picker->offset = std::clamp(picker->offset + delta, minimum, maximum);
                rebuild();
              });
              navigation.addChild(std::move(button));
            };
            navigate("chevrons-left", "Previous year", -12);
            navigate("chevron-left", "Previous month", -1);
            auto label = std::make_unique<Label>();
            label->setTextAlign(TextAlign::Center);
            label->setFlexGrow(1.0F);
            picker->month = label.get();
            navigation.addChild(std::move(label));
            navigate("chevron-right", "Next month", 1);
            navigate("chevrons-right", "Next year", 12);
            auto days = std::make_unique<Flex>();
            days->setDirection(FlexDirection::Vertical);
            picker->days = days.get();
            dialog.addChild(std::move(days));
            rebuild();
          },
      .useMeasuredMinHeight = true,
      .preferredWidth = 380,
      .minWidth = 380,
  };
  std::vector<std::pair<std::string, int>> actions{{"Cancel", 0}};
  if (picker->selected.year)
    actions.push_back({"Clear date", 2});
  host.showDialog(
      second ? "Choose end date" : "Choose modified date", "", std::move(actions),
      [this, picker, origin, second](int result) {
        if ((result != 1 && result != 2) || !m_windowOpen || activeTab().id != origin || !activeSession().searchOpen())
          return;
        changeSearchMetadata([&](auto& filters) {
          (second ? filters.dateSecond : filters.dateFirst) = result == 1 ? picker->selected : SearchDate{};
        });
      },
      nullptr, false, false, std::move(content)
  );
}
