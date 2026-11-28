#pragma once

#include "filemanager/file_columns.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>

namespace filemanager {

  inline std::string serializeFileColumns(const FileColumns& widths) {
    std::string text;
    for (float width : widths.widths) {
      if (!text.empty())
        text += ',';
      text += std::to_string(width);
    }
    return text;
  }

  inline bool parseFileColumns(std::string_view text, FileColumns& widths) {
    FileColumns parsed;
    std::size_t count = 0;
    while (count < parsed.widths.size()) {
      const auto comma = text.find(',');
      const auto token = text.substr(0, comma);
      if (token.empty())
        return false;
      const auto index = count++;
      auto& value = parsed.widths[index];
      const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
      if (error != std::errc{}
          || end != token.data() + token.size()
          || !std::isfinite(value)
          || value < 0.0F
          || (value == 0.0F && index != 0)
          || value > 10000.0F)
        return false;
      if (comma == std::string_view::npos) {
        if (count != kDefaultFileColumns.size() && count != parsed.widths.size())
          return false;
        widths = parsed;
        return true;
      }
      text.remove_prefix(comma + 1);
    }
    return false;
  }

  inline std::string serializeFileColumnLayout(const FileColumnLayout& layout) {
    std::string text;
    for (auto column : layout.order) {
      if (!text.empty())
        text += ',';
      if (!layout.visible[columnIndex(column)])
        text += '-';
      text += kFileColumnLabels[columnIndex(column)];
    }
    return text;
  }

  inline bool parseFileColumnLayout(std::string_view text, FileColumnLayout& layout) {
    FileColumnLayout parsed;
    std::array<bool, kFileColumns.size()> seen{};
    for (std::size_t i = 0; i < kFileColumns.size(); ++i) {
      const auto comma = text.find(',');
      auto token = text.substr(0, comma);
      const bool visible = !token.starts_with('-');
      if (!visible)
        token.remove_prefix(1);
      const auto found = std::ranges::find(kFileColumnLabels, token);
      if (found == kFileColumnLabels.end() || (comma == std::string_view::npos) != (i + 1 == kFileColumns.size()))
        return false;
      const auto index = static_cast<std::size_t>(found - kFileColumnLabels.begin());
      if (seen[index] || (!visible && kFileColumns[index] == FileColumn::Name))
        return false;
      seen[index] = true;
      parsed.order[i] = kFileColumns[index];
      parsed.visible[index] = visible;
      if (comma != std::string_view::npos)
        text.remove_prefix(comma + 1);
    }
    layout = parsed;
    return true;
  }

  inline std::string serializeSearchFileColumnLayout(const FileColumnLayout& layout) {
    auto text = serializeFileColumnLayout(layout);
    text.replace(text.find("Created"), sizeof("Created") - 1, "Location");
    return text;
  }

  inline bool parseSearchFileColumnLayout(std::string_view text, FileColumnLayout& layout) {
    const auto location = text.find("Location");
    if (location == std::string_view::npos || text.contains("Created"))
      return false;
    std::string normalized(text);
    normalized.replace(location, sizeof("Location") - 1, "Created");
    FileColumnLayout parsed;
    if (!parseFileColumnLayout(normalized, parsed) || parsed.visible[columnIndex(FileColumn::Accessed)])
      return false;
    layout = parsed;
    return true;
  }

} // namespace filemanager
