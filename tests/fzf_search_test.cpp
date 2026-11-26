#include "filemanager/directory_session.h"
#include "filemanager/file_search_session.h"
#include "filemanager/fzf_search.h"
#include "test_isolation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
  using namespace filemanager;
  using enum SearchMode;
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;

  template <typename Predicate> void waitFor(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      if (!g_main_context_iteration(nullptr, false))
        std::this_thread::sleep_for(1ms);
    }
    TEST_CHECK(ready());
  }

  void write(const fs::path& path) { TEST_CHECK(static_cast<bool>(std::ofstream(path) << "fixture")); }

  void testRecognition(const fs::path& folder) {
    TEST_CHECK(!manualSearchMode("two words"));
    TEST_CHECK(!manualSearchMode(R"(\^foo bar\$)"));
    TEST_CHECK(manualSearchMode("'alpha") == Contiguous);
    TEST_CHECK(manualSearchMode("^alpha") == StartsWith);
    TEST_CHECK(manualSearchMode("alpha$") == EndsWith);
    TEST_CHECK(manualSearchMode("!alpha") == Exclude);
    TEST_CHECK(manualSearchMode("^alpha$") == Exact);
    TEST_CHECK(manualSearchMode("alpha | bravo") == None);
    TEST_CHECK(manualSearchMode("alpha !bravo") == None);
    TEST_CHECK(manualSearchMode("!^alpha") == None);
    TEST_CHECK(manualSearchMode("'alpha'") == None);
    TEST_CHECK(manualSearchMode(R"(^two\ words$)") == Exact);
    TEST_CHECK(manualSearchMode(R"(^literal\$)") == StartsWith);
    TEST_CHECK(!manualSearchMode("file|name"));

    DirectorySession directory(folder, {});
    directory.configureSearchBackend("/usr/bin/fzf", StartsWith);
    directory.openSearch();
    directory.setSearchMode(Fuzzy);
    directory.setSearchQuery("^alpha$");
    TEST_CHECK(directory.searchMode() == Exact);
    directory.setSearchQuery("alpha | beta");
    TEST_CHECK(directory.searchMode() == None);
    directory.setSearchQuery(R"(\^alpha)");
    TEST_CHECK(directory.searchMode() == Fuzzy);
    directory.setSearchQuery("");
    TEST_CHECK(directory.searchMode() == StartsWith);
  }

  void testFzf(const std::string& fzf) {
    const std::array<std::string_view, 12> names{
        "alpha",         "Alpha report",         "alphabet", "prefixalpha", "a-l-p-h-a",
        "two words.txt", "new\nline\textra.txt", "^alpha",   "dollar$",     "bad\xff-unique",
        "file|name",     "ordinary^name"
    };
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    const auto expect = [&](SearchMode mode, std::string_view query, std::vector<std::size_t> indices,
                            bool manual = false) {
      const auto result = filterWithFzf({fzf, mode, manual}, query, names, cancel);
      TEST_CHECK(result && *result == indices);
    };
    expect(Contiguous, "alpha", {0, 1, 2, 3, 7});
    expect(Fuzzy, "alpha", {0, 1, 2, 3, 4, 7});
    expect(StartsWith, "alpha", {0, 1, 2});
    expect(EndsWith, "alpha", {0, 3, 7});
    expect(Exclude, "alpha", {4, 5, 6, 8, 9, 10, 11});
    expect(Exact, "alpha", {0});
    expect(None, "^alpha$ | unique", {0, 9});
    expect(Contiguous, "two words", {5});
    expect(Contiguous, "new\nline", {6});
    expect(Contiguous, R"(\^alpha)", {7});
    expect(Exact, R"(dollar\$)", {8});
    expect(Contiguous, "file|name", {10});
    expect(StartsWith, "^alpha", {0, 1, 2}, true);
    expect(None, R"(\^alpha | ^alpha$)", {0, 7}, true);
    expect(None, R"(\^alpha | ordinary^name)", {7, 11}, true);
    expect(Contiguous, "absent", {});
  }

  void testBackendFailures(const fs::path& root, const fs::path& self, const std::string& fzf) {
    const auto normal = root / "needle.txt";
    write(normal);
    write(root / "other.txt");
    using Status = FileSearchSession::Status;
    constexpr FileSearchSession::Order order{FileDialogSortField::Name, FileDialogSortOrder::Ascending, true, true};
    FileSearchSession session;
    const auto begin = [&](std::string query, std::string backend) {
      session.edit(root, std::move(query), false, order, [](bool) {}, [] { return true; }, {std::move(backend)});
      session.submit();
    };
    for (const auto& executable : {root / "missing-fzf", self}) {
      begin("needle", executable.string());
      waitFor([&] { return session.status() != Status::Searching; });
      TEST_CHECK(session.usingFallback());
      TEST_CHECK(session.entries().size() == 1 && session.entries().front().absPath == normal);
    }
    begin("needle", "");
    waitFor([&] { return session.status() != Status::Searching; });
    TEST_CHECK(!session.usingFallback() && session.entries().size() == 1);

    const auto partial = root / "partial";
    TEST_CHECK(fs::create_directory(partial));
    write(partial / "onlyneedle");
    begin("onlyneedle", self.string());
    waitFor([&] { return session.status() != Status::Searching; });
    TEST_CHECK(session.usingFallback());
    TEST_CHECK(session.entries().size() == 1 && session.entries().front().absPath == partial / "onlyneedle");

    const auto startedMarker = fs::path(g_get_user_runtime_dir()) / "fzf-test-started";
    begin("hang", self.string());
    waitFor([&] { return fs::exists(startedMarker); });
    long childPid = 0;
    waitFor([&] { return static_cast<bool>(std::ifstream(startedMarker) >> childPid); });
    TEST_CHECK(childPid > 1);
    const auto replacedAt = std::chrono::steady_clock::now();
    begin("obsolete", self.string());
    begin("other", fzf);
    waitFor([&] { return session.status() != Status::Searching; });
    TEST_CHECK(std::chrono::steady_clock::now() - replacedAt < 1500ms);
    TEST_CHECK(!session.usingFallback());
    TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "other.txt");
    waitFor([&] { return !fs::exists(fs::path("/proc") / std::to_string(childPid)); });
  }

  void testProgressive(const fs::path& root, const std::string& fzf) {
    write(root / "match-first");
    TEST_CHECK(fs::create_directory(root / "nested"));
    for (int i = 0; i < 1100; ++i)
      write(root / "nested" / ("match-" + std::to_string(i)));
    write(root / "nested" / "match-with\nline\tand space");
    FileSearchSession session;
    std::size_t updates = 0;
    session.edit(
        root, "match", false, {FileDialogSortField::Name, FileDialogSortOrder::Ascending, true, true},
        [&](bool done) { updates += !done; }, [] { return true; }, {fzf}
    );
    session.submit();
    waitFor([&] { return session.status() == FileSearchSession::Status::Finished; });
    TEST_CHECK(!session.usingFallback() && updates >= 2 && session.entries().size() == 1102);
    TEST_CHECK(std::ranges::any_of(session.entries(), [&](const auto& item) {
      return item.absPath == root / "nested" / "match-with\nline\tand space" && item.hasIdentity;
    }));
  }
} // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--read0") {
    for (int i = 2; i < argc; ++i) {
      if (std::string_view(argv[i]) == "--filter='onlyneedle") {
        const auto marker = fs::path(g_get_user_runtime_dir()) / "fzf-test-partial";
        if (!fs::exists(marker)) {
          write(marker);
          // A valid ID with a misleading payload must never become the FileEntry path.
          constexpr char record[] = "0\t/fake/fzf/path\0";
          std::cout.write(record, sizeof(record) - 1);
          return 0;
        }
      }
      if (std::string_view(argv[i]) == "--filter='hang") {
        std::ofstream(fs::path(g_get_user_runtime_dir()) / "fzf-test-started") << getpid();
        std::this_thread::sleep_for(10s);
      }
    }
    return 2;
  }
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-files-fzf"});
    testRecognition(env.work());
    const auto fzf = findFzf();
    if (!fzf.empty()) {
      nythera::test::ScopedEnvVar options("FZF_DEFAULT_OPTS", "--print-query");
      testFzf(fzf);
      const auto progressive = env.work() / "progressive";
      TEST_CHECK(fs::create_directory(progressive));
      testProgressive(progressive, fzf);
    }
    testBackendFailures(env.work(), fs::canonical("/proc/self/exe"), fzf);
    const auto emptyPath = env.work() / "empty-path";
    TEST_CHECK(fs::create_directory(emptyPath));
    nythera::test::ScopedEnvVar path("PATH", emptyPath.string());
    TEST_CHECK(findFzf().empty());
  });
}
