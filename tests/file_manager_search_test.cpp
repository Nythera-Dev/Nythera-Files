#include "config/application_paths.h"
#include "core/input/key_modifiers.h"
#include "filemanager/file_manager.h"
#include "render/core/texture_manager.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <gio/gdesktopappinfo.h>
#include <memory>
#include <thread>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using Status = filemanager::FileSearchSession::Status;

  template <typename Predicate> void waitFor(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      if (!g_main_context_iteration(nullptr, false))
        std::this_thread::sleep_for(1ms);
    }
    TEST_CHECK(ready());
  }

  void writeFile(const fs::path& path, std::string_view text = {}) {
    TEST_CHECK(static_cast<bool>(std::ofstream(path) << text));
  }

  void installFileHandler(const nythera::test::ScopedTestEnv& env) {
    const auto script = env.work() / "search-handler.sh";
    writeFile(script, R"(printf '%s' "$1" > "$1.opened"
)");
    const auto apps = env.data() / "applications";
    fs::create_directory(apps);
    const auto desktopFile = apps / "nythera-search-test.desktop";
    writeFile(
        desktopFile,
        "[Desktop Entry]\nType=Application\nName=Isolated search test handler\n"
        "Exec=/bin/sh "
            + script.string()
            + " %f\nMimeType=text/plain;application/octet-stream;\n"
    );
    g_autoptr(GDesktopAppInfo) handler = g_desktop_app_info_new_from_filename(desktopFile.c_str());
    TEST_CHECK(handler);
    TEST_CHECK(g_app_info_set_as_default_for_type(G_APP_INFO(handler), "text/plain", nullptr));
    TEST_CHECK(g_app_info_set_as_default_for_type(G_APP_INFO(handler), "application/octet-stream", nullptr));
  }

  class ThumbnailTextures final : public TextureManager {
  public:
    TextureHandle loadFromRgba(const std::uint8_t*, int width, int height, bool) override {
      ++uploads;
      return {.id = TextureId{1}, .width = width, .height = height};
    }
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override {
      return {};
    }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    void unload(TextureHandle& handle) override {
      ++unloads;
      handle = {};
    }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}

    int uploads = 0;
    int unloads = 0;
  };

  void testSearchThumbnails(const fs::path& root) {
    using filemanager::SearchThumbnails;
    g_autoptr(GFileInfo) filesystem = g_file_info_new();
    TEST_CHECK(!SearchThumbnails::permitsAutomaticPreview(filesystem));
    g_file_info_set_attribute_boolean(filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, true);
    TEST_CHECK(!SearchThumbnails::permitsAutomaticPreview(filesystem));
    g_file_info_set_attribute_boolean(filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, false);
    TEST_CHECK(SearchThumbnails::permitsAutomaticPreview(filesystem));
    g_file_info_set_attribute_uint32(
        filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_NEVER
    );
    TEST_CHECK(!SearchThumbnails::permitsAutomaticPreview(filesystem));

    constexpr auto svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect width="10" height="10" fill="red"/></svg>)";
    const std::array old{(root / "obsolete.svg").string()};
    const std::array next{(root / "visible-without-extension").string()};
    writeFile(old.front(), svg);
    writeFile(next.front(), svg);
    FileEntry entry{.absPath = next.front(), .size = std::string_view(svg).size(), .type = "image/svg+xml"};
    TEST_CHECK(SearchThumbnails::eligible(entry));
    entry.isSymlink = true;
    TEST_CHECK(!SearchThumbnails::eligible(entry));
    entry.isSymlink = false;
    entry.type = "application/octet-stream";
    TEST_CHECK(!SearchThumbnails::eligible(entry));

    ThumbnailTextures textures;
    ThumbnailService service;
    int updates = 0;
    SearchThumbnails previews(service, [&] { ++updates; });
    previews.updateVisible(old, 64);
    previews.clear();
    waitFor([&] {
      previews.updateVisible(next, 64);
      (void)service.uploadPending(textures);
      return previews.peek(next.front()).id != 0;
    });
    TEST_CHECK(updates > 0);
    TEST_CHECK(previews.peek(old.front()).id == 0 && textures.uploads == 1);
    previews.clear();
    TEST_CHECK(previews.peek(next.front()).id == 0 && textures.unloads == 0);
    previews.releaseRetired();
    TEST_CHECK(textures.unloads == 1);
  }

  void testDiscoveryCancellation(const fs::path& root) {
    filemanager::RecursiveFileDiscovery discovery;
    const auto token = std::make_shared<int>(0);
    const std::weak_ptr<int> outstanding = token;
    bool called = false;
    discovery.start(
        root, false,
        [token, &called](const auto&, auto, GCancellable*) -> filemanager::RecursiveFileDiscovery::Publish {
          return [&called] { called = true; };
        },
        [&called](std::string, std::size_t) { called = true; }
    );
    // Cancel while the first GIO request is in flight and wait for its owner to drain.
    discovery.cancel();
    auto workDrained = [&] { return outstanding.use_count() == 1; };
    waitFor(workDrained);
    TEST_CHECK(!called);
  }
} // namespace

class FileManagerTestAccess {
  static void key(FileManager& manager, std::uint32_t sym, std::uint32_t mods = 0) {
    manager.handleWindowKeyEvent({.sym = sym, .modifiers = mods, .pressed = true});
  }

  static void query(FileManager& manager, const std::string& text) {
    auto& input = *manager.m_searchInput;
    input.selectAll();
    if (text.empty())
      key(manager, XKB_KEY_BackSpace);
    else
      input.textInputApplyEdit({.commitText = text, .hasCommitText = true});
  }

  static void finish(FileManager& manager) {
    waitFor([&] { return manager.activeSession().search().status() != Status::Searching; });
  }

  static const FileEntry& find(const filemanager::DirectorySession& session, const fs::path& path) {
    const auto result = std::ranges::find(session.entries(), path, &FileEntry::absPath);
    TEST_CHECK(result != session.entries().end());
    return *result;
  }

  static void testMatchesAndActions(FileManager& manager, const fs::path& root, const fs::path& outside) {
    auto& session = manager.activeSession();
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    TEST_CHECK(session.searchOpen() && session.search().status() == Status::Idle);
    TEST_CHECK(session.entries().size() == 5);

    query(manager, "nEeDlE");
    TEST_CHECK(manager.m_searchStatus->visible() && manager.m_searchStatus->text() == "Searching…");
    finish(manager);
    TEST_CHECK(session.search().status() == Status::Finished);
    TEST_CHECK(session.entries().size() == 5);
    TEST_CHECK(find(session, root / "Needle Folder").isDir);
    TEST_CHECK(find(session, root / "Needle Link").isSymlink && find(session, root / "Needle Link").isDir);
    TEST_CHECK(find(session, root / "Needle Folder" / "Deep" / "another NEEDLE.txt").name == "another NEEDLE.txt");
    TEST_CHECK(
        find(session, root / "Needle Folder" / "needle.txt").absPath != find(session, root / "needle.txt").absPath
    );
    TEST_CHECK(session.search().skippedFolders() == (getuid() == 0 ? 0U : 1U));
    TEST_CHECK(manager.m_searchStatus->text().starts_with("5 found"));

    const auto folder = root / "Needle Folder";
    manager.activeTab().selectedPaths = {folder.string(), (folder / "needle.txt").string()};
    TEST_CHECK(manager.selectedPaths().size() == 2);
    TEST_CHECK(manager.selectedOperationPaths() == std::vector<std::string>{folder.string()});

    query(manager, "needle folder");
    key(manager, XKB_KEY_Return);
    finish(manager);
    TEST_CHECK(session.entries().size() == 1);
    manager.activatePath(session.entries().front().absPath);
    waitFor([&] { return !session.loading(); });
    TEST_CHECK(session.path() == root / "Needle Folder" && !session.searchOpen());

    manager.navigate(root);
    waitFor([&] { return !session.loading(); });
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    query(manager, "other needle");
    finish(manager);
    const auto target = find(session, root / "Needle Folder" / "Deep" / "another NEEDLE.txt").absPath;
    manager.activeTab().selectedPaths.insert(target.string());
    manager.activeTab().selectedPath = target;
    TEST_CHECK(manager.activeTab().selectedPathsOrdered() == std::vector<std::string>{target.string()});
    g_autoptr(GFile) file = g_file_new_for_path(target.c_str());
    g_autoptr(GAppInfo) handler = g_file_query_default_handler(file, nullptr, nullptr);
    TEST_CHECK(handler && std::string_view(g_app_info_get_id(handler)) == "nythera-search-test.desktop");
    manager.activatePath(target);
    const auto opened = fs::path(target.string() + ".opened");
    waitFor([&] {
      std::error_code error;
      return fs::file_size(opened, error) == target.string().size() && !error;
    });
    std::ifstream openedPath(opened);
    std::string contents;
    std::getline(openedPath, contents);
    TEST_CHECK(contents == target.string());
    TEST_CHECK(session.path() == root && session.searchOpen());
    manager.m_menus.setContextMenuTarget(target);
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kOpenContainingFolder});
    waitFor([&] { return !session.loading(); });
    TEST_CHECK(session.path() == target.parent_path() && !session.searchOpen());

    manager.navigate(root);
    waitFor([&] { return !session.loading(); });
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    query(manager, "unused needle from linked folder");
    finish(manager);
    TEST_CHECK(session.entries().empty());
    TEST_CHECK(manager.m_searchStatus->text().starts_with("No matches"));
    query(manager, "cafe\u0301");
    finish(manager);
    TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "CAFÉ.txt");
    query(manager, "");
    TEST_CHECK(session.searchOpen() && !session.search().active() && session.entries().size() == 5);
    TEST_CHECK(!manager.m_searchStatus->visible());
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(!session.searchOpen() && !manager.m_searchBar->visible());

    manager.navigate(outside / "missing");
    waitFor([&] { return !session.loading(); });
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    query(manager, "needle");
    finish(manager);
    TEST_CHECK(session.search().status() == Status::Failed);
    TEST_CHECK(manager.m_searchStatus->text().starts_with("Search failed: "));
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(!session.searchOpen() && session.entries().empty());
  }

  static void testMetadataAndHiddenEntries(FileManager& manager, const fs::path& root) {
    // Keep desktop handlers and user files isolated while using the system MIME rules for sniffing.
    g_auto(GStrv) savedMimeDirs = g_strdupv(const_cast<char**>(g_content_type_get_mime_dirs()));
    const char* mimeDirs[] = {"/usr/share/mime", nullptr};
    g_content_type_set_mime_dirs(mimeDirs);
    auto restoreMimeDirs = nythera::test::makeScopeExit([&] { g_content_type_set_mime_dirs(savedMimeDirs); });

    const auto folder = root / "metadata";
    TEST_CHECK(fs::create_directory(folder));
    const auto hidden = folder / "metadata-hidden";
    const auto listed = folder / "metadata.gif";
    const auto extensionless = folder / "metadata-unknown";
    const auto broken = folder / "metadata-broken";
    writeFile(folder / ".hidden", "metadata-hidden\nmetadata-hidden-folder\n");
    writeFile(hidden, "hidden");
    TEST_CHECK(fs::create_directory(folder / "metadata-hidden-folder"));
    writeFile(folder / "metadata-hidden-folder" / "metadata-child", "hidden by its parent");
    writeFile(listed, "GIF89a long known name");
    writeFile(extensionless, "GIF89a");
    fs::create_symlink("missing-target", broken);

    auto& session = manager.activeSession();
    manager.navigate(folder);
    waitFor([&] { return !session.loading(); });
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    query(manager, "metadata");
    finish(manager);
    TEST_CHECK(session.entries().size() == 3);

    const auto& file = find(session, extensionless);
    TEST_CHECK(file.hasIdentity && file.size == 6 && !file.isSymlink);
    struct stat identity{};
    TEST_CHECK(lstat(extensionless.c_str(), &identity) == 0);
    TEST_CHECK(file.device == identity.st_dev && file.inode == identity.st_ino);
    TEST_CHECK(file.type != "image/gif");
    TEST_CHECK(find(session, listed).type == "image/gif");
    TEST_CHECK(find(session, broken).isSymlink && !find(session, broken).hasIdentity);

    g_autoptr(GFile) handle = g_file_new_for_path(extensionless.c_str());
    g_autoptr(GFileInfo) full =
        g_file_query_info(handle, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    TEST_CHECK(full && std::string_view(g_file_info_get_content_type(full)) == "image/gif");

    manager.activeTab().searchViewState.sortField = FileDialogSortField::Size;
    manager.activeTab().searchViewState.sortOrder = FileDialogSortOrder::Descending;
    session.sortEntries();
    waitFor([&] { return session.entries().front().absPath == listed; });
    session.setShowHidden(true);
    session.setSearchQuery("metadata");
    finish(manager);
    TEST_CHECK(session.entries().size() == 6);
    TEST_CHECK(find(session, hidden).absPath == hidden);
    TEST_CHECK(find(session, folder / "metadata-hidden-folder" / "metadata-child").size > 0);
    session.setShowHidden(false);
    manager.activeTab().searchViewState.sortField = FileDialogSortField::Name;
    manager.activeTab().searchViewState.sortOrder = FileDialogSortOrder::Ascending;
    key(manager, XKB_KEY_Escape);
  }

  static void testSearchView(FileManager& manager, const fs::path& root) {
    auto& session = manager.activeSession();
    manager.navigate(root);
    waitFor([&] { return !session.loading(); });
    auto& header = manager.m_listHeader;
    using namespace filemanager;
    manager.setGridMode(true);
    manager.setSortField(FileDialogSortField::Modified);
    TEST_CHECK(!header.searchView() && header.gridMode());
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    TEST_CHECK(header.searchView() && !header.gridMode());
    const std::vector defaults{
        FileColumn::Name, kSearchLocationSlot, FileColumn::Size, FileColumn::Modified, FileColumn::Type
    };
    TEST_CHECK(header.columnLayout().displayed() == defaults);
    TEST_CHECK(header.columnLabel(kSearchLocationSlot) == "Location");
    TEST_CHECK(header.columnSortField(kSearchLocationSlot) == FileDialogSortField::Location);
    TEST_CHECK(!manager.m_searchStatus->visible());
    TEST_CHECK(manager.m_searchStatus->parent() == manager.m_searchInput->inputArea());

    query(manager, "needle");
    finish(manager);
    manager.activateContextMenuEntry({.id = 63});
    TEST_CHECK(header.sortField() == FileDialogSortField::Location);
    waitFor([&] {
      const auto firstFile = std::ranges::find_if(session.entries(), [](const auto& entry) { return !entry.isDir; });
      return firstFile != session.entries().end() && firstFile->absPath == root / "needle.txt";
    });
    manager.toggleSortOrder();
    waitFor([&] { return session.entries().back().absPath == root / "needle.txt"; });

    const auto folder = root / "Needle Folder";
    const auto otherFolder = root / "Needle Link";
    const auto visibleFolder = std::ranges::find(session.entries(), folder, &FileEntry::absPath);
    TEST_CHECK(visibleFolder != session.entries().end());
    const auto folderIndex = static_cast<std::size_t>(visibleFolder - session.entries().begin());
    const auto other = std::ranges::find(session.entries(), otherFolder, &FileEntry::absPath);
    TEST_CHECK(other != session.entries().end());
    session.updateVisibleSearchFolders(
        {{otherFolder.string(), static_cast<std::size_t>(other - session.entries().begin())}}
    );
    session.updateVisibleSearchFolders({{folder.string(), folderIndex}});
    waitFor([&] { return session.folderItemCount(*visibleFolder).has_value(); });
    TEST_CHECK(session.folderItemCount(*visibleFolder) == 2);
    TEST_CHECK(!session.folderItemCount(*other));
    session.updateVisibleSearchFolders({});
    TEST_CHECK(session.folderItemCount(*visibleFolder) == 2);

    TEST_CHECK(header.columnLayout().move(kSearchLocationSlot, 3));
    manager.activateContextMenuEntry(
        {.id = filemanager::menu_id::kFirstColumnAction + static_cast<int>(columnIndex(FileColumn::Type))}
    );
    const auto expectedLayout = header.columnLayout().displayed();
    FileColumns widths;
    widths[kSearchLocationSlot] = 234.0F;
    TEST_CHECK(manager.m_settings.saveListColumnWidths(widths, true));
    manager.setGridMode(true);
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(!header.searchView() && header.gridMode());
    TEST_CHECK(header.sortField() == FileDialogSortField::Modified);
    TEST_CHECK(header.columnLabel(FileColumn::Created) == "Created");
    manager.setGridMode(false);
    manager.setSortField(FileDialogSortField::Name);

    // Reload from disk, then reopen to cover persistence and browse/search independence.
    TEST_CHECK(manager.m_settings.reload());
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    TEST_CHECK(header.searchView() && header.gridMode());
    TEST_CHECK(header.sortField() == FileDialogSortField::Location);
    TEST_CHECK(header.sortOrder() == FileDialogSortOrder::Descending);
    TEST_CHECK(header.columnLayout().displayed() == expectedLayout);
    FileColumns loaded;
    manager.m_settings.loadListColumnWidths(loaded, true);
    TEST_CHECK(loaded[kSearchLocationSlot] == 234.0F);
    FileColumns browsingWidths;
    manager.m_settings.loadListColumnWidths(browsingWidths);
    TEST_CHECK(browsingWidths[kSearchLocationSlot] != 234.0F);
    TEST_CHECK(manager.m_settings.loadFileView().sortField == FileDialogSortField::Name);
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(!header.searchView() && !header.gridMode());
  }

  static void testFzfModeUi(FileManager& manager) {
    const auto executable = filemanager::findFzf();
    if (executable.empty())
      return;

    using enum filemanager::SearchMode;
    auto& session = manager.activeSession();
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    TEST_CHECK(!manager.m_searchModeButton->visible());
    session.configureSearchBackend(executable, StartsWith);
    manager.syncSearchUi();
    TEST_CHECK(manager.m_searchModeButton->visible() && session.searchMode() == StartsWith);
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kFirstSearchMode + static_cast<int>(Fuzzy)});
    query(manager, "^metadata$");
    TEST_CHECK(session.searchMode() == Exact);
    TEST_CHECK(manager.m_searchInput->value() == "^metadata$");
    query(manager, "metadata");
    TEST_CHECK(session.searchMode() == Fuzzy && manager.m_searchInput->value() == "metadata");
    query(manager, "");
    TEST_CHECK(session.searchMode() == StartsWith);
    key(manager, XKB_KEY_Escape);
  }

  static void testProgressAndCancellation(FileManager& manager, const fs::path& bulk) {
    manager.navigate(bulk);
    auto& session = manager.activeSession();
    waitFor([&] { return !session.loading(); });
    key(manager, XKB_KEY_f, KeyMod::Ctrl);

    int heartbeats = 0;
    const auto timer = g_timeout_add(
        1,
        [](gpointer data) -> gboolean {
          ++*static_cast<int*>(data);
          return G_SOURCE_CONTINUE;
        },
        &heartbeats
    );
    auto removeTimer = nythera::test::makeScopeExit([&] { g_source_remove(timer); });

    query(manager, "fixture");
    session.submitSearch();
    waitFor([&] { return !session.entries().empty(); });
    TEST_CHECK(session.search().status() == Status::Searching);
    query(manager, "fixture-11");
    TEST_CHECK(session.entries().empty());
    query(manager, "replacement");
    finish(manager);
    TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "Replacement.txt");

    query(manager, "fixture");
    const int beatsBefore = heartbeats;
    session.submitSearch();
    finish(manager);
    TEST_CHECK(session.entries().size() == 12000);
    if (const char* locale = std::getenv("LC_ALL"); locale && std::string_view(locale) == "en_GB.utf8")
      TEST_CHECK(manager.m_searchStatus->text() == "12,000 found");
    TEST_CHECK(heartbeats - beatsBefore > 5);
    TEST_CHECK(session.entries().front().name == "fixture-0");
    TEST_CHECK(session.entries().back().name == "fixture-11999");

    query(manager, "fixture");
    session.submitSearch();
    waitFor([&] { return !session.entries().empty(); });
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(!session.searchOpen() && !session.search().active());
    TEST_CHECK(session.entries().size() == 12);
    // A replacement search following Escape must publish only its own results.
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    query(manager, "replacement");
    finish(manager);
    TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "Replacement.txt");
    key(manager, XKB_KEY_Escape);
  }

public:
  static void run(const fs::path& work) {
    const auto root = work / "browse";
    const auto outside = work / "outside";
    const auto bulk = work / "bulk";
    fs::create_directories(root / "Needle Folder" / "Deep");
    fs::create_directory(root / "locked");
    fs::create_directory(outside);
    fs::create_directory(bulk);
    writeFile(root / "needle.txt");
    writeFile(root / "CAFÉ.txt");
    writeFile(root / ".needle-hidden");
    writeFile(root / "Needle Folder" / "needle.txt");
    writeFile(root / "Needle Folder" / "Deep" / "another NEEDLE.txt", "search test file\n");
    writeFile(root / "Needle Folder" / "Deep" / "N_e_e_d_l_e.txt");
    writeFile(root / "locked" / "secret.txt");
    writeFile(outside / "unused needle from linked folder");
    fs::create_directory_symlink(root, root / "Needle Folder" / "Deep" / "loop");
    fs::create_directory_symlink(outside, root / "Needle Link");
    fs::permissions(root / "locked", fs::perms::none);
    auto unlock = nythera::test::makeScopeExit([&] {
      std::error_code error;
      fs::permissions(root / "locked", fs::perms::owner_all, error);
    });

    for (int folder = 0; folder < 12; ++folder) {
      const auto directory = bulk / std::to_string(folder);
      fs::create_directory(directory);
      for (int entry = 0; entry < 1000; ++entry)
        writeFile(directory / ("fixture-" + std::to_string(1000 * folder + entry)));
    }
    writeFile(bulk / "0" / "Replacement.txt");
    testDiscoveryCancellation(root);

    Flex scene;
    Button searchButton;
    FileManager manager(root.string());
    manager.m_searchButton = &searchButton;
    manager.buildSearchBar(scene);
    scene.addChild(manager.m_listHeader.buildHeader());
    manager.m_dispatcher.setSceneRoot(&scene);
    auto detach = nythera::test::makeScopeExit([&] { manager.m_dispatcher.setSceneRoot(nullptr); });
    manager.enumerate(root);
    waitFor([&] { return !manager.activeSession().loading(); });
    testMatchesAndActions(manager, root, outside);
    testProgressAndCancellation(manager, bulk);
    testMetadataAndHiddenEntries(manager, root);
    testFzfModeUi(manager);
    testSearchView(manager, root);
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-files-search", .setXdgDirs = true});
    const auto config = fs::path(FileUtils::applicationConfigDir(FileUtils::Application::Files));
    TEST_CHECK(nythera::test::isPathWithinRoot(config, env.config()));
    fs::create_directories(config);
    writeFile(config / "config.toml", "[files]\nfzf_search = false\n");
    installFileHandler(env);
    testSearchThumbnails(env.work());
    FileManagerTestAccess::run(env.work());
  });
}
