#include "filemanager/dialogs/search_date_picker.h"

#include "ui/controls/button.h"
#include "ui/controls/calendar_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace filemanager {

  void showSearchDatePicker(
      ModalDialogHost& host, SearchDate initial, bool second, std::function<void(std::optional<SearchDate>)> onSelected
  ) {
    if (host.isOpen())
      return;
    struct Picker {
      SearchDate selected;
      int offset = 0;
      Flex* days = nullptr;
      Label* month = nullptr;
    };
    auto picker = std::make_shared<Picker>();
    picker->selected = initial;
    const auto today = calendar_view::stateForOffset(0).current;
    if (picker->selected.year)
      picker->offset = (picker->selected.year - today.year) * 12 + picker->selected.month - today.month - 1;
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
              auto navigation = std::make_unique<Flex>();
              navigation->setAlign(FlexAlign::Center);
              navigation->setGap(Style::spaceXs);
              auto* navRow = static_cast<Flex*>(dialog.addChild(std::move(navigation)));
              const auto navigate = [navRow, picker, rebuild,
                                     today](std::string_view glyph, std::string_view title, int delta) {
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
                navRow->addChild(std::move(button));
              };
              navigate("chevrons-left", "Previous year", -12);
              navigate("chevron-left", "Previous month", -1);
              auto label = std::make_unique<Label>();
              label->setTextAlign(TextAlign::Center);
              label->setFlexGrow(1.0F);
              picker->month = label.get();
              navRow->addChild(std::move(label));
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
    std::vector<DialogChoice> actions{{"Cancel", 0, DialogChoiceStyle::Cancel}};
    if (picker->selected.year)
      actions.push_back({"Clear date", 2});
    host.showDialog(
        second ? "Choose end date" : "Choose modified date", "", std::move(actions),
        [picker, onSelected = std::move(onSelected)](int result) {
          if (result != 1 && result != 2)
            return;
          if (onSelected)
            onSelected(result == 1 ? std::optional(picker->selected) : std::nullopt);
        },
        nullptr, std::move(content)
    );
  }

} // namespace filemanager
