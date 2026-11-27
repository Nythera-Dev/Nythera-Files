#include "filemanager/file_search_session.h"
#include "test_isolation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  using namespace filemanager;
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;

  void write(const fs::path& path, std::string_view contents) {
    TEST_CHECK(static_cast<bool>(std::ofstream(path, std::ios::binary) << contents));
  }

  template <typename Predicate> void waitFor(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      if (!g_main_context_iteration(nullptr, false))
        std::this_thread::sleep_for(1ms);
    }
    TEST_CHECK(ready());
  }

  void metadata() {
    SearchFilterDraft draft;
    TEST_CHECK(draft.sizeFirst.empty() && draft.sizeSecond.empty() && draft.dateAmount.empty());
    TEST_CHECK(!resolveSizeFilter(draft));
    draft.sizeFirstUnit = SizeUnit::KB;
    draft.sizeFirst = "1.5";
    auto size = [&] { return resolveSizeFilter(draft).value(); };
    TEST_CHECK(!size().matches(1500) && size().matches(1501));
    draft.sizeComparison = SizeComparison::Smaller;
    TEST_CHECK(size().matches(1499) && !size().matches(1500));
    draft.sizeComparison = SizeComparison::Exactly;
    TEST_CHECK(!size().matches(1499) && size().matches(1500));
    draft.sizeComparison = SizeComparison::Between;
    draft.sizeSecondUnit = SizeUnit::KiB;
    TEST_CHECK(!resolveSizeFilter(draft));
    draft.sizeSecond = "2";
    TEST_CHECK(!size().matches(1499) && size().matches(1500) && size().matches(2048) && !size().matches(2049));
    draft.sizeSecond = "1";
    TEST_CHECK(!resolveSizeFilter(draft));
    draft.sizeComparison = SizeComparison::Exactly;
    draft.sizeFirst = "18446744073709551615";
    TEST_CHECK(!resolveSizeFilter(draft));
    draft.sizeFirstUnit = SizeUnit::Bytes;
    TEST_CHECK(size().matches(UINT64_MAX));
    draft.sizeFirst = "1";
    constexpr std::array<std::uint64_t, 9> units{1,          1000,       1'000'000,  1'000'000'000, 1'000'000'000'000,
                                                 1ULL << 10, 1ULL << 20, 1ULL << 30, 1ULL << 40};
    for (std::size_t index = 0; index < units.size(); ++index) {
      draft.sizeFirstUnit = static_cast<SizeUnit>(index);
      TEST_CHECK(size().matches(units[index]));
    }

    g_autoptr(GTimeZone) zone = g_time_zone_new_identifier("Europe/London");
    TEST_CHECK(zone);
    g_autoptr(GDateTime) now = g_date_time_new(zone, 2024, 3, 31, 12, 0, 0);
    const auto timestamp = [&](int year, int month, int day, int hour = 0) {
      g_autoptr(GDateTime) date = g_date_time_new(zone, year, month, day, hour, 0, 0);
      return g_date_time_to_unix(date);
    };
    auto date = [&] { return resolveModifiedFilter(draft, now).value(); };
    TEST_CHECK(!resolveModifiedFilter(draft, now));
    draft.dateAmount = "1";
    draft.dateComparison = DateComparison::Within;
    const std::array cutoffs{
        timestamp(2024, 3, 31, 11), timestamp(2024, 3, 30, 12), timestamp(2024, 3, 24, 12), timestamp(2024, 2, 29, 12),
        timestamp(2023, 3, 31, 12)
    };
    for (std::size_t index = 0; index < cutoffs.size(); ++index) {
      draft.dateUnit = static_cast<DateUnit>(index);
      TEST_CHECK(!date().matches(cutoffs[index] - 1) && date().matches(cutoffs[index]));
      TEST_CHECK(!date().matches(timestamp(2024, 3, 31, 13)));
    }
    draft.dateComparison = DateComparison::Older;
    TEST_CHECK(date().matches(cutoffs.back() - 1) && !date().matches(cutoffs.back()));
    draft.dateFirst = {2024, 3, 31};
    draft.dateSecond = draft.dateFirst;
    draft.dateComparison = DateComparison::Before;
    TEST_CHECK(date().matches(timestamp(2024, 3, 31) - 1) && !date().matches(timestamp(2024, 3, 31)));
    draft.dateComparison = DateComparison::After;
    TEST_CHECK(!date().matches(timestamp(2024, 4, 1) - 1) && date().matches(timestamp(2024, 4, 1)));
    draft.dateComparison = DateComparison::Between;
    TEST_CHECK(date().matches(timestamp(2024, 3, 31)) && date().matches(timestamp(2024, 4, 1) - 1));
    TEST_CHECK(!date().matches(timestamp(2024, 4, 1)));
    draft.dateSecond = {2024, 2, 30};
    TEST_CHECK(!resolveModifiedFilter(draft, now));

    draft.sizeFirst.clear();
    draft.dateAmount.clear();
    draft.content.query = "draft";
    TEST_CHECK(!resolveSearchFilters(draft, now).active());
    draft.submitted = draft.content;
    draft.content.query += " edited";
    TEST_CHECK(resolveSearchFilters(draft, now).content->query == "draft");
    draft.content.query.clear();
    TEST_CHECK(!resolveSearchFilters(draft, now).active());
  }

  std::string tar(std::string_view name, std::string_view text) {
    // Deterministic ustar fixture. Two trailing blocks terminate the archive.
    std::array<char, 512> header{};
    TEST_CHECK(name.size() < 100);
    std::copy(name.begin(), name.end(), header.begin());
    std::snprintf(header.data() + 100, 8, "%07o", 0644);
    std::snprintf(header.data() + 124, 12, "%011zo", text.size());
    std::fill_n(header.data() + 148, 8, ' ');
    header[156] = '0';
    std::copy_n("ustar", 5, header.data() + 257);
    unsigned int checksum = 0;
    for (unsigned char byte : header)
      checksum += byte;
    std::snprintf(header.data() + 148, 7, "%06o", checksum);
    std::string data(header.data(), header.size());
    data += text;
    data.resize(((data.size() + 511) / 512 + 2) * 512, '\0');
    return data;
  }

  void archive(const fs::path& path, std::string_view text) { write(path, tar("entry.txt", text)); }

  constexpr FileSearchSession::Order kOrder{FileDialogSortField::Name, FileDialogSortOrder::Ascending, false, false};

  void begin(
      FileSearchSession& session, const fs::path& root, std::string name, SearchFilters filters,
      FileSearchSession::Changed notify = {}, FileSearchSession::BuildPlan plan = {}, FzfSearch fzf = {}
  ) {
    if (!notify)
      notify = [](bool) {};
    session.edit(
        root, std::move(name), false, kOrder, std::move(notify), {}, std::move(fzf), std::move(plan), std::move(filters)
    );
    session.submit();
  }

  std::set<std::string> results(FileSearchSession& session) {
    waitFor([&] { return session.status() != FileSearchSession::Status::Searching; });
    TEST_CHECK(session.status() == FileSearchSession::Status::Finished);
    std::set<std::string> names;
    for (const auto& entry : session.entries())
      TEST_CHECK(names.insert(entry.name).second);
    return names;
  }

  void integration(const fs::path& root) {
    fs::create_directory(root);
    const auto unusual = std::string("-weird\t\n") + char(0xff) + ".txt";
    write(root / unusual, "Needle\nNeedle\n");
    write(root / "plain.txt", "needle anywhere\n");
    write(root / "binary", std::string_view("Needle\0binary", 13));
    write(root / ".hidden.txt", "Needle\n");
    fs::create_directory(root / "excluded");
    write(root / "excluded" / "match.txt", "Needle\n");
    TEST_CHECK(mkfifo((root / "pipe").c_str(), 0600) == 0);
    fs::create_symlink("plain.txt", root / "link");
    fs::create_symlink("excluded/match.txt", root / "excluded-link");
    write(root.parent_path() / "outside.txt", "Needle\n");
    fs::create_symlink(root.parent_path() / "outside.txt", root / "outside-link");
    fs::create_symlink("pipe", root / "pipe-link");
    archive(root / "packed.tar", "Needle packed\n");
    archive(root / (std::string("pack\n") + char(0xff) + ".TAR"), "Needle packed twice\n");
    write(root / "bad.tar", "malformed archive\n");
    const auto modification = std::time_t(1'700'000'000);
    const timespec dates[2]{{0, UTIME_OMIT}, {modification, 0}};
    TEST_CHECK(utimensat(AT_FDCWD, (root / unusual).c_str(), dates, 0) == 0);

    const auto plan = [&] {
      auto resolved = resolveSearchScope({}, root, {}, nullptr, {}, discoverSearchMounts());
      resolved.exclusions.insert((root / "excluded").string());
      return resolved;
    };
    FileSearchSession session;
    begin(session, root, {}, {.size = SizeFilter{SizeComparison::Exactly, 14, 14}}, {}, plan);
    TEST_CHECK(results(session) == std::set{unusual});
    begin(session, root, {}, {.modified = ModifiedFilter{modification, modification + 1}}, {}, plan);
    TEST_CHECK(results(session) == std::set{unusual});

    const auto backends = findContentBackends();
    if (!backends.supports(ContentMode::Plain)) {
      std::cout << "SKIP external backend integration (rg unavailable)\n";
      return;
    }
    const auto run = [&](ContentFilter filter) {
      begin(session, root, {}, {.content = std::move(filter)}, {}, plan);
      return results(session);
    };
    TEST_CHECK(run({"Needle", ContentMode::Plain, true}) == std::set{unusual});
    TEST_CHECK((run({"^needle.*$", ContentMode::Plain, false, true}) == std::set{unusual, std::string("plain.txt")}));
    TEST_CHECK(run({"[", ContentMode::Plain, false, true}).empty());
    TEST_CHECK(session.contentIssues().backendError && session.contentNotice().contains("regular expression"));
    auto follow = [&] {
      auto resolved = plan();
      resolved.followSymlinks = true;
      return resolved;
    };
    begin(session, root, {}, {.content = ContentFilter{"Needle", ContentMode::Plain}}, {}, follow);
    TEST_CHECK((results(session) == std::set{unusual, std::string("plain.txt"), std::string("link")}));
    for (const auto& entry : session.entries()) {
      TEST_CHECK(entry.isSymlink == (entry.name == "link"));
      TEST_CHECK(entry.hasIdentity == !entry.isSymlink);
      if (entry.hasIdentity) {
        struct stat info{};
        TEST_CHECK(stat(entry.absPath.c_str(), &info) == 0);
        TEST_CHECK(entry.device == info.st_dev && entry.inode == info.st_ino);
      }
    }
    begin(
        session, root, "weird",
        {.size = SizeFilter{SizeComparison::Exactly, 14, 14},
         .modified = ModifiedFilter{modification, modification + 1},
         .content = ContentFilter{"Needle", ContentMode::Plain, true}},
        {}, plan, FzfSearch{findFzf(), SearchMode::Contiguous, false}
    );
    TEST_CHECK(results(session) == std::set{unusual});
    begin(session, root, {}, {.content = ContentFilter{"Needle", ContentMode::Plain}}, {}, plan);
    begin(session, root, "plain", {.content = ContentFilter{"needle", ContentMode::Plain}}, {}, plan);
    TEST_CHECK(results(session) == std::set{std::string("plain.txt")});

    if (!backends.supports(ContentMode::Extracted)) {
      std::cout << "SKIP extraction integration (rga/rga-preproc unavailable)\n";
      return;
    }
    write(root / "mail.eml", "Subject: Needle\n\nNeedle");
    write(root / "large.txt", "Needle\n" + std::string(8192, '\n'));
    fs::resize_file(root / "large.txt", 128ULL * 1024 * 1024 + 1);
    archive(root / "too-large.tar", "Needle");
    fs::resize_file(root / "too-large.tar", 128ULL * 1024 * 1024 + 1);
    const auto unusualArchive = std::string("pack\n") + char(0xff) + ".TAR";
    TEST_CHECK((run({"Needle", ContentMode::Extracted}) == std::set{unusualArchive, std::string("packed.tar")}));
    TEST_CHECK(session.contentIssues().extractionFailed == 1 && !session.contentIssues().backendError);
    TEST_CHECK(
        (run({"Needle", ContentMode::All})
         == std::set{
             unusual, unusualArchive, std::string("plain.txt"), std::string("packed.tar"), std::string("large.txt")
         })
    );
    TEST_CHECK(session.contentIssues().sizeRestricted == 1);
    TEST_CHECK(session.contentIssues().unsupported == 2);
    TEST_CHECK(!session.contentIssues().backendError);
    TEST_CHECK(nythera::test::isPathWithinRoot(searchExtractionCache(), root.parent_path().parent_path() / "cache"));
    TEST_CHECK(fs::is_directory(searchExtractionCache()));
    const auto oldMtime = fs::last_write_time(root / "packed.tar");
    archive(root / "packed.tar", "fresh content\n");
    fs::last_write_time(root / "packed.tar", oldMtime);
    TEST_CHECK(run({"Needle", ContentMode::Extracted}) == std::set{unusualArchive});
    write(root / "nested.tar", tar("a.tar", tar("b.tar", tar("c.tar", tar("entry.txt", "DeepNeedle\n")))));
    TEST_CHECK(run({"archive recursion reached", ContentMode::Extracted}).empty());
    TEST_CHECK(session.contentIssues().archiveTruncated == 1);
    TEST_CHECK(run({"DeepNeedle", ContentMode::Extracted}).empty());
    TEST_CHECK(session.contentIssues().archiveTruncated == 1);
  }

  void optionalBackends(const fs::path& root) {
    TEST_CHECK(!ContentBackends{}.supports(ContentMode::All));
    const auto backends = findContentBackends();
    if (backends.rg.empty())
      return;
    const auto file = root / "optional.txt";
    write(file, "Needle\n");
    std::vector<ContentCandidate> candidates;
    candidates.emplace_back(FileEntry{.name = "optional.txt", .absPath = file}, open(file.c_str(), O_PATH | O_CLOEXEC));
    TEST_CHECK(candidates.front().fd >= 0);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    ContentSearch neither({"Needle"}, {});
    TEST_CHECK(neither.match(candidates, cancel).issues.backendError);
    ContentSearch noExtraction({"Needle", ContentMode::Extracted}, {backends.rg, {}});
    TEST_CHECK(noExtraction.match(candidates, cancel).issues.backendError);
    ContentSearch noRga({"Needle"}, {backends.rg, {}});
    TEST_CHECK(noRga.match(candidates, cancel).entries.size() == 1 && !noRga.notice().empty());

    if (!backends.rga.empty()) {
      const auto mockRga = root / "list-adapters";
      write(
          mockRga,
          "#!/bin/sh\nprintf '%s\\n' 'Adapters:' ' - **sample**' "
          "'     Runs: nythera-nonexistent-extractor-365b' '     Extensions: .sample' "
          "'The following adapters are disabled by default'\n"
      );
      TEST_CHECK(chmod(mockRga.c_str(), 0700) == 0);
      const auto document = root / "file.sample";
      write(document, "Needle\n");
      std::vector<ContentCandidate> documents;
      documents.emplace_back(FileEntry{.absPath = document}, open(document.c_str(), O_PATH | O_CLOEXEC));
      ContentSearch missing({"Needle", ContentMode::All}, {backends.rg, mockRga});
      TEST_CHECK(missing.match(documents, cancel).issues.extractionFailed == 1);
      TEST_CHECK(missing.notice().contains("sample (requires nythera-nonexistent-extractor-365b)"));
    }
  }

  void separatedExtraction(const fs::path& root) {
    const auto backends = findContentBackends();
    if (backends.rg.empty())
      return;

    fs::create_directory(root);
    const auto bin = root / "bin";
    const auto files = root / "files";
    fs::create_directory(bin);
    fs::create_directory(files);
    write(
        bin / "rga",
        "#!/bin/sh\nprintf '%s\\n' 'Adapters:' ' - **ffmpeg**' '     Extensions: .mp4' "
        "'The following adapters are disabled by default'\n"
    );
    for (const auto* command : {"ffprobe", "ffmpeg"}) {
      write(bin / command, "#!/bin/sh\nexit 0\n");
      TEST_CHECK(chmod((bin / command).c_str(), 0700) == 0);
    }
    write(
        bin / "rga-preproc",
        R"sh(#!/bin/sh
for input; do :; done
printf 'metadata: format.filename="%s"\n' "$input"
case "$(cat "$input")" in
  tagged) printf 'metadata: format.tags.title="Nythera"\n';;
  slow)
    touch "$NYTHERA_TEST_GATE.started"
    while ! [ -e "$NYTHERA_TEST_GATE" ]; do sleep 0.01; done;;
  timeout) kill -XCPU "$$";;
esac
)sh"
    );
    TEST_CHECK(chmod((bin / "rga").c_str(), 0700) == 0);
    TEST_CHECK(chmod((bin / "rga-preproc").c_str(), 0700) == 0);
    const auto gate = root / "gate";
    const nythera::test::ScopedEnvVar gateEnv("NYTHERA_TEST_GATE", gate.native());
    const nythera::test::ScopedEnvVar pathEnv("PATH", bin.native() + ":" + g_getenv("PATH"));
    g_auto(GStrv) savedMimeDirs = g_strdupv(const_cast<char**>(g_content_type_get_mime_dirs()));
    const char* mimeDirs[] = {"/usr/share/mime", nullptr};
    g_content_type_set_mime_dirs(mimeDirs);
    auto restoreMimeDirs = nythera::test::makeScopeExit([&] { g_content_type_set_mime_dirs(savedMimeDirs); });

    write(files / "plain.txt", "Nythera\n");
    write(files / "alias-only.mp4", "untagged");
    write(files / "tagged.mp4", "tagged");
    write(files / "slow.mp4", "slow");
    write(files / "timeout.mp4", "timeout");
    FileSearchSession session;
    auto release = nythera::test::makeScopeExit([&] { write(gate, ""); });
    begin(session, files, {}, {.content = ContentFilter{"Nythera"}});
    waitFor([&] {
      return fs::exists(gate.native() + ".started")
          && std::ranges::find(session.entries(), "plain.txt", &FileEntry::name) != session.entries().end();
    });
    TEST_CHECK(session.status() == FileSearchSession::Status::Searching);
    write(gate, "");
    TEST_CHECK((results(session) == std::set{std::string("plain.txt"), std::string("tagged.mp4")}));
    const auto video = std::ranges::find(session.entries(), "tagged.mp4", &FileEntry::name);
    TEST_CHECK(
        video != session.entries().end() && video->type == "video/mp4" && video->hasIdentity && !video->isSymlink
    );
    TEST_CHECK(session.contentIssues().extractionTimeouts == 1);
    TEST_CHECK(!session.contentIssues().backendError && session.contentIssues().extractionFailed == 0);
  }

  void partialPlainBatch(const fs::path& root) {
    const auto fake = root / "rg-timeout";
    write(
        fake, "#!/bin/sh\nwhile [ \"$1\" != -- ]; do shift; done\nshift\nprintf '%s\\000' \"$1\"\nkill -XCPU \"$$\"\n"
    );
    TEST_CHECK(chmod(fake.c_str(), 0700) == 0);
    std::vector<ContentCandidate> candidates;
    for (const auto* name : {"first.txt", "second.txt"}) {
      const auto file = root / name;
      write(file, "needle\n");
      candidates.emplace_back(FileEntry{.name = name, .absPath = file}, open(file.c_str(), O_PATH | O_CLOEXEC));
    }
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    ContentSearch content({"needle", ContentMode::Plain}, {fake, {}});
    const auto result = content.match(candidates, cancel);
    TEST_CHECK(result.entries.size() == 1 && result.entries.front().name == "first.txt");
    TEST_CHECK(result.issues.plainTimeouts == 1 && result.issues.inaccessible == 0 && !result.issues.backendError);
  }

  void cancellation(const fs::path& root) {
    const auto file = root / "cancellable.txt";
    write(file, "needle\n");
    const auto process = root / "pid";
    const auto fake = root / "rg";
    write(fake, std::string("#!/bin/sh\nsleep 30 &\nprintf '%s\\n' \"$!\" > '") + process.native() + "'\nwait\n");
    TEST_CHECK(chmod(fake.c_str(), 0700) == 0);

    ContentSearch content({"needle", ContentMode::Plain}, {fake, {}});
    std::vector<ContentCandidate> candidates;
    candidates.emplace_back(FileEntry{.absPath = file}, open(file.c_str(), O_PATH | O_CLOEXEC));
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    std::atomic<bool> finished = false;
    std::jthread worker([&] {
      const auto result = content.match(candidates, cancel);
      TEST_CHECK(result.entries.empty());
      finished = true;
    });
    auto stop = nythera::test::makeScopeExit([&] { g_cancellable_cancel(cancel); });
    pid_t child = -1;
    waitFor([&] { return static_cast<bool>(std::ifstream(process) >> child); });
    const auto start = std::chrono::steady_clock::now();
    g_cancellable_cancel(cancel);
    waitFor([&] { return finished.load(); });
    TEST_CHECK(std::chrono::steady_clock::now() - start < 1s);
    waitFor([&] {
      std::ifstream state("/proc/" + std::to_string(child) + "/stat");
      std::string info;
      return !std::getline(state, info) || info.substr(info.rfind(')') + 2).starts_with('Z');
    });
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-search-filters", .setXdgDirs = true});
    metadata();
    integration(env.work() / "search");
    optionalBackends(env.work());
    separatedExtraction(env.work() / "lanes");
    partialPlainBatch(env.work());
    cancellation(env.work());
  });
}
