// Format probing and encoder presets adapted from Luciole archive_writer.cpp and archive_type.cpp.
#include "archive_safety.h"

#include <algorithm>
#include <archive.h>
#include <lzma.h>

namespace filemanager::archive_backend {
  namespace {
    constexpr std::array formats{
        std::pair{Format::TarGzip, std::string_view(".tar.gz")},
        std::pair{Format::TarXz, std::string_view(".tar.xz")},
        std::pair{Format::TarZstd, std::string_view(".tar.zst")},
        std::pair{Format::TarBzip2, std::string_view(".tar.bz2")},
        std::pair{Format::Zip, std::string_view(".zip")},
        std::pair{Format::SevenZip, std::string_view(".7z")},
        std::pair{Format::Tar, std::string_view(".tar")},
        std::pair{Format::Gzip, std::string_view(".gz")},
        std::pair{Format::Rar, std::string_view(".rar")},
    };

    bool canRead(Format format) {
      using A = struct archive;
      const std::unique_ptr<A, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
      if (!reader || !detail::nativeFilter(format))
        return false;
      auto* a = reader.get();
      switch (format) {
      case Format::Zip:
        return archive_read_support_format_zip_seekable(a) == ARCHIVE_OK;
      case Format::SevenZip:
        return archive_liblzma_version() && archive_read_support_format_7zip(a) == ARCHIVE_OK;
      case Format::Gzip:
        return archive_read_support_format_raw(a) == ARCHIVE_OK && archive_read_support_format_empty(a) == ARCHIVE_OK;
      case Format::Rar:
        return archive_read_support_format_rar(a) == ARCHIVE_OK && archive_read_support_format_rar5(a) == ARCHIVE_OK;
      case Format::TarBzip2:
        if (archive_read_support_filter_bzip2(a) != ARCHIVE_OK)
          return false;
        break;
      case Format::TarZstd:
        if (archive_read_support_filter_zstd(a) != ARCHIVE_OK)
          return false;
        break;
      default:
        break;
      }
      return archive_read_support_format_tar(a) == ARCHIVE_OK;
    }
  } // namespace

  std::optional<Format> formatFromName(std::string_view name) {
    const auto endsWith = [name](std::string_view suffix) {
      return name.size() > suffix.size()
          && std::ranges::equal(name.substr(name.size() - suffix.size()), suffix, [](char a, char b) {
               return g_ascii_tolower(a) == b;
             });
    };
    if (endsWith(".tgz"))
      return Format::TarGzip;
    for (const auto& [format, suffix] : formats) {
      if (endsWith(suffix))
        return format;
    }
    return std::nullopt;
  }

  std::span<const Capability> capabilities() {
    static const auto available = [] {
      std::array<Capability, formats.size()> result{};
      for (std::size_t i = 0; i < formats.size(); ++i) {
        const auto [format, suffix] = formats[i];
        auto& item = result[i];
        item = {format, suffix, canRead(format), false, {}};
        for (const auto level : {Level::Fast, Level::Balanced, Level::Maximum}) {
          using A = struct archive;
          const std::unique_ptr<A, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
          const bool usable = writer
              && detail::setFormat(writer.get(), format, level) == ARCHIVE_OK
              && detail::addFilter(writer.get(), format, level) == ARCHIVE_OK;
          if (level == Level::Balanced)
            item.create = usable;
          item.levels[static_cast<std::size_t>(level)] = usable && format != Format::Tar;
        }
      }
      return result;
    }();
    return available;
  }
} // namespace filemanager::archive_backend

namespace filemanager::archive_backend::detail {
  namespace {
    const char* compressionLevel(Level level, bool zstd = false) {
      switch (level) {
      case Level::Fast:
        return "1";
      case Level::Balanced:
        return zstd ? "3" : "6";
      case Level::Maximum:
        return zstd ? "19" : "9";
      }
      return "6";
    }
  } // namespace

  bool nativeFilter(Format format) {
    switch (format) {
    case Format::TarBzip2:
      return archive_bzlib_version();
    case Format::TarZstd:
      return archive_libzstd_version();
    case Format::TarXz:
      return lzma_filter_decoder_is_supported(LZMA_FILTER_LZMA2);
    default:
      return true;
    }
  }

  int addFilter(struct archive* writer, Format format, Level level) {
    const char* name = nullptr;
    int status = ARCHIVE_FATAL;
    switch (format) {
    case Format::Gzip:
    case Format::TarGzip:
      if (!archive_zlib_version())
        return ARCHIVE_FATAL;
      name = "gzip";
      status = archive_write_add_filter_gzip(writer);
      break;
    case Format::TarBzip2:
      if (!archive_bzlib_version())
        return ARCHIVE_FATAL;
      name = "bzip2";
      status = archive_write_add_filter_bzip2(writer);
      break;
    case Format::TarZstd:
      if (!archive_libzstd_version())
        return ARCHIVE_FATAL;
      name = "zstd";
      status = archive_write_add_filter_zstd(writer);
      break;
    case Format::TarXz:
      if (!archive_liblzma_version())
        return ARCHIVE_FATAL;
      name = "xz";
      status = archive_write_add_filter_xz(writer);
      if (status == ARCHIVE_OK)
        status = archive_write_set_filter_option(writer, name, "threads", "1");
      break;
    default:
      return archive_write_add_filter_none(writer);
    }
    return status == ARCHIVE_OK
        ? archive_write_set_filter_option(
              writer, name, "compression-level", compressionLevel(level, format == Format::TarZstd)
          )
        : status;
  }

  int setFormat(struct archive* writer, Format format, Level level) {
    if (format == Format::Rar)
      return ARCHIVE_FATAL;
    if (format == Format::Gzip)
      return archive_write_set_format_raw(writer);
    if (format == Format::Zip) {
      const auto status = archive_write_set_format_zip(writer);
      return status == ARCHIVE_OK
          ? archive_write_set_format_option(writer, "zip", "compression-level", compressionLevel(level))
          : status;
    }
    if (format != Format::SevenZip)
      return archive_write_set_format_pax_restricted(writer);
    if (!archive_liblzma_version())
      return ARCHIVE_FATAL;
    auto status = archive_write_set_format_7zip(writer);
    if (status == ARCHIVE_OK)
      status = archive_write_set_format_option(writer, "7zip", "compression", "lzma2");
    if (status == ARCHIVE_OK)
      status = archive_write_set_format_option(writer, "7zip", "compression-level", compressionLevel(level));
    // Older libarchive 7z encoders only support a single thread.
    if (status == ARCHIVE_OK && archive_version_number() >= 3008000)
      status = archive_write_set_format_option(writer, "7zip", "threads", "1");
    return status;
  }
} // namespace filemanager::archive_backend::detail
