#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/file_archive_actions.h"
#include "filemanager/menu_ids.h"
#include "ui/controls/context_menu.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace filemanager {
  inline constexpr int kQuickLookMenuId = menu_id::kQuickLook;
  inline constexpr int kGoToLinkTargetMenuId = menu_id::kGoToLinkTarget;
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
    constexpr int propertiesId = menu_id::kProperties;
    if (fileOrFolder) {
      groups = {
          {menu_id::kOpen, menu_id::kOpenWithSubmenu, menu_id::kQuickLook, menu_id::kGoToLinkTarget,
           menu_id::kOpenFolderInNewTab, menu_id::kOpenFolderNewWindow, menu_id::kOpenFolderAsAdministrator},
          {menu_id::kCut, menu_id::kCopy, menu_id::kPaste, menu_id::kCopyLocation},
          {menu_id::kCreateNewSubmenu},
          {menu_id::kDuplicate, menu_id::kRename, menu_id::kTrash, menu_id::kDeletePermanently},
          {menu_id::kArchiveCompress, menu_id::kArchiveExtract},
          {menu_id::kPinTargetToPlaces, menu_id::kPlaceRemove, menu_id::kSortBySubmenu},
          {menu_id::kSaveProtectedChanges},
          {menu_id::kCalculateChecksum, menu_id::kVerifyChecksum},
      };
    } else if (context == FileMenuContext::Empty) {
      groups = {
          {menu_id::kPaste},
          {menu_id::kCreateNewSubmenu},
          {menu_id::kToggleViewMode, menu_id::kShowHidden, menu_id::kSortBySubmenu},
          {menu_id::kPinCurrentToPlaces},
      };
    } else if (context == FileMenuContext::Places) {
      groups = {
          {menu_id::kPlaceOpen, menu_id::kPlaceOpenInNewTab, menu_id::kPlaceOpenNewWindow},
          {menu_id::kPlaceMount, menu_id::kPlaceUnmount, menu_id::kPlaceEject},
          {menu_id::kPlaceRemove, menu_id::kPlacePin, menu_id::kPlaceHide, menu_id::kPlaceUnhide,
           menu_id::kPlaceToggleShowHidden},
          {menu_id::kPlaceProperties},
      };
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
        field(menu_id::kSortName, "Name", FileDialogSortField::Name),
        field(menu_id::kSortSize, "Size", FileDialogSortField::Size),
        field(menu_id::kSortType, "Type", FileDialogSortField::Type),
        field(menu_id::kSortModified, "Modified", FileDialogSortField::Modified),
        {.id = menu_id::kSortAscending,
         .label = "Ascending",
         .radio = true,
         .toggleState = sortOrder == FileDialogSortOrder::Ascending ? 1 : 0},
        {.id = menu_id::kSortDescending,
         .label = "Descending",
         .radio = true,
         .toggleState = sortOrder == FileDialogSortOrder::Descending ? 1 : 0},
        {.id = menu_id::kFoldersFirst,
         .label = "Folders First",
         .checkmark = true,
         .toggleState = foldersFirst ? 1 : 0},
        {.id = menu_id::kHiddenLast, .label = "Hidden Files Last", .checkmark = true, .toggleState = hiddenLast ? 1 : 0}
    };
    if (search) {
      children.insert(children.begin() + 1, field(menu_id::kSortLocation, "Location", FileDialogSortField::Location));
    } else {
      children.insert(
          children.begin() + 4,
          {field(menu_id::kSortCreated, "Created", FileDialogSortField::Created),
           field(menu_id::kSortAccessed, "Accessed", FileDialogSortField::Accessed)}
      );
    }
    return {
        .id = menu_id::kSortBySubmenu,
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
      entries.push_back({.id = menu_id::kToggleViewMode, .label = gridMode ? "Switch to List" : "Switch to Grid"});
      entries.push_back(
          {.id = menu_id::kShowHidden,
           .label = "Show Hidden Files",
           .checkmark = true,
           .toggleState = showHidden ? 1 : 0}
      );
    }
    entries.push_back(fileSortByMenuEntry(sortField, sortOrder, foldersFirst, hiddenLast, search));
  }
} // namespace filemanager
