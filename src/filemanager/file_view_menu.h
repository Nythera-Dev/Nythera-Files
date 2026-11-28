#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/file_archive_actions.h"
#include "ui/controls/context_menu.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace filemanager {
  inline constexpr int kQuickLookMenuId = 30400;
  inline constexpr int kGoToLinkTargetMenuId = 30411;
  enum class FileMenuContext {
    Toolbar,
    File,
    Folder,
    Empty,
    Places,
    Trash,
  };

  // Reorder existing native actions into visual groups. Separators are generated only
  // between non-empty groups, so optional actions cannot leave empty or adjacent rules.
  inline void groupFileMenuEntries(std::vector<ContextMenuControlEntry>& entries, FileMenuContext context) {
    using Group = std::vector<int>;
    std::vector<Group> groups;
    const bool fileOrFolder = context == FileMenuContext::File || context == FileMenuContext::Folder;
    constexpr int propertiesId = 22;
    if (fileOrFolder) {
      groups = {
          {10, 80, kQuickLookMenuId, kGoToLinkTargetMenuId, 30013, 60, 30151},
          {2, 1, 3, 59},
          {110},
          {119, 4, 5, 6},
          {menu_id::kArchiveCompress, menu_id::kArchiveExtract},
          {20, 31, 100},
          {30152},
          {30104, 30105},
      };
    } else if (context == FileMenuContext::Empty) {
      groups = {{3}, {110}, {14, 50, 100}, {21}};
    } else if (context == FileMenuContext::Places) {
      groups = {{30, 41, 40}, {33, 34, 35}, {31, 32, 36, 37, 38}, {39}};
    } else {
      return; // Toolbar and Trash retain their existing action order and management scope.
    }

    std::vector<ContextMenuControlEntry> actions;
    actions.reserve(entries.size());
    for (auto& entry : entries) {
      if (!entry.separator)
        actions.push_back(std::move(entry));
    }
    std::vector<bool> used(actions.size(), false);
    std::vector<ContextMenuControlEntry> ordered;
    ordered.reserve(actions.size() + groups.size());
    for (const auto& group : groups) {
      bool hasGroupAction = false;
      for (const int id : group) {
        for (std::size_t i = 0; i < actions.size(); ++i) {
          if (!used[i] && actions[i].id == id) {
            ordered.push_back(std::move(actions[i]));
            used[i] = true;
            hasGroupAction = true;
          }
        }
      }
      if (hasGroupAction && !ordered.empty())
        ordered.push_back({.separator = true});
    }
    std::optional<ContextMenuControlEntry> properties;
    for (std::size_t i = 0; i < actions.size(); ++i) {
      if (used[i])
        continue;
      if (fileOrFolder && actions[i].id == propertiesId)
        properties = std::move(actions[i]);
      else
        ordered.push_back(std::move(actions[i]));
    }
    if (properties) {
      if (!ordered.empty() && !ordered.back().separator)
        ordered.push_back({.separator = true});
      ordered.push_back(std::move(*properties));
    }
    while (!ordered.empty() && ordered.back().separator)
      ordered.pop_back();
    entries = std::move(ordered);
  }

  inline ContextMenuControlEntry fileSortByMenuEntry(
      FileDialogSortField sortField, FileDialogSortOrder sortOrder, bool foldersFirst, bool hiddenLast,
      bool search = false
  ) {
    const auto field = [sortField](int id, const char* label, FileDialogSortField value) {
      return ContextMenuControlEntry{
          .id = id, .label = label, .radio = true, .toggleState = sortField == value ? 1 : 0
      };
    };
    std::vector<ContextMenuControlEntry> children{
        field(53, "Name", FileDialogSortField::Name),
        field(54, "Size", FileDialogSortField::Size),
        field(61, "Type", FileDialogSortField::Type),
        field(55, "Modified", FileDialogSortField::Modified),
        {.id = 58,
         .label = "Ascending",
         .radio = true,
         .toggleState = sortOrder == FileDialogSortOrder::Ascending ? 1 : 0},
        {.id = 62,
         .label = "Descending",
         .radio = true,
         .toggleState = sortOrder == FileDialogSortOrder::Descending ? 1 : 0},
        {.id = 51, .label = "Folders First", .checkmark = true, .toggleState = foldersFirst ? 1 : 0},
        {.id = 52, .label = "Hidden Files Last", .checkmark = true, .toggleState = hiddenLast ? 1 : 0}
    };
    if (search) {
      children.insert(children.begin() + 1, field(63, "Location", FileDialogSortField::Location));
    } else {
      children.insert(
          children.begin() + 4,
          {field(56, "Created", FileDialogSortField::Created), field(57, "Accessed", FileDialogSortField::Accessed)}
      );
    }
    return {
        .id = 100,
        .label = "Sort By",
        .hasSubmenu = true,
        .submenuEntries = std::move(children),
        .actionIconNames = {"view-sort-ascending-symbolic", "view-sort-ascending"},
        .actionGlyph = "arrows-sort"
    };
  }

  inline void appendFileMenuEntries(
      std::vector<ContextMenuControlEntry>& entries, FileMenuContext context, bool gridMode, bool showHidden,
      FileDialogSortField sortField, FileDialogSortOrder sortOrder, bool foldersFirst, bool hiddenLast,
      bool search = false
  ) {
    if (context == FileMenuContext::Toolbar || context == FileMenuContext::Empty) {
      entries.push_back({.id = 14, .label = gridMode ? "Switch to List" : "Switch to Grid"});
      entries.push_back({.id = 50, .label = "Show Hidden Files", .checkmark = true, .toggleState = showHidden ? 1 : 0});
    }
    entries.push_back(fileSortByMenuEntry(sortField, sortOrder, foldersFirst, hiddenLast, search));
  }
} // namespace filemanager
