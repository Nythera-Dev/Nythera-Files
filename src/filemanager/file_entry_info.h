#pragma once

#include "core/files/directory_scanner.h"
#include "ui/dialogs/file_artwork.h"

#include <chrono>
#include <gio/gio.h>

namespace filemanager {

  inline FileEntry makeFileEntry(
      GFileInfo* info, const std::filesystem::path& path,
      const char* contentType = G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
  ) {
    FileEntry entry;
    const auto* name = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_STANDARD_NAME);
    entry.name = name ? name : path.filename().string();
    entry.absPath = path;
    entry.isDir = gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_DIRECTORY;
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
      entry.size = g_file_info_get_size(info);
    if (const auto* type = g_file_info_get_attribute_string(info, contentType))
      entry.type = type;
    entry.mtime = std::filesystem::file_time_type::clock::from_sys(
        std::chrono::system_clock::from_time_t(
            static_cast<std::time_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        )
    );
    entry.hasCreated = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_CREATED);
    entry.created = entry.hasCreated ? g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CREATED) : 0;
    entry.hasAccessed = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_ACCESS);
    entry.accessed = entry.hasAccessed ? g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_ACCESS) : 0;
    entry.isSymlink =
        g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) && g_file_info_get_is_symlink(info);
    if (!entry.isSymlink
        && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE)
        && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE)) {
      entry.device = static_cast<std::uint64_t>(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE));
      entry.inode = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE);
      entry.hasIdentity = true;
    }
    return entry;
  }

} // namespace filemanager
