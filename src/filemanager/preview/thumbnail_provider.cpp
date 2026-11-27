// Adapted from Luciole (noctalia-dev), MIT. See LICENSE.luciole.
#include "filemanager/preview/thumbnail_provider.h"

#include "core/files/directory_scanner.h"
#include "core/files/display_text.h"
#include "filemanager/preview/provider_process.h"
#include "render/core/image_encoder.h"
#include "render/core/thumbnail_service.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <format>
#ifdef NYTHERA_PREVIEW_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace filemanager::preview {
  namespace {
    struct Fd {
      explicit Fd(int fd) : value(fd) {}
      ~Fd() {
        if (valid())
          ::close(value);
      }
      Fd(const Fd&) = delete;
      Fd& operator=(const Fd&) = delete;
      int get() const { return value; }
      bool valid() const { return value >= 0; }
      int value;
    };

    std::optional<std::vector<std::uint8_t>> readThumbnailFile(const std::filesystem::path& path, std::size_t limit) {
      const Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
      struct stat info{};
      if (!file.valid()
          || ::fstat(file.get(), &info) != 0
          || !S_ISREG(info.st_mode)
          || info.st_size <= 0
          || static_cast<std::uintmax_t>(info.st_size) > limit)
        return std::nullopt;
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          return std::nullopt;
        offset += static_cast<std::size_t>(count);
      }
      return bytes;
    }

    std::expected<DecodedRasterImage, std::string> decodeOutput(const std::vector<std::uint8_t>& bytes, int side) {
      return decodeRasterImage(
          bytes.data(), bytes.size(), {.maxDimension = side, .maxDecodedBytes = 8ULL * side * side + 65536}
      );
    }

    DecodedRasterImage videoFilmstrip(
        DecodedRasterImage frame, int side, const std::function<bool()>& cancel, const std::filesystem::path& directory,
        std::chrono::steady_clock::time_point deadline
    ) {
      g_autofree char* helper = g_find_program_in_path("ffmpegthumbnailer");
      if (!helper || cancel()) {
        return frame;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return frame;
      }
      // Only pass our bounded, re-encoded pixels to the optional helper, never the original media.
      // This retains the source decoder's pixel/protocol limits on older thumbnailer releases too.
      const auto bytes = encodePng(frame.pixels.data(), frame.width, frame.height);
      const auto inputPath = directory / "filmstrip-input.png";
      const Fd input(::open(inputPath.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
      if (bytes.empty() || !input.valid()) {
        return frame;
      }
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = ::write(input.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return frame;
        }
        offset += static_cast<std::size_t>(count);
      }
      if (::lseek(input.get(), 0, SEEK_SET) < 0) {
        return frame;
      }
      const auto remaining = std::min(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
          std::chrono::milliseconds(2000)
      );
      if (remaining <= std::chrono::milliseconds::zero()) {
        return frame;
      }
      const auto output = directory / "filmstrip.png";
      const auto result = runProviderProcess(
          {helper, "-i", "/proc/self/fd/3", "-o", output.native(), "-s", std::to_string(side), "-t", "0", "-f", "-c",
           "png"},
          input.get(), cancel, 8U * 1024U * 1024U, 1, -1, remaining
      );
      if (result) {
        if (const auto encoded = readThumbnailFile(output, 8U * 1024U * 1024U)) {
          if (auto decorated = decodeOutput(*encoded, side);
              decorated && decorated->width == frame.width && decorated->height == frame.height) {
            return std::move(*decorated);
          }
        }
      }
      return frame;
    }
    std::optional<double>
    videoDuration(int source, const std::filesystem::path& directory, const std::function<bool()>& cancel) {
      const auto path = directory / "duration";
      const Fd output(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
      if (!output.valid()
          || !runProviderProcess(
              {"ffprobe", "-v", "error", "-protocol_whitelist", "file,pipe", "-show_entries", "format=duration", "-of",
               "default=noprint_wrappers=1:nokey=1", "/proc/self/fd/3"},
              source, cancel, 4096, 1, output.get(), std::chrono::seconds(2)
          )) {
        return std::nullopt;
      }
      const auto bytes = readThumbnailFile(path, 4096);
      if (!bytes || bytes->empty()) {
        return std::nullopt;
      }
      const auto* begin = reinterpret_cast<const char*>(bytes->data());
      double duration = 0;
      const auto parsed = std::from_chars(begin, begin + bytes->size(), duration);
      return parsed.ec == std::errc{} && std::isfinite(duration) && duration > 0 ? std::optional(duration)
                                                                                 : std::nullopt;
    }
    bool darkFrame(const DecodedRasterImage& image) {
      std::size_t dark = 0;
      const auto pixels = image.pixels.size() / 4;
      for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        dark += image.pixels[i] < 12 && image.pixels[i + 1] < 12 && image.pixels[i + 2] < 12;
      }
      return pixels != 0 && dark * 100 >= pixels * 98;
    }
    std::expected<DecodedRasterImage, std::string> videoThumbnail(
        int source, int side, const std::function<bool()>& cancel, const std::filesystem::path& output, bool browsing
    ) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      const auto duration = videoDuration(source, output.parent_path(), cancel);
      const std::vector<double> positions =
          duration && *duration >= 1 ? std::vector{*duration * 0.1, *duration / 3, 0.0} : std::vector{0.0};
      std::optional<DecodedRasterImage> dark;
      std::string failure = "no video frame available";
      for (const auto position : positions) {
        if (cancel()) {
          return std::unexpected("thumbnail request cancelled");
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
          break;
        }
        std::vector<std::string> arguments{"ffmpeg",   "-nostdin",
                                           "-v",       "error",
                                           "-xerror",  "-threads",
                                           "1",        "-filter_threads",
                                           "1",        "-protocol_whitelist",
                                           "file,pipe"};
        // Decode only keyframes: an accurate seek decodes every frame since the previous keyframe, which dominates
        // generation time for long GOPs.
        arguments.insert(arguments.end(), {"-skip_frame:v", "nokey"});
        if (position > 0) {
          arguments.insert(arguments.end(), {"-noaccurate_seek", "-ss", std::to_string(position)});
        }
        arguments.insert(
            arguments.end(),
            {"-i", "/proc/self/fd/3", "-map", "0:V:0", "-frames:v", "1", "-an", "-sn", "-threads", "1", "-vf",
             "scale=" + std::to_string(side) + ':' + std::to_string(side) + ":force_original_aspect_ratio=decrease",
             "-y", output.native()}
        );
        (void)::unlink(output.c_str());
        const auto result =
            runProviderProcess(std::move(arguments), source, cancel, 8U * 1024U * 1024U, 0, -1, remaining);
        if (!result) {
          failure = result.error();
          continue;
        }
        const auto bytes = readThumbnailFile(output, 8U * 1024U * 1024U);
        auto image = bytes ? decodeOutput(*bytes, side)
                           : std::expected<DecodedRasterImage, std::string>(std::unexpected("no video frame output"));
        if (!image) {
          failure = image.error();
          continue;
        }
        if (!darkFrame(*image)) {
          return browsing ? videoFilmstrip(std::move(*image), side, cancel, output.parent_path(), deadline)
                          : std::move(*image);
        }
        if (dark) {
          break;
        }
        dark = std::move(*image);
      }
      // A genuinely dark video is still a valid preview; do not turn it into a missing thumbnail.
      if (!dark) {
        return std::unexpected(failure);
      }
      return browsing ? videoFilmstrip(std::move(*dark), side, cancel, output.parent_path(), deadline)
                      : std::move(*dark);
    }

  } // namespace

  bool mediaThumbnail(std::string_view mime) {
    return mime.starts_with("audio/") || mime.starts_with("video/") || mime == "application/pdf";
  }

  bool imageThumbnail(std::string_view mime) { return mime.starts_with("image/"); }

  std::optional<DecodedRasterImage> provideThumbnail(
      int fd, const std::string& path, std::size_t bytes, std::size_t sourceLimit, int side, bool browsing,
      const std::function<bool()>& canceled
  ) {
    // Use NytheraLib's current decoders for its established image formats.
    if (DirectoryScanner::isThumbnailPath(path))
      return std::nullopt;
    gboolean uncertain = false;
    g_autofree char* type = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain);
    if (!type || uncertain || std::string_view(type) == "application/octet-stream") {
      std::array<guchar, 4096> header;
      const auto count = ::pread(fd, header.data(), header.size(), 0);
      if (count > 0) {
        g_free(type);
        type = g_content_type_guess(path.c_str(), header.data(), static_cast<std::size_t>(count), nullptr);
      }
    }
    const std::string_view mime = type ? type : "";
    const bool image = imageThumbnail(mime);
    if (!image && !mediaThumbnail(mime))
      return std::nullopt;
#ifndef NYTHERA_PREVIEW_PIXBUF
    if (image)
      return std::nullopt;
#endif
    if (canceled() || ((image || mime == "application/pdf") && bytes > sourceLimit))
      return std::nullopt;
    if (!image)
      side = std::min(side, 1024);

    g_autofree char* temporary = g_dir_make_tmp("nythera-thumbnail-XXXXXX", nullptr);
    if (!temporary)
      return std::nullopt;
    struct Cleanup {
      std::filesystem::path directory;
      ~Cleanup() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
      }
    } work{temporary};
    const auto output = work.directory / "result.png";
    if (mime.starts_with("video/")) {
      auto frame = videoThumbnail(fd, side, canceled, output, browsing);
      return frame ? std::optional(std::move(*frame)) : std::nullopt;
    }
    const std::uint64_t outputLimit = std::max(8ULL * 1024 * 1024, 8ULL * side * side);
    std::expected<void, std::string> result;
    if (image) {
      result = runProviderProcess(
          {"/proc/self/exe", "--internal-preview-image", std::to_string(side), output.string()}, fd, canceled,
          kProviderMemoryBytes, 2
      );
    } else if (mime == "application/pdf") {
      result = runProviderProcess(
          {"pdftoppm", "-f", "1", "-singlefile", "-scale-to", std::to_string(side), "-png", "-q", "/proc/self/fd/3",
           (work.directory / "result").string()},
          fd, canceled
      );
    } else {
      result = runProviderProcess(
          {"ffmpeg",
           "-nostdin",
           "-v",
           "error",
           "-xerror",
           "-threads",
           "1",
           "-filter_threads",
           "1",
           "-protocol_whitelist",
           "file,pipe",
           "-i",
           "/proc/self/fd/3",
           "-map",
           "0:v:0",
           "-frames:v",
           "1",
           "-an",
           "-sn",
           "-threads",
           "1",
           "-vf",
           "thumbnail=4,scale="
               + std::to_string(side)
               + ':'
               + std::to_string(side)
               + ":force_original_aspect_ratio=decrease",
           "-y",
           output.string()},
          fd, canceled
      );
    }
    if (!result)
      return std::nullopt;
    const auto encoded = readThumbnailFile(output, outputLimit);
    if (!encoded)
      return std::nullopt;
    auto decoded = decodeOutput(*encoded, side);
    return decoded ? std::optional(std::move(*decoded)) : std::nullopt;
  }

  std::vector<Detail> mediaDetails(int fd, std::string_view mime, const std::function<bool()>& canceled) {
    const bool pdf = mime == "application/pdf";
    Fd output(::memfd_create("nythera-preview-metadata", MFD_CLOEXEC));
    if (!output.valid())
      return {};
    const auto arguments = pdf ? std::vector<std::string>{"pdfinfo", "/proc/self/fd/3"}
                               : std::vector<std::string>{
                                     "ffprobe",
                                     "-v",
                                     "error",
                                     "-protocol_whitelist",
                                     "file,pipe",
                                     "-show_entries",
                                     "format=duration,format_long_name:format_tags=title,artist,album:stream=width,"
                                     "height,sample_rate,channels",
                                     "-of",
                                     "default=noprint_wrappers=1",
                                     "/proc/self/fd/3"
                                 };
    if (!runProviderProcess(arguments, fd, canceled, 4096, 1, output.get(), std::chrono::seconds(2)))
      return {};
    std::array<char, 4096> buffer;
    const auto count = ::pread(output.get(), buffer.data(), buffer.size(), 0);
    if (count <= 0)
      return {};
    auto value = [&](std::string_view key) -> std::string_view {
      std::string_view data(buffer.data(), static_cast<std::size_t>(count));
      while (!data.empty()) {
        const auto end = data.find('\n');
        auto line = data.substr(0, end);
        if (line.starts_with(key)) {
          line.remove_prefix(key.size());
          const auto first = line.find_first_not_of(" \t");
          return first == std::string_view::npos ? std::string_view{} : line.substr(first);
        }
        if (end == std::string_view::npos)
          break;
        data.remove_prefix(end + 1);
      }
      return {};
    };
    const auto numeric = [&](std::string_view key) {
      const auto text = value(key);
      int number = 0;
      if (!text.empty())
        (void)std::from_chars(text.data(), text.data() + text.size(), number);
      return number;
    };
    std::vector<Detail> details;
    const auto add = [&](std::string label, std::string_view text) {
      if (!text.empty() && text != "N/A")
        details.push_back({std::move(label), sanitizeFilenameForDisplay(text)});
    };
    if (pdf) {
      add("Pages", value("Pages:"));
      add("Page Size", value("Page size:"));
      return details;
    }
    const int width = numeric("width=");
    const int height = numeric("height=");
    const int sampleRate = numeric("sample_rate=");
    const int channels = numeric("channels=");
    const int duration = numeric("duration=");
    add("Format", value("format_long_name="));
    if (width > 0 && height > 0)
      add("Frame Size", std::format("{} × {} px", width, height));
    if (sampleRate > 0)
      add("Sample Rate", std::format("{} Hz", sampleRate));
    if (channels > 0)
      add("Channels", std::to_string(channels));
    if (duration > 0)
      add("Duration", std::format("{}:{:02}", duration / 60, duration % 60));
    add("Title", value("TAG:title="));
    add("Artist", value("TAG:artist="));
    add("Album", value("TAG:album="));
    return details;
  }

  int runImageWorker(const char* side, const char* output) {
#ifdef NYTHERA_PREVIEW_PIXBUF
    int size = 0;
    const std::string_view argument(side);
    const auto parsed = std::from_chars(argument.begin(), argument.end(), size);
    struct stat info{};
    if (parsed.ec != std::errc{}
        || parsed.ptr != argument.end()
        || size <= 0
        || size > ThumbnailService::kMaxSvgRenderPx
        || ::fstat(3, &info) != 0
        || !S_ISREG(info.st_mode)
        || info.st_size <= 0
        || static_cast<std::uintmax_t>(info.st_size) > ThumbnailService::kMaxSourceFileBytes)
      return 1;
    int width = 0;
    int height = 0;
    if (!gdk_pixbuf_get_file_info("/proc/self/fd/3", &width, &height))
      return 1;
    size = std::min(size, std::max(width, height));
    g_autoptr(GdkPixbuf) decoded = gdk_pixbuf_new_from_file_at_scale("/proc/self/fd/3", size, size, true, nullptr);
    if (!decoded)
      return 1;
    g_autoptr(GdkPixbuf) image = gdk_pixbuf_apply_embedded_orientation(decoded);
    return image && gdk_pixbuf_save(image, output, "png", nullptr, nullptr) ? 0 : 1;
#else
    (void)side;
    (void)output;
    return 1;
#endif
  }
} // namespace filemanager::preview
