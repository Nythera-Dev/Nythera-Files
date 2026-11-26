#pragma once

#include "filemanager/search_scope.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {

  // Root lines are provisional until completed. Use the caret to distinguish
  // unchanged lines after an edit from identical text pasted over them.
  class CustomSearchEditorState {
  public:
    void load(std::string_view value) {
      m_text = value;
      m_lines = split(value);
      for (auto& line : m_lines)
        line.completed = true;
    }

    void edit(std::string_view value, std::size_t cursor, std::optional<std::size_t> completedLine = std::nullopt) {
      auto next = split(value);
      cursor = std::min(cursor, value.size());
      const std::string_view old = m_text;
      if (old == value) {
        next = m_lines;
      } else {
        std::size_t prefix = 0;
        while (prefix < std::min(old.size(), cursor) && old[prefix] == value[prefix])
          ++prefix;
        std::size_t suffix = 0;
        while (suffix < std::min(old.size() - prefix, value.size() - cursor)
               && old[old.size() - 1 - suffix] == value[value.size() - 1 - suffix])
          ++suffix;

        std::size_t i = 0;
        for (; i < next.size() && next[i].end < prefix; ++i)
          next[i].completed = m_lines[i].completed;
        for (std::size_t remaining = m_lines.size(), current = next.size(); remaining > i && current > i;) {
          const auto& previous = m_lines[--remaining];
          auto& line = next[--current];
          if (line.start < value.size() - suffix || previous.start < old.size() - suffix)
            break;
          line.completed = previous.completed;
        }
        // A multiline paste can complete several lines while leaving its last line active.
        for (auto& line : next) {
          if (line.end >= prefix && line.end < cursor && value[line.end] == '\n')
            line.completed = true;
        }
        if (next.size() == m_lines.size()) {
          for (; i < next.size(); ++i) {
            if (m_lines[i].completed && rootLine(old, m_lines[i]) && rootLine(value, next[i]))
              next[i].completed = true;
          }
        }
      }
      if (completedLine && *completedLine < next.size())
        next[*completedLine].completed = true;
      m_text = value;
      m_lines = std::move(next);
    }

    [[nodiscard]] bool hasCompletedRoot() const {
      return std::ranges::any_of(m_lines, [this](const auto& line) {
        return line.completed && rootLine(m_text, line);
      });
    }

    [[nodiscard]] static bool saveWithMounts(const SearchLocations& included, bool checked) {
      return checked && explicitlyIncludesRoot(included);
    }

  private:
    struct Line {
      std::size_t start;
      std::size_t end;
      bool completed = false;
    };

    static std::vector<Line> split(std::string_view value) {
      std::vector<Line> lines;
      std::size_t start = 0;
      for (auto end = value.find('\n'); end != value.npos; end = value.find('\n', start)) {
        lines.push_back({start, end});
        start = end + 1;
      }
      lines.push_back({start, value.size()});
      return lines;
    }

    static bool rootLine(std::string_view text, const Line& line) {
      return trimSearchText(text.substr(line.start, line.end - line.start)) == "/";
    }

    std::string m_text;
    std::vector<Line> m_lines;
  };

} // namespace filemanager
