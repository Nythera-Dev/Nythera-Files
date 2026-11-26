#pragma once

#include "render/scene/node.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/style.h"
#include "xdg-shell-client-protocol.h"

#include <cmath>

namespace filemanager {

  inline void anchorSearchDropdown(ContextMenuPopupRequest& request, const Node& trigger) {
    float left{}, top{}, right{}, bottom{};
    Node::transformedBounds(&trigger, left, top, right, bottom);
    const auto x = static_cast<std::int32_t>(std::lround(left));
    const auto y = static_cast<std::int32_t>(std::lround(top));
    request.anchor = {
        x, y, static_cast<std::int32_t>(std::lround(right)) - x, static_cast<std::int32_t>(std::lround(bottom)) - y
    };
    request.minMenuWidth = std::max(1.0F, right - left);
    request.placement = ContextMenuPopupPlacement{
        .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT,
        .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
        .offsetY = static_cast<std::int32_t>(Style::spaceXs),
        .chromeAttachment = {popup_chrome::HorizontalAttachment::Left, popup_chrome::VerticalAttachment::Top},
    };
    request.fitAvailableHeight = true;
  }

} // namespace filemanager
