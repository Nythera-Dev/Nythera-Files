#include "filemanager/preview/preview_content.h"
#include "filemanager/preview/thumbnail_provider.h"
#include "render/core/image_encoder.h"
#include "test_isolation.h"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <gio/gio.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  void write(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    TEST_CHECK(file.good());
  }

  bool runFfmpeg(const char* program, const std::vector<std::string>& options) {
    std::vector<const char*> argv{program, "-nostdin", "-v", "error"};
    for (const auto& option : options)
      argv.push_back(option.c_str());
    argv.push_back(nullptr);
    int status = 0;
    return g_spawn_sync(
               nullptr, const_cast<char**>(argv.data()), nullptr,
               static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr, nullptr,
               nullptr, nullptr, &status, nullptr
           )
        && g_spawn_check_wait_status(status, nullptr);
  }

  std::string samplePdf() {
    constexpr std::string_view drawing = "0 0 1 rg 0 0 100 100 re f\n";
    const std::array objects{
        std::string("<< /Type /Catalog /Pages 2 0 R >>"),
        std::string("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        std::string("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >>"),
        std::format("<< /Length {} >>\nstream\n{}endstream", drawing.size(), drawing),
    };
    std::string pdf = "%PDF-1.4\n";
    std::string xref = "xref\n0 5\n0000000000 65535 f \n";
    for (std::size_t i = 0; i < objects.size(); ++i) {
      xref += std::format("{:010} 00000 n \n", pdf.size());
      pdf += std::format("{} 0 obj\n{}\nendobj\n", i + 1, objects[i]);
    }
    const auto index = pdf.size();
    pdf += std::format("{}trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n{}\n%%EOF\n", xref, index);
    return pdf;
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-files-preview"});
    using namespace filemanager;
    using namespace filemanager::preview;
    const auto complete = previewText("\xEF\xBB\xBFline\tfirst\r\nline two", false);
    TEST_CHECK(complete.text == "line    first\nline two" && complete.notice.empty());
    const auto splitUtf8 = previewText("hi\xE2\x80", true);
    TEST_CHECK(splitUtf8.text == "hi" && splitUtf8.notice.starts_with("Preview truncated."));
    const auto invalidUtf8 = previewText("hi\xE2\x80", false);
    TEST_CHECK(invalidUtf8.text.empty() && invalidUtf8.notice.find("UTF-8") != std::string::npos);
    const auto longLine = previewText(std::string(400, 'a'), false);
    TEST_CHECK(longLine.text.ends_with("…") && longLine.notice.starts_with("Preview truncated."));
    TEST_CHECK(!previewText({}, false).notice.empty());
    TEST_CHECK(textMime("application/yaml") && textMime("application/x-desktop"));
    TEST_CHECK(!textMime("image/svg+xml") && !textMime("application/zip"));

    const auto document = env.work() / "sample.yaml";
    constexpr std::string_view yaml = "example: true\n";
    write(document, yaml);
    Request request{
        .entry = {.name = "sample.yaml", .absPath = document, .size = yaml.size(), .type = "application/yaml"},
        .source = document.string(),
        .mode = PreviewMode::Full,
    };
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    TEST_CHECK(collect(request, cancel).text == yaml);
    const std::string longText(65 * 1024, 'a');
    write(document, longText);
    const auto truncated = collect(request, cancel);
    TEST_CHECK(!truncated.text.empty() && truncated.notice.starts_with("Preview truncated."));
    request.mode = PreviewMode::Information;
    TEST_CHECK(collect(request, cancel).text.empty());
    request.mode = PreviewMode::Full;
    request.source = "archive:/sample.yaml";
    TEST_CHECK(collect(request, cancel).text.empty());
    request.source = document.string();
    g_cancellable_cancel(cancel);
    TEST_CHECK(collect(request, cancel).text.empty());

    const auto documentPdf = env.work() / "sample.pdf";
    const auto pdf = samplePdf();
    write(documentPdf, pdf);
    const int fd = ::open(documentPdf.c_str(), O_RDONLY | O_CLOEXEC);
    TEST_CHECK(fd >= 0);
    const auto close = nythera::test::makeScopeExit([fd] { ::close(fd); });
    constexpr auto neverCanceled = [] { return false; };
    TEST_CHECK(!provideThumbnail(fd, documentPdf.string(), pdf.size(), pdf.size() - 1, 128, true, neverCanceled));
    TEST_CHECK(!provideThumbnail(fd, documentPdf.string(), pdf.size(), pdf.size(), 128, true, [] { return true; }));
    g_autofree char* pdftoppm = g_find_program_in_path("pdftoppm");
    if (pdftoppm) {
      const auto result = provideThumbnail(fd, documentPdf.string(), pdf.size(), pdf.size(), 128, true, neverCanceled);
      TEST_CHECK(result && result->width == 128 && result->height == 128);
    }
    g_autofree char* pdfinfo = g_find_program_in_path("pdfinfo");
    if (pdfinfo)
      TEST_CHECK(std::ranges::contains(mediaDetails(fd, "application/pdf", neverCanceled), Detail{"Pages", "1"}));

    g_autofree char* ffmpeg = g_find_program_in_path("ffmpeg");
    if (ffmpeg) {
      const auto video = env.work() / "sample.avi";
      // The raw video encoder and AVI muxer require no external codecs.
      TEST_CHECK(runFfmpeg(
          ffmpeg,
          {"-f", "lavfi", "-i", "color=red:size=48x32:duration=1", "-threads", "1", "-c:v", "rawvideo", "-pix_fmt",
           "yuv420p", video.string()}
      ));
      const int videoFd = ::open(video.c_str(), O_RDONLY | O_CLOEXEC);
      TEST_CHECK(videoFd >= 0);
      const auto closeVideo = nythera::test::makeScopeExit([videoFd] { ::close(videoFd); });
      const auto videoSize = fs::file_size(video);
      const auto frame = provideThumbnail(videoFd, video.string(), videoSize, 1, 128, false, neverCanceled);
      TEST_CHECK(frame && frame->width == 128 && frame->height < frame->width);
      TEST_CHECK(frame->pixels[0] > frame->pixels[1] + 100);
      g_autofree char* ffprobe = g_find_program_in_path("ffprobe");
      if (ffprobe)
        TEST_CHECK(
            std::ranges::contains(
                mediaDetails(videoFd, "video/x-msvideo", neverCanceled), Detail{"Frame Size", "48 × 32 px"}
            )
        );

      const auto cover = env.work() / "cover.png";
      const std::vector<std::uint8_t> rgba(24 * 24 * 4, 255);
      const auto png = encodePng(rgba.data(), 24, 24);
      write(cover, {reinterpret_cast<const char*>(png.data()), png.size()});
      const auto audio = env.work() / "sample.mp3";
      // Embedded artwork is optional on minimal FFmpeg builds without an MP3 encoder.
      if (runFfmpeg(
              ffmpeg,
              {"-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-i", cover.string(), "-map", "0:a", "-map", "1:v",
               "-c:v", "copy", "-disposition:v", "attached_pic", audio.string()}
          )) {
        const int audioFd = ::open(audio.c_str(), O_RDONLY | O_CLOEXEC);
        TEST_CHECK(audioFd >= 0);
        const auto closeAudio = nythera::test::makeScopeExit([audioFd] { ::close(audioFd); });
        const auto artwork =
            provideThumbnail(audioFd, audio.string(), fs::file_size(audio), 1, 96, false, neverCanceled);
        TEST_CHECK(artwork && artwork->width == 96 && artwork->height == 96);
      }
    }
  });
}
