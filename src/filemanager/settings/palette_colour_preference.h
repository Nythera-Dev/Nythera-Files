#pragma once

#include "config/color_spec.h"
#include "filemanager/settings/preference_rows.h"
#include "render/scene/input_area.h"

namespace filemanager {

  // The caller decides how to persist the selected role or fixed colour.
  inline void paletteColourPreference(
      Flex& card, const SettingsPageContext& ctx, std::string title, std::string subtitle, const PreferencePath& key,
      std::optional<ColorSpec> overrideValue, ColorSpec inheritedValue, std::string defaultLabel,
      std::function<bool(FileManagerSettings&, ColorSpec)> set, float labelIndent = 0.F
  ) {
    if (ctx.modifiedOnly && !ctx.settings.hasPreferenceKey(key))
      return;

    const auto current = overrideValue.value_or(inheritedValue);
    const bool allowDefault = !defaultLabel.empty();
    std::vector<std::string> labels;
    std::vector<ColorSpec> swatches;
    std::size_t selected = 0;
    if (allowDefault) {
      labels.push_back(std::move(defaultLabel));
      swatches.push_back(clearColorSpec());
    }
    const auto firstRole = labels.size();
    for (const auto& [role, token] : kColorRoleTokens) {
      if ((overrideValue || !allowDefault) && current.role == role)
        selected = labels.size();
      labels.emplace_back(token);
      swatches.push_back(colorSpecFromRole(role));
    }
    const auto fixedIndex = labels.size();
    const bool customSelected = (overrideValue || !allowDefault) && !current.role;
    if (customSelected) {
      selected = fixedIndex;
      labels.push_back(colorSpecToConfigString(current));
      swatches.push_back(current);
    }
    const auto customIndex = labels.size();
    labels.emplace_back("Custom");
    swatches.push_back(clearColorSpec());

    auto select = ui::select({
        .options = std::move(labels),
        .selectedIndex = selected,
        .fontSize = Style::fontSizeBody * ctx.scale,
        .controlHeight = Style::controlHeight * ctx.scale,
        .glyphSize = Style::fontSizeBody * ctx.scale,
        .optionIndicators = std::move(swatches),
        .notifyOnReselect = true,
        .width = 190.F * ctx.scale,
    });
    for (const auto& child : select->children()) {
      if (auto* area = dynamic_cast<InputArea*>(child.get())) {
        std::string focusKey = "palette-colour";
        for (const auto& segment : key)
          focusKey += ":" + segment;
        area->setTabFocusKey(std::move(focusKey));
        break;
      }
    }
    select->setOnSelectionChanged([control = select.get(), selected, current, firstRole, fixedIndex, customIndex, key,
                                   mutate = ctx.mutate, pick = ctx.pickColor, pickerTitle = "Edit " + title,
                                   set = std::move(set)](std::size_t index, std::string_view) {
      if (index == customIndex || index == fixedIndex) {
        // Keep the committed choice visible if the picker is cancelled.
        control->setSelectedIndexSilently(selected);
        if (pick)
          pick(pickerTitle, resolveColorSpec(current), [mutate, set](Color color) {
            mutate([set, color](FileManagerSettings& settings) { return set(settings, fixedColorSpec(color)); });
          });
      } else if (index < firstRole) {
        mutate([key](FileManagerSettings& settings) { return settings.reset(key); });
      } else if (index != selected) {
        const auto spec = colorSpecFromRole(kColorRoleTokens[index - firstRole].role);
        mutate([set, spec](FileManagerSettings& settings) { return set(settings, spec); });
      }
    });
    preferenceRow(card, ctx, std::move(title), std::move(subtitle), key, std::move(select), true, labelIndent);
  }

} // namespace filemanager
