#include "filemanager/search_bar_controller.h"

#include "filemanager/dialogs/search_date_picker.h"
#include "filemanager/menu_ids.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <format>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace filemanager {

  namespace {

    enum Choice : std::size_t { SizeOp, SizeFirstUnit, SizeSecondUnit, DateOp, RelativeUnit, TextMode };
    enum Value : std::size_t { SizeFirst, SizeSecond, RelativeAmount, ContentText };
    enum Check : std::size_t { CaseSensitive, Regex };

    constexpr float kNumericWidth = 98.0F;
    constexpr float kContentInputMinWidth = 140.0F;
    constexpr float kMetadataSeparatorGap = Style::spaceLg;

    constexpr int kFirstFilterChoice = menu_id::kFirstSearchFilterChoice;
    constexpr int kChoiceStride = menu_id::kSearchFilterChoiceStride;
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

    bool driveScope(SearchScope scope) { return scope == SearchScope::Local || scope == SearchScope::All; }

  } // namespace

  SearchBarController::SearchBarController(
      WaylandConnection& wayland, AnimationManager& animations, InputDispatcher& dispatcher,
      FileManagerSettings& settings, TabManager& tabs, FileListHeaderController& listHeader, FileMenuController& menus,
      ModalDialogHost& modalDialog, CustomSearchDialogController& customSearches, Callbacks callbacks
  )
      : m_wayland(wayland), m_animations(animations), m_dispatcher(dispatcher), m_settings(settings), m_tabs(tabs),
        m_listHeader(listHeader), m_menus(menus), m_modalDialog(modalDialog), m_customSearches(customSearches),
        m_callbacks(std::move(callbacks)) {}

  void SearchBarController::detachUi() {
    m_searchBar = nullptr;
    m_searchTools = nullptr;
    m_searchFilterUi = {};
    m_searchInput = nullptr;
    m_searchButton = nullptr;
    m_searchModeButton = nullptr;
    m_searchScopeButton = nullptr;
    m_searchStatus = nullptr;
    m_searchSpinner = nullptr;
    m_searchStatusArea = nullptr;
  }

  void SearchBarController::setStatusFontSize(float fontSize) {
    if (m_searchStatus)
      m_searchStatus->setFontSize(fontSize);
    if (m_searchSpinner)
      m_searchSpinner->setSpinnerSize(fontSize);
  }

  bool SearchBarController::ownsFocusedInput(const InputArea* focused) const {
    if (!focused)
      return false;
    if (m_searchInput && focused == m_searchInput->inputArea())
      return true;
    return std::ranges::any_of(m_searchFilterUi.values, [focused](const Input* input) {
      return input && focused == input->inputArea();
    });
  }

  void SearchBarController::refreshContentBackends() { m_searchFilterUi.backends = findContentBackends(); }

  void SearchBarController::focusInput() {
    if (m_searchInput) {
      m_dispatcher.setFocus(m_searchInput->inputArea());
      m_searchInput->selectAll();
    }
  }

  bool SearchBarController::isStatusAreaVisible() const { return m_searchStatusArea && m_searchStatusArea->visible(); }

  float SearchBarController::measureStatusAreaWidth(Renderer& renderer) const {
    return m_searchStatusArea ? m_searchStatusArea->measure(renderer, {}).width : 0.0F;
  }

  void SearchBarController::buildSearchBar(Flex& pane, float extraTopSpacing) {
    auto tools = std::make_unique<Flex>();
    tools->setDirection(FlexDirection::Vertical);
    tools->setAlign(FlexAlign::Stretch);
    tools->setGap(Style::spaceXs);
    tools->setPadding(extraTopSpacing, 0.0F, 0.0F, 0.0F);
    m_searchTools = static_cast<Flex*>(pane.addChild(std::move(tools)));
    auto bar = std::make_unique<Flex>();
    bar->setAlign(FlexAlign::Center);
    bar->setGap(Style::spaceSm);
    m_searchBar = static_cast<Flex*>(m_searchTools->addChild(std::move(bar)));

    auto input = std::make_unique<Input>();
    m_searchInput = input.get();
    input->setPlaceholder("Search this folder and subfolders");
    input->setFlexGrow(1.0F);
    input->setOnChange([this](const std::string& query) {
      if (m_callbacks.refreshSearchQuery)
        m_callbacks.refreshSearchQuery(query);
    });
    input->setOnSubmit([this](const std::string&) {
      submitFilters();
      if (m_callbacks.gridFocusArea) {
        if (auto* area = m_callbacks.gridFocusArea())
          m_dispatcher.setFocus(area);
      }
    });
    m_searchBar->addChild(std::move(input));

    auto scope = std::make_unique<Button>();
    m_searchScopeButton = scope.get();
    m_compactSearchScope = false;
    scope->setVariant(ButtonVariant::Outline);
    scope->setTrailingGlyph("chevron-down");
    scope->setFontSize(Style::fontSizeCaption);
    scope->setContentAlign(ButtonContentAlign::Start);
    scope->label()->setFlexGrow(1.0F);
    scope->label()->setEllipsize(TextEllipsize::End);
    scope->setOnClick([this] { openScopeMenu(); });
    m_searchBar->addChild(std::move(scope));

    auto mode = std::make_unique<Button>();
    m_searchModeButton = mode.get();
    mode->setVariant(ButtonVariant::Outline);
    mode->setTrailingGlyph("chevron-down");
    mode->setMinWidth(136.0F);
    mode->setMaxWidth(136.0F);
    mode->setContentAlign(ButtonContentAlign::Start);
    mode->label()->setFlexGrow(1.0F);
    mode->setTooltip("Search matching mode");
    mode->setOnClick([this] { openModeMenu(); });
    m_searchBar->addChild(std::move(mode));

    auto filter = std::make_unique<Button>();
    m_searchFilterUi.toggle = filter.get();
    filter->setVariant(ButtonVariant::Outline);
    filter->setGlyph("adjustments-horizontal");
    filter->setTooltip("Search filters");
    filter->setOnClick([this] {
      auto& session = m_tabs.activeTab().session;
      session.setSearchFiltersExpanded(!session.searchFiltersExpanded());
      syncFilters();
    });
    m_searchBar->addChild(std::move(filter));

    auto clear = std::make_unique<Button>();
    clear->setVariant(ButtonVariant::Ghost);
    clear->setGlyph("filter-x");
    clear->setTooltip("Clear Filters");
    clear->setOnClick([this] {
      auto& session = m_tabs.activeTab().session;
      session.searchFilters() = {};
      if (m_callbacks.refreshSearchQuery)
        m_callbacks.refreshSearchQuery(session.search().query());
    });
    m_searchFilterUi.clear = static_cast<Button*>(m_searchBar->addChild(std::move(clear)));

    auto close = std::make_unique<Button>();
    close->setVariant(ButtonVariant::Ghost);
    close->setGlyph("x");
    close->setTooltip("Close Search");
    close->setOnClick([this] {
      if (m_callbacks.closeSearch)
        m_callbacks.closeSearch();
    });
    m_searchBar->addChild(std::move(close));
    buildSearchFilters(*m_searchTools);

    m_searchBar->setVisible(false);
    m_searchTools->setVisible(false);
    m_searchTools->setParticipatesInLayout(false);
  }

  void SearchBarController::attachStatusArea(Flex& statusRow, float fontSize) {
    auto search = std::make_unique<Flex>();
    search->setGap(Style::spaceXs);
    search->setMirrorInRtl(false);
    auto spinner = std::make_unique<Spinner>();
    spinner->setSpinnerSize(fontSize);
    spinner->setVisible(false);
    m_searchSpinner = static_cast<Spinner*>(search->addChild(std::move(spinner)));
    m_searchSpinner->setAnimationManager(&m_animations);
    auto status = std::make_unique<Label>();
    status->setFontSize(fontSize);
    status->setMaxLines(1);
    status->setEllipsize(TextEllipsize::End);
    status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    status->setVisible(false);
    m_searchStatus = static_cast<Label*>(search->addChild(std::move(status)));
    m_searchStatusArea = static_cast<Flex*>(statusRow.addChild(std::move(search)));
    m_searchStatusArea->setVisible(false);
  }

  void SearchBarController::buildSearchFilters(Flex& pane) {
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
        m_tabs.activeTab().session.searchFilters().content.*member = checked;
        syncFilters();
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
      control->setOnClick([this, field] { openFilterMenu(field); });
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
        auto& session = m_tabs.activeTab().session;
        if (field != ContentText) {
          changeMetadata([&](auto& draft) { member(draft) = text; });
          return;
        }
        auto& draft = session.searchFilters();
        member(draft) = text;
        if (text.empty() && draft.submitted) {
          draft.submitted.reset();
          if (m_callbacks.refreshSearchQuery)
            m_callbacks.refreshSearchQuery(session.search().query());
        } else {
          syncFilters();
        }
      });
      control->setOnSubmit([this, field](const std::string&) {
        if (field == ContentText)
          submitFilters();
        else
          m_tabs.activeTab().session.submitSearch();
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
      button->setOnClick([this, index] { openDatePicker(index != 0); });
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
    submit->setOnClick([this] { submitFilters(); });
    ui.submit = submit.get();
    options.addChild(std::move(submit));

    show(*ui.panel, false);
  }

  void SearchBarController::syncUi() {
    auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    if (m_listHeader.searchView() != session.search().active() && m_callbacks.applyActiveView)
      m_callbacks.applyActiveView();
    updateStatus();
    if (!m_searchBar)
      return;
    const bool open = session.searchOpen();
    m_searchButton->setEnabled(!tab.administratorBrowsing && session.canSearch());
    m_searchButton->setSelected(open);
    if ((open != m_searchTools->visible() || m_searchUiTab != tab.id)) {
      m_searchUiTab = tab.id;
      m_searchInput->setValue(session.search().query());
      m_searchBar->setVisible(open);
      m_searchTools->setVisible(open);
      m_searchTools->setParticipatesInLayout(open);
      if (!open) {
        for (const Node* node = m_dispatcher.focusedArea(); node; node = node->parent()) {
          if (node == m_searchTools) {
            m_dispatcher.setFocus(m_callbacks.gridFocusArea ? m_callbacks.gridFocusArea() : nullptr);
            break;
          }
        }
      }
    }
    syncFilters();
    if (!open)
      return;

    syncScopeButton();
    m_searchInput->setPlaceholder(
        session.searchSelection().scope == SearchScope::Recursive ? "Search this folder and subfolders"
                                                                  : "Search files and folders"
    );

    const bool fzf = session.fzfSearchActive();
    m_searchModeButton->setVisible(fzf);
    m_searchModeButton->setParticipatesInLayout(fzf);
    if (fzf)
      m_searchModeButton->setText(kSearchModeLabels[static_cast<std::size_t>(session.searchMode())]);
  }

  void SearchBarController::syncFilters() {
    auto& ui = m_searchFilterUi;
    auto& session = m_tabs.activeTab().session;
    const auto& draft = session.searchFilters();
    const bool expanded = session.searchOpen() && session.searchFiltersExpanded();
    if (ui.panel->visible() != expanded) {
      if (!expanded) {
        m_menus.closeContextMenu();
        for (const Node* node = m_dispatcher.focusedArea(); node; node = node->parent()) {
          if (node == ui.panel) {
            m_dispatcher.setFocus(
                session.searchOpen() ? m_searchInput->inputArea()
                                     : (m_callbacks.gridFocusArea ? m_callbacks.gridFocusArea() : nullptr)
            );
            break;
          }
        }
      }
      show(*ui.panel, expanded);
      if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
        surface->requestLayout();
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

    const bool relative =
        draft.dateComparison == DateComparison::Within || draft.dateComparison == DateComparison::Older;
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
        !draft.content.query.empty() && !queryValid ? "Use one line of up to 8,192 bytes"
            : draft.submitted && draft.submitted != draft.content
            ? "Search or Enter applies changes to the content query"
            : "Press Search or Enter to apply the content query"
    );
  }

  void SearchBarController::syncScopeButton() {
    const auto& selection = m_tabs.activeTab().session.searchSelection();
    const auto label = selection.scope == SearchScope::Custom
        ? std::string_view(selection.profile)
        : kSearchScopeLabels[static_cast<std::size_t>(selection.scope)];
    const float width = m_compactSearchScope ? 78.0F : 196.0F;
    m_searchScopeButton->setMinWidth(width);
    m_searchScopeButton->setMaxWidth(width);
    m_searchScopeButton->setText(m_compactSearchScope ? "Scope" : label);
    m_searchScopeButton->setTooltip(
        selection.includeSystem ? std::string(label) + " · Includes system directories" : std::string(label)
    );
  }

  void SearchBarController::updateStatus() {
    if (!m_searchStatusArea)
      return;
    using Status = FileSearchSession::Status;
    const auto& search = m_tabs.activeTab().session.search();
    const auto& issues = search.contentIssues();
    const bool searching = search.status() == Status::Searching;
    const bool partial = search.skippedFolders() > 0 || issues.partial() || !search.contentNotice().empty();
    const auto countStr = [&](std::size_t n) {
      return m_callbacks.formatCount ? m_callbacks.formatCount(n) : std::to_string(n);
    };
    std::string status;
    switch (search.status()) {
    case Status::Idle:
      break;
    case Status::Searching:
      status = "Searching… " + countStr(search.entries().size()) + " found";
      break;
    case Status::Finished:
      status = search.entries().empty() ? "No matches" : countStr(search.entries().size()) + " found";
      if (partial)
        status = "Partial results · " + status;
      break;
    case Status::Cancelled:
      status = "Search cancelled · " + countStr(search.entries().size()) + " found";
      break;
    case Status::Failed:
      status = "Search failed";
      break;
    }

    std::vector<TooltipRow> warnings;
    const auto addCount = [&warnings, &countStr](std::size_t count, std::string_view label) {
      if (count)
        warnings.push_back({std::string(label), countStr(count)});
    };
    addCount(search.skippedFolders(), "Folders skipped");
    addCount(issues.sizeRestricted, "Over extraction size limit");
    addCount(issues.inaccessible, "Unreadable or changed");
    addCount(issues.extractionFailed, "Extraction failures");
    addCount(issues.extractionTimeouts, "Extraction timeouts");
    addCount(issues.plainTimeouts, "Plain-text batch timeouts");
    addCount(issues.archiveTruncated, "Archives at depth limit");
    addCount(issues.unsupported, "Unsupported formats");
    if (issues.backendError)
      warnings.push_back({"Content backend", "Error"});
    if (search.usingFallback())
      warnings.push_back({"fzf", "Unavailable"});

    m_searchStatus->setText(status);
    const auto currentTooltip = m_searchStatus->tooltipContent();
    if (status.empty()) {
      if (m_searchStatus->hasTooltip())
        m_searchStatus->clearTooltip();
    } else if (warnings.empty() || search.status() == Status::Failed) {
      std::string tooltip = status;
      const auto& detail = search.status() == Status::Failed ? search.error() : search.contentNotice();
      if (!detail.empty())
        tooltip += "\n" + detail;
      const auto* currentText = std::get_if<std::string>(&currentTooltip);
      if (!currentText || *currentText != tooltip)
        m_searchStatus->setTooltip(std::move(tooltip));
    } else {
      std::string_view notice = search.contentNotice();
      constexpr std::string_view separator = " · ";
      while (!notice.empty()) {
        const auto end = notice.find(separator);
        warnings.push_back({{}, std::string(notice.substr(0, end))});
        if (end == std::string_view::npos)
          break;
        notice.remove_prefix(end + separator.size());
      }
      warnings.insert(warnings.begin(), {"Matches", countStr(search.entries().size())});
      const auto* currentRows = std::get_if<std::vector<TooltipRow>>(&currentTooltip);
      if (!currentRows || !std::ranges::equal(*currentRows, warnings, [](const TooltipRow& a, const TooltipRow& b) {
            return a.key == b.key && a.value == b.value;
          })) {
        m_searchStatus->setTooltip(std::move(warnings));
      }
    }
    m_searchStatus->setVisible(!status.empty());
    m_searchStatusArea->setVisible(!status.empty());
    m_searchSpinner->setVisible(searching);
    if (searching)
      m_searchSpinner->start();
    else
      m_searchSpinner->stop();
  }

  void SearchBarController::updateScopeWidth(float paneWidth) {
    const auto& session = m_tabs.activeTab().session;
    if (!m_searchScopeButton || !session.searchOpen())
      return;
    const bool compact = paneWidth < (session.fzfSearchActive() ? 520.0F : 360.0F);
    if (m_compactSearchScope == compact)
      return;
    m_compactSearchScope = compact;
    syncScopeButton();
  }

  void SearchBarController::updateFilterWidth(float paneWidth, Renderer& renderer) {
    auto& ui = m_searchFilterUi;
    if (!ui.panel || !ui.panel->visible())
      return;
    const float width = paneWidth - ui.panel->paddingLeft() - ui.panel->paddingRight();
    auto* query = static_cast<Flex*>(ui.content->children().front().get());
    const float labelWidth = query->children().front()->measure(renderer, {}).width;
    ui.sizeCaption->setMinWidth(labelWidth);

    const bool metadataInline = ui.size->measure(renderer, {}).width
            + ui.date->measure(renderer, {}).width
            + 2.0F * kMetadataSeparatorGap
            + 1.0F
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

  void SearchBarController::layoutForFrame(
      float headerWidth, float fallbackScopeBudget, float statusWidth, Renderer& renderer, float& summaryWidth
  ) {
    updateFilterWidth(headerWidth, renderer);
    if (m_searchFilterUi.clear && m_searchBar) {
      const float scopeWidthBudget = m_searchFilterUi.clear->visible()
          ? headerWidth - m_searchFilterUi.clear->measure(renderer, {}).width - m_searchBar->gap()
          : fallbackScopeBudget;
      updateScopeWidth(scopeWidthBudget);
    }
    if (m_searchStatusArea && m_searchStatusArea->visible() && m_searchStatus) {
      m_searchStatus->setMaxWidth(std::floor(statusWidth * 0.45F));
      summaryWidth -= m_searchStatusArea->measure(renderer, {}).width + Style::spaceSm;
    }
  }

  void SearchBarController::openModeMenu() {
    const auto& session = m_tabs.activeTab().session;
    if (!m_searchModeButton || !session.searchOpen() || !session.fzfSearchActive())
      return;
    const auto current = static_cast<std::size_t>(session.searchMode());
    const auto& labels = kSearchModeLabels;
    std::vector<ContextMenuControlEntry> entries;
    for (std::size_t index = 0; index < labels.size(); ++index) {
      entries.push_back({
          .id = menu_id::kFirstSearchMode + static_cast<int>(index),
          .label = std::string(labels[index]),
          .radio = true,
          .toggleState = index == current ? 1 : 0,
      });
    }
    m_menus.openSearchDropdown(
        std::move(entries), *m_searchModeButton, m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard
    );
  }

  void SearchBarController::openScopeMenu() {
    const auto& session = m_tabs.activeTab().session;
    if (!m_searchScopeButton || !session.searchOpen())
      return;
    const auto& selection = session.searchSelection();
    const auto& labels = kSearchScopeLabels;
    std::vector<ContextMenuControlEntry> entries;
    for (auto index = 0; index < static_cast<int>(SearchScope::Custom); ++index) {
      entries.push_back({
          .id = menu_id::kFirstSearchScope + index,
          .label = std::string(labels[index]),
          .radio = true,
          .toggleState = index == static_cast<int>(selection.scope) ? 1 : 0,
      });
    }
    if (driveScope(selection.scope)) {
      entries.push_back({
          .id = menu_id::kSearchIncludeSystem,
          .label = "Include system directories",
          .checkmark = true,
          .toggleState = selection.includeSystem ? 1 : 0,
          .indentLevel = 1,
      });
    }
    std::vector<ContextMenuControlEntry> custom;
    for (const auto& profile : m_settings.searchSettings().profiles) {
      custom.push_back({
          .id = menu_id::kUseCustomSearch,
          .label = profile.name,
          .radio = true,
          .toggleState = selection.scope == SearchScope::Custom && selection.profile == profile.name ? 1 : 0,
      });
    }
    if (!custom.empty())
      custom.push_back({.separator = true});
    custom.push_back({.id = menu_id::kDefineCustomSearch, .label = "Define custom search…"});
    custom.push_back({.id = menu_id::kManageCustomSearches, .label = "Manage custom searches…"});
    entries.push_back({
        .label = std::string(labels.back()),
        .hasSubmenu = true,
        .radio = true,
        .toggleState = selection.scope == SearchScope::Custom ? 1 : 0,
        .submenuEntries = std::move(custom),
    });

    const auto options = session.searchOptions();
    entries.push_back({.separator = true});
    entries.push_back(
        {.id = menu_id::kSearchShowHidden,
         .label = "Show hidden items",
         .checkmark = true,
         .toggleState = options.showHidden ? 1 : 0}
    );
    entries.push_back(
        {.id = menu_id::kSearchFollowLinks,
         .label = "Follow symbolic links",
         .checkmark = true,
         .toggleState = options.followSymlinks ? 1 : 0}
    );
    m_menus.openSearchDropdown(
        std::move(entries), *m_searchScopeButton, m_wayland.lastInputSource() == WaylandSeat::InputSource::Keyboard
    );
  }

  void SearchBarController::openFilterMenu(std::size_t field) {
    const auto selected = selection(m_tabs.activeTab().session.searchFilters())[field];
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

  bool SearchBarController::activateScopeEntry(const ContextMenuControlEntry& entry) {
    using namespace menu_id;
    const bool option = entry.id == kSearchShowHidden || entry.id == kSearchFollowLinks;
    if (!option && (entry.id < kFirstSearchScope || entry.id > kManageCustomSearches))
      return false;
    const auto& session = m_tabs.activeTab().session;
    if (!session.searchOpen())
      return true;
    if (option) {
      if (m_callbacks.toggleSearchOption)
        m_callbacks.toggleSearchOption(
            entry.id == kSearchShowHidden ? &SearchOptions::showHidden : &SearchOptions::followSymlinks
        );
    } else if (entry.id < kFirstSearchScope + static_cast<int>(SearchScope::Custom)) {
      const auto scope = static_cast<SearchScope>(entry.id - kFirstSearchScope);
      const auto& previous = session.searchSelection();
      if (m_callbacks.selectSearchScope)
        m_callbacks.selectSearchScope(
            {.scope = scope, .includeSystem = driveScope(scope) && driveScope(previous.scope) && previous.includeSystem}
        );
    } else if (entry.id == kSearchIncludeSystem) {
      auto selection = session.searchSelection();
      if (driveScope(selection.scope) && m_callbacks.selectSearchScope) {
        selection.includeSystem = !selection.includeSystem;
        m_callbacks.selectSearchScope(std::move(selection));
      }
    } else if (entry.id == kUseCustomSearch) {
      if (m_callbacks.selectSearchScope)
        m_callbacks.selectSearchScope({.scope = SearchScope::Custom, .profile = entry.label});
    } else if (entry.id == kDefineCustomSearch) {
      m_customSearches.editCustomSearch(std::nullopt);
    } else if (entry.id == kManageCustomSearches) {
      m_customSearches.manageCustomSearches();
    }
    return true;
  }

  bool SearchBarController::activateFilterEntry(const ContextMenuControlEntry& entry) {
    const auto id = entry.id - kFirstFilterChoice;
    if (id < 0 || id >= static_cast<int>(kChoices.size()) * kChoiceStride)
      return false;
    const auto field = static_cast<std::size_t>(id / kChoiceStride);
    const auto value = static_cast<std::size_t>(id % kChoiceStride);
    auto& session = m_tabs.activeTab().session;
    if (!session.searchOpen() || !session.searchFiltersExpanded() || value >= kChoices[field].size())
      return true;
    if (field == TextMode) {
      if (m_searchFilterUi.backends.supports(static_cast<ContentMode>(value)))
        session.searchFilters().content.mode = static_cast<ContentMode>(value);
      syncFilters();
    } else {
      changeMetadata([&](auto& draft) {
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

  void SearchBarController::submitFilters() {
    auto& session = m_tabs.activeTab().session;
    auto& draft = session.searchFilters();
    if (validContentQuery(draft.content.query) && m_searchFilterUi.backends.supports(draft.content.mode)) {
      draft.submitted = draft.content;
      if (m_callbacks.refreshSearchQuery)
        m_callbacks.refreshSearchQuery(session.search().query());
    }
    session.submitSearch();
  }

  void SearchBarController::changeMetadata(const std::function<void(SearchFilterDraft&)>& change) {
    auto& session = m_tabs.activeTab().session;
    auto& draft = session.searchFilters();
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    const auto size = resolveSizeFilter(draft);
    const auto modified = resolveModifiedFilter(draft, now);
    change(draft);
    if (size != resolveSizeFilter(draft) || modified != resolveModifiedFilter(draft, now)) {
      if (m_callbacks.refreshSearchQuery)
        m_callbacks.refreshSearchQuery(session.search().query());
    } else {
      syncFilters();
    }
  }

  void SearchBarController::openDatePicker(bool second) {
    const auto& draft = m_tabs.activeTab().session.searchFilters();
    const auto origin = m_tabs.activeTab().id;
    showSearchDatePicker(
        m_modalDialog, second ? draft.dateSecond : draft.dateFirst, second,
        [this, origin, second](std::optional<SearchDate> selected) {
          const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
          if (!windowOpen || m_tabs.activeTab().id != origin || !m_tabs.activeTab().session.searchOpen())
            return;
          changeMetadata([&](auto& filters) {
            (second ? filters.dateSecond : filters.dateFirst) = selected.value_or(SearchDate{});
          });
        }
    );
  }

} // namespace filemanager
