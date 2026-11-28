#include "filemanager/file_columns.h"
#include "filemanager/settings/settings_pages.h"
#include "filemanager/settings/settings_presentation.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <vector>

namespace filemanager {

  void addViewScalePreference(Flex& card, const SettingsPageContext& ctx) {
    const auto viewScale = ctx.settings.loadViewScale();
    Slider* scaleSlider = nullptr;
    Label* scaleLabel = nullptr;
    auto scaleSliderNode = ui::slider({
        .out = &scaleSlider,
        .minValue = 80.0,
        .maxValue = 180.0,
        .step = 5.0,
        .value = static_cast<double>(std::round(viewScale * 100.0F)),
        .width = 160.0F * ctx.scale,
    });
    auto scaleLabelNode = ui::label({
        .out = &scaleLabel,
        .text = std::format("{}%", static_cast<int>(std::lround(viewScale * 100.0F))),
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
      mutate([clamped](FileManagerSettings& settings) { return settings.saveViewScale(clamped, false); });
    });
    auto scaleControl = ui::row(
        {
            .align = FlexAlign::Center,
            .gap = Style::spaceSm * ctx.scale,
        },
        std::move(scaleSliderNode), std::move(scaleLabelNode)
    );
    addPreferenceRow(
        card, ctx,
        {
            .title = "View scale",
            .subtitle = "Default zoom factor for file lists and icon grids (80% – 180%).",
            .control = std::move(scaleControl),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "view_scale"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "view_scale"}); });
            },
        }
    );
  }

  void buildFilesFoldersSettingsPage(Flex& parent, const SettingsPageContext& ctx) {
    const auto view = ctx.settings.loadFileView(false);

    auto* viewCard = addPreferenceCard(parent, "Default View", "layout-grid", ctx.scale);

    addPreferenceRow(
        *viewCard, ctx,
        {
            .title = "View mode",
            .subtitle = "Default presentation for folder views.",
            .control = ui::select({
                .options = std::vector<std::string>{"List", "Grid"},
                .selectedIndex = view.gridMode ? std::size_t{1} : std::size_t{0},
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 160.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate, view](std::size_t index, std::string_view) {
                      auto next = view;
                      next.gridMode = index == 1;
                      mutate([next, view](FileManagerSettings& settings) {
                        return settings.saveFileView(next, view, false);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "grid_view"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "grid_view"}); });
            },
        }
    );

    constexpr std::array<FileDialogSortField, 6> sortFields{
        FileDialogSortField::Name,    FileDialogSortField::Size,     FileDialogSortField::Modified,
        FileDialogSortField::Created, FileDialogSortField::Accessed, FileDialogSortField::Type,
    };
    std::size_t sortIndex = 0;
    for (std::size_t i = 0; i < sortFields.size(); ++i) {
      if (sortFields[i] == view.sortField) {
        sortIndex = i;
        break;
      }
    }
    addPreferenceRow(
        *viewCard, ctx,
        {
            .title = "Sort by",
            .subtitle = "Default attribute used to order files and folders.",
            .control = ui::select({
                .options =
                    std::vector<std::string>{"Name", "Size", "Date Modified", "Date Created", "Date Accessed", "Type"},
                .selectedIndex = sortIndex,
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 180.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate, view, sortFields](std::size_t index, std::string_view) {
                      if (index >= sortFields.size())
                        return;
                      auto next = view;
                      next.sortField = sortFields[index];
                      mutate([next, view](FileManagerSettings& settings) {
                        return settings.saveFileView(next, view, false);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "sort_field"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "sort_field"}); });
            },
        }
    );

    addPreferenceRow(
        *viewCard, ctx,
        {
            .title = "Sort order",
            .subtitle = "Order entries ascending or descending.",
            .control = ui::select({
                .options = std::vector<std::string>{"Ascending", "Descending"},
                .selectedIndex = view.sortOrder == FileDialogSortOrder::Descending ? std::size_t{1} : std::size_t{0},
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 160.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate, view](std::size_t index, std::string_view) {
                      auto next = view;
                      next.sortOrder = index == 1 ? FileDialogSortOrder::Descending : FileDialogSortOrder::Ascending;
                      mutate([next, view](FileManagerSettings& settings) {
                        return settings.saveFileView(next, view, false);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "sort_descending"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "sort_descending"}); });
            },
        }
    );

    addPreferenceRow(
        *viewCard, ctx,
        {
            .title = "Show folders first",
            .subtitle = "Group directories ahead of files when sorting.",
            .control = ui::toggle({
                .checked = view.foldersFirst,
                .scale = ctx.scale,
                .onChange =
                    [mutate = ctx.mutate, view](bool checked) {
                      auto next = view;
                      next.foldersFirst = checked;
                      mutate([next, view](FileManagerSettings& settings) {
                        return settings.saveFileView(next, view, false);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "folders_first"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "folders_first"}); });
            },
        }
    );

    addPreferenceRow(
        *viewCard, ctx,
        {
            .title = "Hidden items last",
            .subtitle = "Place dotfiles after visible entries when hidden files are shown.",
            .control = ui::toggle({
                .checked = view.hiddenLast,
                .scale = ctx.scale,
                .onChange =
                    [mutate = ctx.mutate, view](bool checked) {
                      auto next = view;
                      next.hiddenLast = checked;
                      mutate([next, view](FileManagerSettings& settings) {
                        return settings.saveFileView(next, view, false);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "hidden_last"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "hidden_last"}); });
            },
        }
    );

    addPreferenceRow(
        *viewCard, ctx,
        {
            .title = "Show hidden files",
            .subtitle = "Display entries whose names start with a dot.",
            .control = ui::toggle({
                .checked = view.showHidden,
                .scale = ctx.scale,
                .onChange =
                    [mutate = ctx.mutate, view](bool checked) {
                      auto next = view;
                      next.showHidden = checked;
                      mutate([next, view](FileManagerSettings& settings) {
                        return settings.saveFileView(next, view, false);
                      });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "show_hidden"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "show_hidden"}); });
            },
        }
    );

    addViewScalePreference(*viewCard, ctx);

    auto* columnsCard = addPreferenceCard(parent, "List Columns", "columns", ctx.scale);
    FileColumnLayout layout;
    ctx.settings.loadListColumnLayout(layout, false);
    auto columnsWrap = ui::row({
        .align = FlexAlign::Center,
        .wrap = true,
        .gap = Style::spaceMd * ctx.scale,
    });
    for (std::size_t i = 0; i < kFileColumns.size(); ++i) {
      const auto column = kFileColumns[i];
      const bool isName = column == FileColumn::Name;
      auto check = std::make_unique<Checkbox>();
      check->setScale(ctx.scale);
      check->setChecked(layout.visible[i]);
      check->setEnabled(!isName);
      check->setOnChange([mutate = ctx.mutate, layout, i](bool checked) {
        auto next = layout;
        next.visible[i] = checked;
        mutate([next](FileManagerSettings& settings) { return settings.saveListColumnLayout(next, false); });
      });
      columnsWrap->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale}, std::move(check),
              ui::label({
                  .text = std::string(kFileColumnLabels[i]),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .color = colorSpecFromRole(isName ? ColorRole::OnSurfaceVariant : ColorRole::OnSurface),
              })
          )
      );
    }
    const bool hasColumnOverrides = ctx.settings.hasPreferenceKey({"files", "list_column_layout"})
        || ctx.settings.hasPreferenceKey({"files", "list_column_widths"});
    if (!ctx.modifiedOnly || hasColumnOverrides)
      columnsCard->addChild(std::move(columnsWrap));
    addPreferenceRow(
        *columnsCard, ctx,
        {
            .title = "Column layout & widths",
            .subtitle = "Reset column visibility, order, and saved widths to defaults.",
            .control = ui::button({
                .text = "Reset Columns",
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .enabled = hasColumnOverrides,
                .variant = ButtonVariant::Default,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick =
                    [mutate = ctx.mutate] {
                      mutate([](FileManagerSettings& settings) {
                        const std::array<nythera::config::ConfigKeyPath, 2> paths{{
                            {"files", "list_column_layout"},
                            {"files", "list_column_widths"},
                        }};
                        return settings.resetKeys(paths);
                      });
                    },
            }),
            .scale = ctx.scale,
        },
        0.F, hasColumnOverrides
    );

    auto* previewCard = addPreferenceCard(parent, "Preview & Thumbnails", "photo", ctx.scale);
    const auto previewMode = ctx.settings.previewMode();
    const std::size_t previewIndex = previewMode == PreviewMode::Hidden ? 0
        : previewMode == PreviewMode::Information                       ? 1
                                                                        : 2;
    addPreferenceRow(
        *previewCard, ctx,
        {
            .title = "Preview pane",
            .subtitle = "Choose whether the side preview pane shows full previews, metadata only, or stays hidden.",
            .control = ui::select({
                .options = std::vector<std::string>{"Hidden", "Information Only", "Full Preview"},
                .selectedIndex = previewIndex,
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 180.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate](std::size_t index, std::string_view) {
                      const auto mode = index == 0 ? PreviewMode::Hidden
                          : index == 1             ? PreviewMode::Information
                                                   : PreviewMode::Full;
                      mutate([mode](FileManagerSettings& settings) { return settings.setPreviewMode(mode); });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "preview_mode"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "preview_mode"}); });
            },
        }
    );

    const auto policy = ctx.settings.thumbnailPolicy();
    const std::size_t policyIndex = policy == ThumbnailPolicy::Never ? 0 : policy == ThumbnailPolicy::Local ? 1 : 2;
    addPreferenceRow(
        *previewCard, ctx,
        {
            .title = "Generate thumbnails",
            .subtitle = "Control where thumbnails are generated for images, videos, and documents.",
            .control = ui::select({
                .options = std::vector<std::string>{"Never", "Local Filesystems Only", "All Filesystems"},
                .selectedIndex = policyIndex,
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .controlHeight = Style::controlHeightSm * ctx.scale,
                .width = 200.0F * ctx.scale,
                .onSelectionChanged =
                    [mutate = ctx.mutate](std::size_t index, std::string_view) {
                      const auto next = index == 0 ? ThumbnailPolicy::Never
                          : index == 1             ? ThumbnailPolicy::Local
                                                   : ThumbnailPolicy::All;
                      mutate([next](FileManagerSettings& settings) { return settings.setThumbnailPolicy(next); });
                    },
            }),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "thumbnail_policy"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "thumbnail_policy"}); });
            },
        }
    );

    auto maxSizeStepper = std::make_unique<Stepper>();
    maxSizeStepper->setScale(ctx.scale);
    maxSizeStepper->setRange(1, 64);
    maxSizeStepper->setStep(1);
    maxSizeStepper->setValueSuffix(" MiB");
    maxSizeStepper->setValue(ctx.settings.thumbnailMaxFileMiB());
    maxSizeStepper->setOnValueCommitted([mutate = ctx.mutate](int value) {
      mutate([value](FileManagerSettings& settings) { return settings.setThumbnailMaxFileMiB(value); });
    });
    addPreferenceRow(
        *previewCard, ctx,
        {
            .title = "Maximum thumbnail source file size",
            .subtitle = "Skip generating thumbnails for files larger than this limit (1 – 64 MiB).",
            .control = std::move(maxSizeStepper),
            .scale = ctx.scale,
            .showReset = ctx.settings.hasPreferenceKey({"files", "thumbnail_max_file_mib"}),
            .onReset = [mutate = ctx.mutate] {
              mutate([](FileManagerSettings& settings) { return settings.reset({"files", "thumbnail_max_file_mib"}); });
            },
        }
    );
  }

} // namespace filemanager
