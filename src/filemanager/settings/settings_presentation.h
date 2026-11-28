#pragma once

#include "filemanager/settings/settings_pages.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/settings_layout.h"

namespace filemanager {

  inline constexpr float kPreferenceCardTitleSize = Style::fontSizeTitle;

  [[nodiscard]] inline Flex*
  addPreferenceCard(Flex& parent, std::string_view title, std::string_view glyph, float scale) {
    Flex* body = nullptr;
    parent.addChild(
        ui::column(
            {.align = FlexAlign::Stretch,
             .gap = Style::spaceSm * scale,
             .configure = [scale](Flex& card) { card.setCardStyle(scale, 0.4F); }},
            ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
                ui::glyph({
                    .glyph = std::string(glyph),
                    .glyphSize = kPreferenceCardTitleSize * scale,
                    .color = colorSpecFromRole(ColorRole::Primary),
                }),
                ui::label({
                    .text = std::string(title),
                    .fontSize = kPreferenceCardTitleSize * scale,
                    .fontWeight = FontWeight::Bold,
                    .color = colorSpecFromRole(ColorRole::Primary),
                })
            ),
            ui::column({.out = &body, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale})
        )
    );
    return body;
  }

  inline void addPreferenceRow(
      Flex& parent, const SettingsPageContext& ctx, ui::SettingsRowProps props, float labelIndent = 0.F,
      bool modifiedWithoutReset = false
  ) {
    if (ctx.modifiedOnly && !props.showReset && !modifiedWithoutReset)
      return;
    const float scale = props.scale;
    auto& row = *ui::makeSettingsRow(parent, std::move(props));
    row.setJustify(FlexJustify::SpaceBetween);
    row.setGap(Style::spaceXs * scale);
    row.setPadding(2.F * scale, 0.F);
    row.setMinHeight(Style::controlHeight * scale);

    // Keep the library row's reset controls and their reserved layout space.
    auto& copy = static_cast<Flex&>(*row.children().front());
    copy.setGap(Style::spaceXs * scale);
    copy.setPadding(0.F, 0.F, 0.F, labelIndent * scale);
    static_cast<Label&>(*copy.children().front()).setFontWeight(FontWeight::Bold);
    static_cast<Flex&>(*row.children().back()).setGap(Style::spaceSm * scale);
  }

} // namespace filemanager
