#include "filemanager/file_columns.h"
#include "filemanager/fzf_search.h"
#include "filemanager/search_scope.h"
#include "filemanager/settings/settings_pages.h"
#include "filemanager/settings/settings_presentation.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <vector>

namespace filemanager {

  void buildSearchSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    auto* matchingCard = addPreferenceCard(parent, "Matching & Ranking", "adjustments", ctx.scale);

    addPreferenceRow(
        *matchingCard, ctx,
        {
            .title = "Enable fzf matching",
            .subtitle = "Use fzf for fast interactive filtering and ranked search results when installed.",
            .control = ui::toggle({
                .checked = ctx.settings.fzfSearchEnabled(),
                .scale = ctx.scale,
                .onChange =
                    [mutate = ctx.mutate](bool checked) {
                      mutate([checked](FileManagerSettings& settings) {
                        return settings.setFzfSearchEnabled(checked);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "fzf_search"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "fzf_search"}); });
            },
        }
    );

    const auto currentMode = ctx.settings.searchDefaultMode();
    std::vector<std::string> modeLabels;
    modeLabels.reserve(kSearchModeLabels.size());
    for (const auto label : kSearchModeLabels)
      modeLabels.emplace_back(label);
    const std::size_t modeIndex =
        static_cast<std::size_t>(currentMode) < kSearchModeLabels.size() ? static_cast<std::size_t>(currentMode) : 0;
    addPreferenceRow(
        *matchingCard, ctx,
        {
            .title = "Default matching mode",
            .subtitle = "Matching strategy used when fzf search starts without an explicit prefix operator.",
            .control = ui::select({
                .options = std::move(modeLabels),
                .selectedIndex = modeIndex,
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 170.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate](std::size_t index, std::string_view) {
                      if (index >= kSearchModeLabels.size())
                        return;
                      const auto mode = static_cast<SearchMode>(index);
                      mutate([mode](FileManagerSettings& settings) { return settings.setSearchDefaultMode(mode); });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "search_default_mode"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "search_default_mode"}); });
            },
        }
    );

    auto* resultsCard = addPreferenceCard(parent, "Search Results Presentation", "list-details", ctx.scale);
    const auto searchView = ctx.settings.loadFileView(true);
    const float searchScale = ctx.settings.loadViewScale(1.0F, true);

    addPreferenceRow(
        *resultsCard, ctx,
        {
            .title = "Results view mode",
            .subtitle = "Presentation used when displaying search results.",
            .control = ui::select({
                .options = std::vector<std::string>{"List", "Grid"},
                .selectedIndex = searchView.gridMode ? std::size_t{1} : std::size_t{0},
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 160.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate, searchView](std::size_t index, std::string_view) {
                      auto next = searchView;
                      next.gridMode = index == 1;
                      mutate([next, searchView](FileManagerSettings& settings) {
                        return settings.saveFileView(next, searchView, true);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "search_grid_view"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "search_grid_view"}); });
            },
        }
    );

    constexpr std::array<FileDialogSortField, 5> searchSortFields{
        FileDialogSortField::Name, FileDialogSortField::Size,     FileDialogSortField::Modified,
        FileDialogSortField::Type, FileDialogSortField::Location,
    };
    std::size_t searchSortIndex = 0;
    for (std::size_t i = 0; i < searchSortFields.size(); ++i) {
      if (searchSortFields[i] == searchView.sortField) {
        searchSortIndex = i;
        break;
      }
    }
    addPreferenceRow(
        *resultsCard, ctx,
        {
            .title = "Sort results by",
            .subtitle = "Default attribute used to order search results.",
            .control = ui::select({
                .options = std::vector<std::string>{"Name", "Size", "Date Modified", "Type", "Location"},
                .selectedIndex = searchSortIndex,
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 170.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate, searchView, searchSortFields](std::size_t index, std::string_view) {
                      if (index >= searchSortFields.size())
                        return;
                      auto next = searchView;
                      next.sortField = searchSortFields[index];
                      mutate([next, searchView](FileManagerSettings& settings) {
                        return settings.saveFileView(next, searchView, true);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "search_sort_field"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "search_sort_field"}); });
            },
        }
    );

    addPreferenceRow(
        *resultsCard, ctx,
        {
            .title = "Results sort order",
            .subtitle = "Order search results ascending or descending.",
            .control = ui::select({
                .options = std::vector<std::string>{"Ascending", "Descending"},
                .selectedIndex =
                    searchView.sortOrder == FileDialogSortOrder::Descending ? std::size_t{1} : std::size_t{0},
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 160.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate, searchView](std::size_t index, std::string_view) {
                      auto next = searchView;
                      next.sortOrder = index == 1 ? FileDialogSortOrder::Descending : FileDialogSortOrder::Ascending;
                      mutate([next, searchView](FileManagerSettings& settings) {
                        return settings.saveFileView(next, searchView, true);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "search_sort_descending"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "search_sort_descending"}); });
            },
        }
    );

    Slider* scaleSlider = nullptr;
    Label* scaleLabel = nullptr;
    auto scaleSliderNode = ui::slider({
        .out = &scaleSlider,
        .minValue = 80.0,
        .maxValue = 180.0,
        .step = 5.0,
        .value = static_cast<double>(std::round(searchScale * 100.0F)),
        .width = 160.0F * ctx.scale,
    });
    auto scaleLabelNode = ui::label({
        .out = &scaleLabel,
        .text = std::format("{}%", static_cast<int>(std::lround(searchScale * 100.0F))),
        .fontSize = Style::fontSizeCaption * ctx.scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .minWidth = 44.0F * ctx.scale,
    });
    scaleSlider->setOnValueChanged([scaleLabel](double value) {
      if (scaleLabel != nullptr)
        scaleLabel->setText(std::format("{}%", static_cast<int>(std::lround(value))));
    });
    scaleSlider->setOnDragEnd([mutate = ctx.mutate, scaleSlider, scaleLabel] {
      const float clamped = std::clamp(static_cast<float>(scaleSlider->value()) / 100.0F, 0.8F, 1.8F);
      if (scaleLabel != nullptr)
        scaleLabel->setText(std::format("{}%", static_cast<int>(std::lround(clamped * 100.0F))));
      mutate([clamped](FileManagerSettings& settings) { return settings.saveViewScale(clamped, true); });
    });
    auto scaleControl = ui::row(
        {
            .align = FlexAlign::Center,
            .gap = Style::spaceSm * ctx.scale,
        },
        std::move(scaleSliderNode), std::move(scaleLabelNode)
    );
    addPreferenceRow(
        *resultsCard, ctx,
        {
            .title = "Results view scale",
            .subtitle = "Independent zoom factor for search results (80% – 180%).",
            .control = std::move(scaleControl),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "search_view_scale"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "search_view_scale"}); });
            },
        }
    );

    FileColumnLayout searchLayout = kDefaultSearchColumnLayout;
    ctx.settings.loadListColumnLayout(searchLayout, true);
    auto columnsWrap = ui::row({
        .align = FlexAlign::Center,
        .wrap = true,
        .gap = Style::spaceMd * ctx.scale,
    });
    for (std::size_t i = 0; i < kFileColumns.size(); ++i) {
      const auto column = kFileColumns[i];
      if (column == FileColumn::Accessed)
        continue;
      const bool isName = column == FileColumn::Name;
      const std::string_view label = column == FileColumn::Created ? "Location" : kFileColumnLabels[i];
      auto check = std::make_unique<Checkbox>();
      check->setScale(ctx.scale);
      check->setChecked(searchLayout.visible[i]);
      check->setEnabled(!isName);
      check->setOnChange([mutate = ctx.mutate, searchLayout, i](bool checked) {
        auto next = searchLayout;
        next.visible[i] = checked;
        next.visible[columnIndex(FileColumn::Accessed)] = false;
        mutate([next](FileManagerSettings& settings) { return settings.saveListColumnLayout(next, true); });
      });
      columnsWrap->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale}, std::move(check),
              ui::label({
                  .text = std::string(label),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .color = colorSpecFromRole(isName ? ColorRole::OnSurfaceVariant : ColorRole::OnSurface),
              })
          )
      );
    }
    const bool hasSearchColumnOverrides = ctx.settings.hasPreferenceKey({"files", "search_column_layout"})
        || ctx.settings.hasPreferenceKey({"files", "search_column_widths"});
    if (!ctx.modifiedOnly || hasSearchColumnOverrides)
      resultsCard->addChild(std::move(columnsWrap));
    addPreferenceRow(
        *resultsCard, ctx,
        {
            .title = "Search result columns",
            .subtitle = "Reset search result column visibility, order, and widths to defaults.",
            .control = ui::button({
                .text = "Reset Columns",
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .enabled = hasSearchColumnOverrides,
                .variant = ButtonVariant::Default,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick =
                    [mutate = ctx.mutate] {
                      mutate([](FileManagerSettings& settings) {
                        const std::array<nythera::config::ConfigKeyPath, 2> paths{{
                            {"files", "search_column_layout"},
                            {"files", "search_column_widths"},
                        }};
                        return settings.resetKeys(paths);
                      });
                    },
            }),
            .scale = ctx.scale,
        },
        0.F, hasSearchColumnOverrides
    );

    const auto& searchSettings = ctx.settings.searchSettings();
    auto* exclusionsCard = addPreferenceCard(parent, "Global Search Exclusions", "filter-off", ctx.scale);
    if (!ctx.modifiedOnly || !searchSettings.exclusions.empty()) {
      exclusionsCard->addChild(
          ui::label({
              .text = "Absolute paths excluded from all searches and custom search profiles.",
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 0,
          })
      );

      std::vector<std::string> exclusionItems;
      exclusionItems.reserve(searchSettings.exclusions.size());
      for (const auto& path : searchSettings.exclusions)
        exclusionItems.push_back(path.string());

      auto editor = std::make_unique<ListEditor>();
      editor->setScale(ctx.scale);
      editor->setAddPlaceholder("/path/to/exclude");
      editor->setItems(exclusionItems);
      editor->setOnAddRequested([mutateSearch = ctx.mutateSearch, showStatus = ctx.showStatus,
                                 exclusions = searchSettings.exclusions](std::string text) {
        if (!validSearchLocation(text)) {
          if (showStatus)
            showStatus("Excluded paths must be non-empty absolute paths.", true);
          return;
        }
        auto next = exclusions;
        std::filesystem::path candidate(text);
        if (std::ranges::contains(next, candidate)) {
          if (showStatus)
            showStatus("That path is already in global exclusions.", false);
          return;
        }
        next.push_back(std::move(candidate));
        mutateSearch([next = std::move(next)](FileManagerSettings& settings) {
          return settings.saveSearchExclusions(next);
        });
      });
      editor->setOnRemoveRequested([mutateSearch = ctx.mutateSearch,
                                    exclusions = searchSettings.exclusions](std::size_t index) {
        if (index >= exclusions.size())
          return;
        auto next = exclusions;
        next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
        mutateSearch([next = std::move(next)](FileManagerSettings& settings) {
          return settings.saveSearchExclusions(next);
        });
      });
      exclusionsCard->addChild(std::move(editor));
    }

    auto* profilesCard = addPreferenceCard(parent, "Custom Search Scopes", "folder-search", ctx.scale);
    addPreferenceRow(
        *profilesCard, ctx,
        {
            .title = "Named search profiles",
            .subtitle = "Manage saved custom search locations and per-profile exclusions.",
            .control = ui::button({
                .text = "Manage Profiles…",
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Default,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick =
                    [manage = ctx.manageCustomSearches] {
                      if (manage)
                        manage();
                    },
            }),
            .scale = ctx.scale,
        },
        0.F, !searchSettings.profiles.empty()
    );

    if (ctx.modifiedOnly && searchSettings.profiles.empty())
      return;
    if (searchSettings.profiles.empty()) {
      profilesCard->addChild(
          ui::label({
              .text = "No custom search profiles defined.",
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      for (const auto& profile : searchSettings.profiles) {
        addPreferenceRow(
            *profilesCard, ctx,
            {
                .title = profile.name,
                .subtitle = std::format(
                    "{} included path{} · {} excluded", profile.included.size(),
                    profile.included.size() == 1 ? "" : "s", profile.excluded.size()
                ),
                .control = ui::row(
                    {.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale},
                    ui::button({
                        .glyph = "edit",
                        .glyphSize = Style::fontSizeCaption * ctx.scale,
                        .variant = ButtonVariant::Ghost,
                        .tooltip = "Edit custom search profile",
                        .minWidth = Style::controlHeightSm * ctx.scale,
                        .minHeight = Style::controlHeightSm * ctx.scale,
                        .padding = Style::spaceXs * ctx.scale,
                        .radius = Style::scaledRadiusSm(ctx.scale),
                        .onClick =
                            [edit = ctx.editCustomSearch, profile] {
                              if (edit)
                                edit(profile);
                            },
                    }),
                    ui::button({
                        .glyph = "trash",
                        .glyphSize = Style::fontSizeCaption * ctx.scale,
                        .variant = ButtonVariant::Ghost,
                        .tooltip = "Remove custom search profile",
                        .minWidth = Style::controlHeightSm * ctx.scale,
                        .minHeight = Style::controlHeightSm * ctx.scale,
                        .padding = Style::spaceXs * ctx.scale,
                        .radius = Style::scaledRadiusSm(ctx.scale),
                        .onClick =
                            [mutateSearch = ctx.mutateSearch, profile] {
                              mutateSearch([profile](FileManagerSettings& settings) {
                                return settings.editCustomSearch(profile, std::nullopt);
                              });
                            },
                    })
                ),
                .scale = ctx.scale,
            },
            0.F, true
        );
      }
    }
  }

} // namespace filemanager
