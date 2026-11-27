// Preview content limits adapted from Luciole (noctalia-dev), MIT. See LICENSE.luciole.
#include "filemanager/preview/preview_content.h"

#include "core/files/display_text.h"
#include "filemanager/folder_size.h"
#include "filemanager/preview/thumbnail_provider.h"
#include "filemanager/search_thumbnails.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>

namespace filemanager::preview {
  namespace {
    bool sameRevision(const struct stat& a, const struct stat& b) {
      return a.st_ino == b.st_ino
          && a.st_dev == b.st_dev
          && a.st_size == b.st_size
          && a.st_mtim.tv_sec == b.st_mtim.tv_sec
          && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
          && a.st_ctim.tv_sec == b.st_ctim.tv_sec
          && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    }
  } // namespace

  bool textMime(std::string_view mime) {
    return mime.starts_with("text/")
        || (mime.starts_with("application/")
            && (mime == "application/json"
                || mime.ends_with("+json")
                || mime == "application/xml"
                || mime.ends_with("+xml")
                || mime == "application/yaml"
                || mime == "application/javascript"
                || mime == "application/x-desktop"
                || mime == "application/x-shellscript"));
  }

  TextPreview previewText(std::string_view bytes, bool truncated) {
    if (bytes.starts_with("\xEF\xBB\xBF"))
      bytes.remove_prefix(3);
    if (bytes.empty())
      return {{}, "Empty file"};
    const char* end = nullptr;
    if (!g_utf8_validate(bytes.data(), static_cast<gssize>(bytes.size()), &end)) {
      const auto remainder = bytes.data() + bytes.size() - end;
      if (!truncated || remainder > 3 || g_utf8_get_char_validated(end, remainder) != static_cast<gunichar>(-2))
        return {{}, "This file is not UTF-8 text."};
      bytes = bytes.substr(0, static_cast<std::size_t>(end - bytes.data()));
    }

    std::string text;
    std::size_t lines = 0;
    std::size_t columns = 0;
    for (const char* p = bytes.data(); p != bytes.data() + bytes.size(); p = g_utf8_next_char(p)) {
      if (text.size() >= 8192 || lines >= 200) {
        truncated = true;
        break;
      }
      const auto ch = g_utf8_get_char(p);
      if (ch == '\n') {
        ++lines;
        columns = 0;
        text += '\n';
      } else if (columns++ < 240) {
        if (ch == '\t')
          text += "    ";
        else if (!g_unichar_iscntrl(ch))
          text.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
      } else if (columns == 241) {
        text += "…";
        truncated = true;
      }
    }
    return {std::move(text), truncated ? "Preview truncated. Open the file to see the rest." : ""};
  }

  Content collect(const Request& request, GCancellable* cancel) {
    Content content;
    if (request.source.empty() || !std::filesystem::path(request.source).is_absolute())
      return content;
    if (request.protectedCopy) {
      content.imageAllowed = request.policy != ThumbnailPolicy::Never;
      return content;
    }
    const auto& entry = request.entry;
    g_autoptr(GFile) file = g_file_new_for_path(request.source.c_str());
    g_autoptr(GFileInfo) fs =
        g_file_query_filesystem_info(file, "filesystem::remote,filesystem::use-preview", cancel, nullptr);
    const bool local = SearchThumbnails::permitsAutomaticPreview(fs);
    const bool full = request.mode == PreviewMode::Full;
    content.imageAllowed = request.policy != ThumbnailPolicy::Never
        && (local || request.policy == ThumbnailPolicy::All)
        && SearchThumbnails::eligible(entry);
    if (!local || g_cancellable_is_cancelled(cancel))
      return content;

    if (entry.isSymlink) {
      g_autoptr(GFileInfo) info = g_file_query_info(
          file, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr
      );
      if (info) {
        if (const char* target = g_file_info_get_symlink_target(info)) {
          content.details.push_back({"Link Target", sanitizeFilenameForDisplay(target)});
          std::error_code error;
          const auto resolved = std::filesystem::canonical(entry.absPath, error);
          if (error)
            content.details.push_back({"Target Status", sanitizeFilenameForDisplay(error.message())});
          else
            content.details.push_back({"Resolved Target", sanitizeFilenameForDisplay(resolved.string())});
        }
      }
      return content;
    }

    if (entry.isDir) {
      if (full && entry.hasIdentity) {
        const auto result = calculateFolderSize({entry.absPath, entry.device, entry.inode}, [cancel] {
          return g_cancellable_is_cancelled(cancel);
        });
        if (!result.failed && !result.cancelled) {
          g_autofree char* size = g_format_size(result.bytes);
          content.details.push_back({"Contents", std::format("{} files · {} folders", result.files, result.folders)});
          content.details.push_back({"Total Size", std::format("{}{}", result.partial() ? "≥ " : "", size)});
        }
      }
      return content;
    }

    const bool knownText = !imageThumbnail(entry.type)
        && (textMime(entry.type) || (!entry.type.empty() && g_content_type_is_a(entry.type.c_str(), "text/plain")));
    const bool sniff = entry.type.empty() || entry.type == "application/octet-stream";
    if (!full || (!knownText && !sniff && !content.imageAllowed))
      return content;
    struct File {
      int fd;
      ~File() {
        if (fd >= 0)
          ::close(fd);
      }
    } source{::open(request.source.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    struct stat before{};
    if (source.fd < 0
        || ::fstat(source.fd, &before) != 0
        || !S_ISREG(before.st_mode)
        || (entry.hasIdentity && (entry.device != before.st_dev || entry.inode != before.st_ino))) {
      content.imageAllowed = false;
      if (knownText)
        content.notice = "Could not read this file.";
      return content;
    }

    if (mediaThumbnail(entry.type)) {
      content.details = mediaDetails(source.fd, entry.type, [cancel] { return g_cancellable_is_cancelled(cancel); });
      struct stat named{};
      if (g_cancellable_is_cancelled(cancel)
          || ::lstat(request.source.c_str(), &named) != 0
          || !sameRevision(before, named))
        return {};
      return content;
    }
    constexpr std::size_t limit = 64 * 1024;
    std::string bytes;
    std::array<char, 4096> buffer{};
    while (bytes.size() < limit && !g_cancellable_is_cancelled(cancel)) {
      const auto count = ::read(source.fd, buffer.data(), std::min(buffer.size(), limit - bytes.size()));
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0) {
        if (knownText)
          content.notice = "Could not read this file.";
        return content;
      }
      if (count == 0)
        break;
      bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    struct stat after{}, named{};
    if (g_cancellable_is_cancelled(cancel)
        || ::fstat(source.fd, &after) != 0
        || ::lstat(request.source.c_str(), &named) != 0
        || !sameRevision(before, after)
        || !sameRevision(before, named))
      return {};

    if (content.imageAllowed && !bytes.empty()) {
      const auto dimensions = probeRasterImageExtent(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
      if (dimensions)
        content.details.push_back({"Dimensions", std::format("{} × {} px", dimensions->width, dimensions->height)});
    }
    if (!knownText && sniff && !bytes.empty()) {
      g_autofree char* mime =
          g_content_type_guess(nullptr, reinterpret_cast<const guchar*>(bytes.data()), bytes.size(), nullptr);
      if (!mime || !textMime(mime))
        return content;
    }
    if (knownText || sniff) {
      auto text = previewText(bytes, static_cast<std::uintmax_t>(before.st_size) > bytes.size());
      content.text = std::move(text.text);
      content.notice = std::move(text.notice);
      if (entry.type == "application/x-desktop") {
        g_autoptr(GKeyFile) desktop = g_key_file_new();
        if (g_key_file_load_from_data(desktop, bytes.data(), bytes.size(), G_KEY_FILE_NONE, nullptr)) {
          g_autofree char* name = g_key_file_get_locale_string(desktop, "Desktop Entry", "Name", nullptr, nullptr);
          g_autofree char* comment =
              g_key_file_get_locale_string(desktop, "Desktop Entry", "Comment", nullptr, nullptr);
          if (name)
            content.details.push_back({"Name", sanitizeFilenameForDisplay(name)});
          if (comment)
            content.details.push_back({"Description", sanitizeFilenameForDisplay(comment)});
        }
      }
    }
    return content;
  }

} // namespace filemanager::preview
