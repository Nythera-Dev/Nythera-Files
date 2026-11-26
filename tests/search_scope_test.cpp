#include "filemanager/file_search_session.h"
#include "filemanager/search_scope.h"
#include "test_isolation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <semaphore>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace {
  using namespace filemanager;
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;

  constexpr FileSearchSession::Order kOrder{FileDialogSortField::Name, FileDialogSortOrder::Ascending, true, true};

  bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (done())
        return true;
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  bool containsRoot(const SearchPlan& plan, const fs::path& path) {
    return std::ranges::find(plan.roots, path, &SearchRoot::path) != plan.roots.end();
  }

  void mountResolution() {
    std::istringstream fixture(
        "12 1 8:1 / / rw shared:1 - ext4 /dev/sda rw\n"
        "13 12 8:2 / /home rw - ext4 /dev/sdb rw\n"
        "14 12 8:3 / /media/USB\\040Disk rw - exfat /dev/sdc rw\n"
        "15 12 0:21 / /net/nfs rw - nfs4 nas:/data rw\n"
        "16 12 0:22 / /net/smb rw - cifs //nas/data rw\n"
        "17 12 0:23 / /net/idle rw - autofs systemd-1 rw\n"
        "18 12 0:24 / /proc rw - proc proc rw\n"
        "19 12 0:25 / /tmp rw - tmpfs tmpfs rw\n"
        "20 12 8:2 /user /mnt/bound rw - ext4 /dev/sdb rw\n"
        "21 12 0:25 / /run/user/1000 rw - tmpfs tmpfs rw\n"
        "22 21 0:26 / /run/user/1000/gvfs rw - fuse.gvfsd-fuse gvfsd-fuse rw\n"
        "23 17 0:27 / /net/idle/already-mounted rw - nfs4 nas:/mounted rw\n"
    );
    const auto mounts = readSearchMounts(fixture);
    TEST_CHECK(mounts.size() == 12);
    TEST_CHECK(std::ranges::any_of(mounts, [](const auto& mount) {
      return mount.path == "/media/USB Disk" && mount.kind == SearchMount::Kind::Local;
    }));

    constexpr auto home = "/home/user";
    constexpr auto current = "/home/user/current";
    const auto resolve = [&](SearchScope scope, bool system = false, const CustomSearch* profile = nullptr) {
      return resolveSearchScope({scope, system}, current, home, profile, {"/home/user/global"}, mounts);
    };
    const auto direct = resolve(SearchScope::Folder);
    TEST_CHECK(!direct.recursive && direct.roots.size() == 1 && containsRoot(direct, current));
    const auto recursive = resolve(SearchScope::Recursive);
    TEST_CHECK(recursive.recursive && containsRoot(recursive, current));
    TEST_CHECK(containsRoot(resolve(SearchScope::Home), home));
    const auto local = resolve(SearchScope::Local);
    TEST_CHECK(containsRoot(local, home) && containsRoot(local, "/media/USB Disk"));
    TEST_CHECK(!containsRoot(local, "/") && !containsRoot(local, "/net/nfs"));
    const auto all = resolve(SearchScope::All, true);
    TEST_CHECK(containsRoot(all, "/") && containsRoot(all, "/net/nfs") && containsRoot(all, "/net/smb"));
    TEST_CHECK(containsRoot(all, "/run/user/1000/gvfs"));
    TEST_CHECK(containsRoot(all, "/net/idle/already-mounted"));
    TEST_CHECK(!containsRoot(all, "/net/idle") && !containsRoot(all, "/tmp"));
    TEST_CHECK(searchPathExcluded(all, "/proc/kcore") && searchPathExcluded(all, "/dev/shm"));
    TEST_CHECK(!searchPathExcluded(all, "/procurement"));
    TEST_CHECK(searchPathExcluded(all, "/mnt/bound/global") && !searchPathExcluded(all, "/mnt/bound/globally"));

    CustomSearch custom{"Root", searchLocationLines(" / \r\n /net/nfs\n"), {"/net/smb/private"}};
    TEST_CHECK(explicitlyIncludesRoot(custom.included));
    TEST_CHECK(!explicitlyIncludesRoot(searchLocationLines("/./\n//")));
    const auto withoutMounts = resolve(SearchScope::Custom, false, &custom);
    TEST_CHECK(
        withoutMounts.roots.size() == 2 && containsRoot(withoutMounts, "/") && containsRoot(withoutMounts, "/net/nfs")
    );
    TEST_CHECK(withoutMounts.boundaries.contains("/net/smb"));
    custom.includeMounts = true;
    const auto withMounts = resolve(SearchScope::Custom, false, &custom);
    TEST_CHECK(containsRoot(withMounts, "/net/smb") && containsRoot(withMounts, "/media/USB Disk"));
    TEST_CHECK(!containsRoot(withMounts, "/proc") && !containsRoot(withMounts, "/tmp"));
    TEST_CHECK(searchPathExcluded(withMounts, "/net/smb/private/child"));
    TEST_CHECK(searchPathExcluded(withMounts, "/home/user/global"));
  }

  std::unordered_set<std::string> results(const SearchPlan& plan, const std::string& fzf = {}, bool hidden = false) {
    FileSearchSession session;
    session.edit(
        plan.roots.front().path, "needle", hidden, kOrder, [](bool) {}, [] { return true; }, {fzf},
        [plan] { return plan; }
    );
    session.submit();
    TEST_CHECK(waitFor([&] { return session.status() != FileSearchSession::Status::Searching; }));
    TEST_CHECK(session.status() == FileSearchSession::Status::Finished && !session.usingFallback());
    std::unordered_set<std::string> paths;
    for (const auto& entry : session.entries())
      TEST_CHECK(paths.insert(entry.absPath.native()).second);
    return paths;
  }

  void scopedTraversal(const fs::path& root) {
    const auto mount = root / "nested-mount";
    const auto blocked = root / "excluded";
    const auto ordinary = root / "ordinary";
    for (const auto& folder : {mount, blocked, root / "excluded-sibling", ordinary, ordinary / "also-excluded"}) {
      fs::create_directories(folder);
      std::ofstream(folder / "needle") << "example";
    }
    std::ofstream(root / "needle-top") << "example";
    fs::create_directory_symlink(ordinary, root / "alias");
    fs::create_directory_symlink(blocked, root / "hidden-target");

    const std::vector<SearchMount> mounts{
        {root, "/", "8:1", SearchMount::Kind::Local}, {mount, "/", "0:2", SearchMount::Kind::Network}
    };
    CustomSearch custom{
        "Fixtures",
        {root, ordinary, root / "alias", mount, blocked, root / "hidden-target"},
        {ordinary / "also-excluded"}
    };
    for (const auto& backend : {std::string{}, findFzf()}) {
      const auto direct = resolveSearchScope({SearchScope::Folder}, root, root, nullptr, {}, mounts);
      TEST_CHECK(results(direct, backend) == std::unordered_set<std::string>{(root / "needle-top").native()});
      const auto plan = resolveSearchScope({SearchScope::Custom}, root, root, &custom, {blocked}, mounts);
      const auto found = results(plan, backend);
      TEST_CHECK(found.size() == 4);
      TEST_CHECK(found.contains((ordinary / "needle").native()));
      TEST_CHECK(found.contains((mount / "needle").native()));
      TEST_CHECK(found.contains((root / "excluded-sibling" / "needle").native()));
      TEST_CHECK(found.contains((root / "needle-top").native()));
      auto bounded = plan;
      std::erase_if(bounded.roots, [&](const auto& source) { return source.path == mount; });
      const auto withoutMount = results(bounded, backend);
      TEST_CHECK(withoutMount.size() == 3 && !withoutMount.contains((mount / "needle").native()));
    }
  }

  void mountedSubfoldersOfExplicitSymlinks(const fs::path& root) {
    const auto actual = root / "real-alias-target";
    const auto nested = actual / "nested-share";
    const auto link = root / "mounted-alias";
    fs::create_directories(nested);
    std::ofstream(actual / "needle-local") << "local";
    std::ofstream(nested / "needle-remote") << "remote";
    fs::create_directory_symlink(actual, link);
    const std::vector<SearchMount> mounts{
        {root, "/", "8:1", SearchMount::Kind::Local}, {nested, "/", "0:2", SearchMount::Kind::Network}
    };
    CustomSearch custom{"Alias", {link}};
    const auto plan = resolveSearchScope({SearchScope::Custom}, root, root, &custom, {}, mounts);
    const auto found = results(plan);
    TEST_CHECK(found.size() == 2);
    TEST_CHECK(found.contains((link / "needle-local").native()));
    TEST_CHECK(found.contains((link / "nested-share" / "needle-remote").native()));
  }

  void optionalDiscovery(const fs::path& fixture) {
    const auto root = fixture / "options";
    const auto local = root / "visible";
    const auto hidden = root / ".hidden-directory";
    const auto listed = root / "listed";
    const auto excluded = root / "excluded-options";
    const auto outside = fixture / "external";
    const auto mount = root / "network";
    for (const auto& path : {local, hidden, listed, excluded, outside, mount}) {
      fs::create_directories(path);
      std::ofstream(path / "needle-file") << "test";
    }
    std::ofstream(root / ".hidden") << "listed\n";
    fs::create_directory_symlink(local, root / "needle-alias");
    fs::create_directory_symlink(".", local / "needle-cycle");
    fs::create_directory_symlink("missing", root / "needle-broken");
    fs::create_directory_symlink("needle-loop-b", root / "needle-loop-a");
    fs::create_directory_symlink("needle-loop-a", root / "needle-loop-b");
    fs::create_directory_symlink(hidden, root / "hidden-alias");
    fs::create_directory_symlink("listed", root / "listed-alias");
    fs::create_directory_symlink(excluded, root / "excluded-alias");
    fs::create_directory_symlink(outside, root / "outside-alias");
    fs::create_directory_symlink("/proc", root / "needle-proc");
    fs::create_directory_symlink(mount, root / "mounted-alias");

    // A reachable in-scope target beyond the ordinary depth budget demonstrates
    // that the link really was traversed, with one canonical result.
    auto deep = local;
    for (int i = 0; i < 260; ++i)
      deep /= "d";
    fs::create_directories(deep);
    std::ofstream(deep / "needle-deep") << "test";
    fs::create_directory_symlink(deep, root / "shortcut-absolute");
    fs::create_directory_symlink(deep.lexically_relative(root), root / "shortcut-relative");

    const std::vector<SearchMount> mounts{
        {root, "/", "8:1", SearchMount::Kind::Local}, {mount, "/", "0:2", SearchMount::Kind::Network}
    };
    const auto disabled = resolveSearchScope({SearchScope::Local}, root, root, nullptr, {excluded}, mounts);
    const auto enabled = resolveSearchScope({SearchScope::Local}, root, root, nullptr, {excluded}, mounts, true);
    const auto hiddenResults = results(enabled, {}, true);
    for (const auto& backend : {std::string{}, findFzf()}) {
      const auto before = results(disabled, backend);
      TEST_CHECK(!before.contains((deep / "needle-deep").native()));
      TEST_CHECK(
          before.contains((root / "needle-alias").native()) && before.contains((root / "needle-broken").native())
      );
      auto expected = before;
      expected.insert((deep / "needle-deep").native());
      const auto after = results(enabled, backend);
      TEST_CHECK(after == expected);
      TEST_CHECK(after.contains((root / "needle-proc").native()));
      TEST_CHECK(
          !after.contains((hidden / "needle-file").native()) && !after.contains((listed / "needle-file").native())
      );
      expected.insert((hidden / "needle-file").native());
      expected.insert((listed / "needle-file").native());
      TEST_CHECK(results(enabled, backend, true) == expected && hiddenResults == expected);
      TEST_CHECK(
          !after.contains((outside / "needle-file").native()) && !after.contains((mount / "needle-file").native())
      );
      TEST_CHECK(!after.contains((excluded / "needle-file").native()));
      const auto direct = resolveSearchScope({SearchScope::Folder}, root, root, nullptr, {}, mounts, true);
      TEST_CHECK(!results(direct, backend).contains((local / "needle-file").native()));
    }
  }

  void concurrencyAndCancellation(const fs::path& fixture) {
    auto plan = std::make_shared<SearchPlan>();
    for (int i = 0; i < 5; ++i) {
      auto path = fixture / std::to_string(i);
      TEST_CHECK(fs::create_directory(path));
      std::ofstream(path / "candidate") << "example";
      plan->roots.push_back({path, i != 4, std::to_string(i)});
    }

    struct State {
      std::counting_semaphore<4> release{0};
      std::atomic<int> networkStarted{0};
      std::atomic<int> networkStopped{0};
      bool localPublished = false;
      bool stalePublished = false;
      bool completed = false;
    };
    auto state = std::make_shared<State>();
    const std::weak_ptr<State> outstanding = state;
    RecursiveFileDiscovery discovery;
    discovery.startPlan(
        plan, false,
        [state] {
          return RecursiveFileDiscovery::Consumer{
              [state](
                  const fs::path& parent, auto, const auto&, GCancellable* token
              ) -> RecursiveFileDiscovery::Publish {
                if (parent.filename() == "4")
                  return [state] { state->localPublished = true; };
                ++state->networkStarted;
                state->release.try_acquire_for(5s);
                if (g_cancellable_is_cancelled(token))
                  ++state->networkStopped;
                return [state] { state->stalePublished = true; };
              }
          };
        },
        [state](auto, auto) { state->completed = true; }
    );
    const bool progressive = waitFor([&] { return state->localPublished && state->networkStarted == 2; });
    discovery.cancel();
    bool replacementCompleted = false;
    auto replacement = std::make_shared<SearchPlan>();
    replacement->roots.push_back(plan->roots.back());
    discovery.startPlan(
        replacement, false,
        [] {
          return RecursiveFileDiscovery::Consumer{
              [](const auto&, auto, const auto&, GCancellable*) -> RecursiveFileDiscovery::Publish { return {}; }
          };
        },
        [&](auto, auto) { replacementCompleted = true; }
    );
    const bool replacementPassed = waitFor([&] { return replacementCompleted; });
    state->release.release(4);
    const bool drained = waitFor([&] { return outstanding.use_count() == 1; });
    TEST_CHECK(progressive && replacementPassed && drained);
    TEST_CHECK(state->networkStarted == 2 && state->networkStopped == 2);
    TEST_CHECK(!state->stalePublished && !state->completed);
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    const nythera::test::ScopedTestEnv env({.prefix = "files-search-scope", .setXdgDirs = true});
    mountResolution();
    scopedTraversal(env.work());
    mountedSubfoldersOfExplicitSymlinks(env.work());
    optionalDiscovery(env.work());
    concurrencyAndCancellation(env.work());
  });
}
