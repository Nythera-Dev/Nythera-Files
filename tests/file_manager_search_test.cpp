#include "config/application_paths.h"
#include "core/input/key_modifiers.h"
#include "core/ui_phase.h"
#include "filemanager/custom_search_editor_state.h"
#include "filemanager/file_list_columns.h"
#include "filemanager/file_manager.h"
#include "filemanager/file_sort.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/search_dropdown.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/controls/spinner.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_entry_row.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <gio/gdesktopappinfo.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <ranges>
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

    const auto videoPlayer = apps / "nythera-video-test.desktop";
    writeFile(
        videoPlayer,
        "[Desktop Entry]\nType=Application\nName=Isolated video player\nExec=/bin/sh "
            + script.string()
            + " %f\nMimeType=video/mp4;\n"
    );
    const auto alternate = apps / "nythera-video-alternate.desktop";
    writeFile(
        alternate,
        "[Desktop Entry]\nType=Application\nName=Alternate video player\nExec=/bin/true %f\n"
        "MimeType=video/mp4;\n"
    );
    g_autoptr(GDesktopAppInfo) videoHandler = g_desktop_app_info_new_from_filename(videoPlayer.c_str());
    g_autoptr(GDesktopAppInfo) alternateHandler = g_desktop_app_info_new_from_filename(alternate.c_str());
    TEST_CHECK(videoHandler && alternateHandler);
    TEST_CHECK(g_app_info_set_as_default_for_type(G_APP_INFO(videoHandler), "video/mp4", nullptr));
    TEST_CHECK(g_app_info_add_supports_type(G_APP_INFO(alternateHandler), "video/mp4", nullptr));
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

  class TestRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float size, const TextFont&, float maxWidth, int, TextAlign, TextEllipsize, bool
    ) override {
      const auto natural = static_cast<float>(text.size()) * size * 0.5F;
      const auto width = maxWidth > 0.0F ? std::min(maxWidth, natural) : natural;
      return {.width = width, .right = width, .bottom = size, .lineCount = 1};
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.bottom = size}; }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .right = size, .bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { return m_textures; }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    ThumbnailTextures m_textures;
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
    entry.type = "video/mp4";
    entry.size = ThumbnailService::kMaxSourceFileBytes + 1;
    TEST_CHECK(SearchThumbnails::eligible(entry));
    entry.type = "application/pdf";
    TEST_CHECK(!SearchThumbnails::eligible(entry));
    entry.size = 1000;
    TEST_CHECK(SearchThumbnails::eligible(entry));
    entry.absPath = "archive:/preview.pdf";
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
        [token, &called](const auto&, auto, const auto&, GCancellable*)
            -> filemanager::RecursiveFileDiscovery::Publish { return [&called] { called = true; }; },
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
    TEST_CHECK(manager.m_searchStatus->visible() && manager.m_searchStatus->text() == "Searching… 0 found");
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
    TEST_CHECK(!manager.m_searchSpinner->spinning());
    TEST_CHECK(manager.m_statusLabel->text() == "2 folders");
    TEST_CHECK(manager.m_statusDetailLabel->text().starts_with("  |  3 files"));
    if (getuid() != 0) {
      TEST_CHECK(manager.m_searchStatus->text() == "Partial results · 5 found");
      const auto rows = std::get<std::vector<TooltipRow>>(manager.m_searchStatus->tooltipContent());
      TEST_CHECK(std::ranges::any_of(rows, [](const TooltipRow& row) {
        return row.key == "Folders skipped" && row.value == "1";
      }));
    } else {
      TEST_CHECK(manager.m_searchStatus->text() == "5 found");
    }

    const auto folder = root / "Needle Folder";
    manager.activeTab().selectedPaths = {folder.string(), (folder / "needle.txt").string()};
    manager.updateSelectionSummary();
    TEST_CHECK(manager.m_statusLabel->text() == "2 selected");
    TEST_CHECK(manager.m_searchStatus->text().ends_with("5 found"));
    manager.activeTab().selectedPaths.clear();
    manager.updateSelectionSummary();
    TEST_CHECK(manager.m_statusLabel->text() == "2 folders");
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
    TEST_CHECK(manager.m_searchStatus->text().ends_with("No matches"));
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
    TEST_CHECK(manager.m_searchStatus->text() == "Search failed");
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
    manager.toggleSearchOption(&filemanager::SearchOptions::showHidden);
    TEST_CHECK(!session.showHidden() && session.searchOptions().showHidden);
    finish(manager);
    TEST_CHECK(session.entries().size() == 6);
    TEST_CHECK(find(session, hidden).absPath == hidden);
    TEST_CHECK(find(session, folder / "metadata-hidden-folder" / "metadata-child").size > 0);
    manager.toggleSearchOption(&filemanager::SearchOptions::showHidden);
    finish(manager);
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
    TEST_CHECK(!header.searchView() && header.gridMode());
    TEST_CHECK(manager.m_searchBar->visible() && !manager.m_searchStatus->visible());
    query(manager, "needle");
    TEST_CHECK(header.searchView() && !header.gridMode());
    const std::vector defaults{
        FileColumn::Name, kSearchLocationSlot, FileColumn::Size, FileColumn::Modified, FileColumn::Type
    };
    TEST_CHECK(header.columnLayout().displayed() == defaults);
    TEST_CHECK(header.columnLabel(kSearchLocationSlot) == "Location");
    TEST_CHECK(header.columnSortField(kSearchLocationSlot) == FileDialogSortField::Location);
    TEST_CHECK(manager.m_searchStatus->parent() == manager.m_searchStatusArea);

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
    TEST_CHECK(!header.searchView() && !header.gridMode());
    query(manager, "needle");
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
    manager.closeSearch();
    TEST_CHECK(!header.searchView() && !header.gridMode());
  }

  static void testScopeUi(FileManager& manager, const fs::path& root) {
    writeFile(root / "Needle Folder" / "Deep" / "scoped fixture.txt");
    using namespace filemanager;
    using namespace filemanager::menu_id;
    manager.navigate(root);
    waitFor([&] { return !manager.activeSession().loading(); });
    auto& session = manager.activeSession();
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    TEST_CHECK(manager.m_searchScopeButton->visible());
    TEST_CHECK(session.searchSelection() == SearchSelection{});
    manager.updateSearchScopeWidth(340);
    TEST_CHECK(manager.m_searchScopeButton->label()->text() == "Scope");
    manager.updateSearchScopeWidth(580);
    TEST_CHECK(manager.m_searchScopeButton->label()->text() == "Current folder & subfolders");
    manager.activateContextMenuEntry({.id = kFirstSearchScope + static_cast<int>(SearchScope::Local)});
    TEST_CHECK(!session.searchSelection().includeSystem);
    manager.activateContextMenuEntry({.id = kSearchIncludeSystem});
    TEST_CHECK(session.searchSelection().includeSystem);
    manager.activateContextMenuEntry({.id = kFirstSearchScope + static_cast<int>(SearchScope::Recursive)});
    TEST_CHECK(!session.searchSelection().includeSystem);
    query(manager, "scoped fixture");
    manager.activateContextMenuEntry({.id = kFirstSearchScope + static_cast<int>(SearchScope::Folder)});
    session.submitSearch();
    finish(manager);
    TEST_CHECK(session.entries().empty());

    manager.activateContextMenuEntry({.id = kFirstSearchScope + static_cast<int>(SearchScope::Recursive)});
    session.submitSearch();
    finish(manager);
    TEST_CHECK(session.entries().size() == 1);
    TEST_CHECK(
        manager.fileColumnText(session.entries().front(), kSearchLocationSlot)
        == (root / "Needle Folder" / "Deep").native()
    );
    TEST_CHECK(manager.fileColumnText(session.entries().front(), FileColumn::Name) == "scoped fixture.txt");
    TEST_CHECK(searchLocationText("/etc/test.conf") == "/etc");
    TEST_CHECK(searchLocationText("/mnt/Goku/SteamLibrary/steamapps/test") == "/mnt/Goku/SteamLibrary/steamapps");
    TEST_CHECK(manager.m_searchInput->value() == "scoped fixture");

    const CustomSearch profile{"Needles", {root / "Needle Folder"}};
    TEST_CHECK(manager.m_settings.editCustomSearch(std::nullopt, profile));
    manager.activateContextMenuEntry({.id = kUseCustomSearch, .label = profile.name});
    session.submitSearch();
    finish(manager);
    TEST_CHECK(session.entries().size() == 1);
    const auto previous = manager.m_settings.searchSettings();
    TEST_CHECK(manager.m_settings.saveSearchExclusions({root / "Needle Folder" / "Deep"}));
    manager.refreshSearchSettings(previous);
    session.submitSearch();
    finish(manager);
    TEST_CHECK(session.searchSelection().profile == profile.name && session.entries().empty());
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(session.searchSelection() == SearchSelection{});
    TEST_CHECK(manager.m_settings.saveSearchExclusions({}));
    TEST_CHECK(fs::remove(root / "Needle Folder" / "Deep" / "scoped fixture.txt"));
  }

  template <typename T> static void collect(Node& node, std::vector<T*>& result) {
    if (auto* found = dynamic_cast<T*>(&node))
      result.push_back(found);
    for (const auto& child : node.children())
      collect(*child, result);
  }

  static void testIdleSearchAndOptionSwitches(FileManager& manager, const fs::path& root) {
    using namespace filemanager;
    using namespace filemanager::menu_id;
    manager.navigate(root);
    waitFor([&] { return !manager.activeSession().loading(); });
    auto& tab = manager.activeTab();
    tab.selectedPath = root / "needle.txt";
    tab.selectedPaths.insert(tab.selectedPath->native());
    tab.selectionAnchor = tab.selectedPath;
    tab.scrollState.offset = 54.0F;

    manager.toggleSearch();
    TEST_CHECK(tab.session.searchOpen() && !manager.m_listHeader.searchView() && tab.selectedPath);
    manager.activateContextMenuEntry({.id = kSearchShowHidden});
    TEST_CHECK(tab.session.searchOptions() == SearchOptions{true, false});
    TEST_CHECK(!manager.m_listHeader.searchView() && !tab.session.showHidden() && tab.selectedPath);
    const CustomSearch profile{"Link policy", {root}, {}, false, {false, true}};
    TEST_CHECK(manager.m_settings.editCustomSearch(std::nullopt, profile));
    manager.activateContextMenuEntry({.id = kUseCustomSearch, .label = profile.name});
    TEST_CHECK(tab.session.searchOptions() == SearchOptions{false, true});
    manager.activateContextMenuEntry({.id = kSearchShowHidden});
    TEST_CHECK(tab.session.searchOptions() == SearchOptions{true, true});
    TEST_CHECK(manager.m_settings.searchSettings().standardOptions == SearchOptions{true, false});
    manager.activateContextMenuEntry({.id = kFirstSearchScope});
    TEST_CHECK(tab.session.searchOptions() == SearchOptions{true, false});

    query(manager, "needle");
    finish(manager);
    TEST_CHECK(manager.m_listHeader.searchView() && !tab.selectedPath);
    manager.activateContextMenuEntry({.id = kSearchFollowLinks});
    TEST_CHECK(tab.session.search().status() == Status::Searching && tab.session.entries().empty());
    TEST_CHECK(tab.session.search().query() == "needle" && tab.session.searchOptions() == SearchOptions{true, true});
    tab.session.submitSearch();
    manager.activateContextMenuEntry({.id = kSearchShowHidden});
    TEST_CHECK(tab.session.search().status() == Status::Searching && tab.session.entries().empty());
    TEST_CHECK(tab.session.searchOptions() == SearchOptions{false, true});
    tab.session.submitSearch();
    finish(manager);
    TEST_CHECK(tab.session.search().status() == Status::Finished);
    query(manager, "");
    TEST_CHECK(!manager.m_listHeader.searchView() && tab.session.searchOpen());
    TEST_CHECK(tab.selectedPath == root / "needle.txt" && tab.scrollState.offset == 54.0F);

    query(manager, "needle");
    manager.toggleSearch();
    TEST_CHECK(!tab.session.searchOpen() && !tab.session.search().active());
    TEST_CHECK(tab.selectedPath == root / "needle.txt");
    manager.toggleSearch();
    query(manager, "needle");
    auto* close = static_cast<Button*>(manager.m_searchBar->children().back().get());
    manager.m_dispatcher.setFocus(close->inputArea());
    key(manager, XKB_KEY_Return);
    TEST_CHECK(!tab.session.searchOpen() && !tab.session.search().active());
    TEST_CHECK(tab.selectedPath == root / "needle.txt");
    FileManagerSettings reopened(manager.m_configService);
    TEST_CHECK(reopened.searchSettings().standardOptions == SearchOptions{false, true});
    TEST_CHECK(manager.m_settings.saveStandardSearchOptions({}));
    const auto stored = std::ranges::find(reopened.searchSettings().profiles, profile.name, &CustomSearch::name);
    TEST_CHECK(stored != reopened.searchSettings().profiles.end() && stored->options == SearchOptions{true, true});
  }

  static void testSearchDropdownPlacement() {
    using namespace filemanager;
    Node trigger;
    trigger.setPosition(700, 56);
    trigger.setSize(136, 32);
    ContextMenuPopupRequest request;
    anchorSearchDropdown(request, trigger);
    TEST_CHECK(request.anchor.x == 700 && request.anchor.y == 56);
    TEST_CHECK(request.anchor.width == 136 && request.anchor.height == 32);
    TEST_CHECK(request.minMenuWidth == 136 && request.maxMenuWidth == 0 && request.menuWidth == 0);
    TEST_CHECK(request.placement->anchor == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    TEST_CHECK(request.placement->gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    TEST_CHECK(request.placement->offsetY == Style::spaceXs && request.fitAvailableHeight);
    const auto geometry = popup_chrome::computeGeometry(203, 292, ShellConfig::ShadowConfig{});
    const auto x = popup_chrome::adjustedOffsetX(0, geometry, request.placement->chromeAttachment.horizontal);
    const auto y = popup_chrome::adjustedOffsetY(
        request.placement->offsetY, geometry, request.placement->chromeAttachment.vertical
    );
    TEST_CHECK(x + geometry.contentX() == 0);
    TEST_CHECK(y + geometry.contentY() == Style::spaceXs);
  }

  static void testRootLineEdits() {
    filemanager::CustomSearchEditorState state;
    state.load("/folder\n/");
    TEST_CHECK(state.hasCompletedRoot());
    state.edit("/", 1);
    TEST_CHECK(!state.hasCompletedRoot());
    state.edit("/\n", 2, 0);
    TEST_CHECK(state.hasCompletedRoot());
    state.edit(" / \n", 3);
    TEST_CHECK(state.hasCompletedRoot());
    state.edit(" /etc \n", 5);
    TEST_CHECK(!state.hasCompletedRoot());
    state.load("");
    state.edit("/\n/var", 6);
    TEST_CHECK(state.hasCompletedRoot());
    state.load("\n/var");
    state.edit("/\n/var", 1);
    TEST_CHECK(!state.hasCompletedRoot());
  }

  static void testQuickLook(FileManager& manager, const fs::path& selected) {
    using namespace filemanager;
    VirtualGridView grid;
    manager.m_grid = &grid;
    manager.m_surface = std::make_unique<ToplevelSurface>(manager.m_wayland);
    auto& tab = manager.activeTab();
    // Ctrl+Space can leave the keyboard cursor on an unselected item.
    tab.selectedPath = selected.parent_path() / "Needle Folder";
    tab.selectedPaths = {selected.string()};
    auto& dialog = manager.m_modalDialog;
    TestRenderer renderer;
    dialog.m_initialLayoutRendererForTest = &renderer;
    ModalDialogHost::DialogSize presented;
    dialog.m_presentToplevelForTest = [&](auto&, auto*, auto size) { presented = size; };
    auto cleanup = nythera::test::makeScopeExit([&] {
      dialog.forceCloseImmediate();
      dialog.m_presentToplevelForTest = {};
      dialog.m_initialLayoutRendererForTest = nullptr;
      manager.m_surface.reset();
      manager.m_grid = nullptr;
      tab.selectedPath.reset();
      tab.selectedPaths.clear();
    });

    manager.activateContextMenuEntry({.id = kQuickLookMenuId});
    TEST_CHECK(dialog.isOpen() && dialog.m_title == selected.filename().string());
    TEST_CHECK(!dialog.surface());
    waitFor([&] { return dialog.surface() != nullptr; });
    TEST_CHECK(presented == ModalDialogHost::DialogSize{780, 620});
    TEST_CHECK(!dialog.m_dialogHeightAnimation.hasActive());
    dialog.recordConfiguredSize(780, 620);
    dialog.recordConfiguredSize(810, 660);
    dialog.setCompactHeight(325.0F);
    TEST_CHECK(dialog.m_dialogHeightAnimation.hasActive());
    dialog.recordConfiguredSize(810, 325);
    TEST_CHECK(dialog.normalHeight() == 660);
    TEST_CHECK(dialog.m_compactDialogHeight == 325);
    dialog.setCompactHeight(std::nullopt);
    TEST_CHECK(dialog.m_normalDialogSize == ModalDialogHost::DialogSize{810, 660});
    dialog.forceCloseImmediate();
    TEST_CHECK(manager.m_settings.loadDialogSize("quick_look") == ModalDialogHost::DialogSize{810, 660});

    key(manager, XKB_KEY_space);
    TEST_CHECK(dialog.isOpen() && !dialog.surface());
    dialog.handleKeyboard({.sym = XKB_KEY_Escape, .pressed = true});
    waitFor([&] { return !dialog.isOpen(); });

    key(manager, XKB_KEY_space);
    TEST_CHECK(dialog.isOpen() && dialog.m_title == selected.filename().string());
    waitFor([&] { return dialog.surface() != nullptr; });
    TEST_CHECK(presented == ModalDialogHost::DialogSize{810, 660});

    dialog.forceCloseImmediate();
    const auto folder = selected.parent_path() / "Needle Folder";
    tab.selectedPaths = {folder.string()};
    key(manager, XKB_KEY_space);
    TEST_CHECK(dialog.isOpen() && !dialog.surface());
    waitFor([&] { return dialog.surface() != nullptr; });
    TEST_CHECK(presented.first == 810 && presented.second < 660);
    TEST_CHECK(dialog.m_compactDialogHeight == presented.second);
    TEST_CHECK(!dialog.m_dialogHeightAnimation.hasActive());
    dialog.recordConfiguredSize(presented.first, presented.second);
    TEST_CHECK(dialog.normalHeight() == 660);
    dialog.forceCloseImmediate();
    TEST_CHECK(manager.m_settings.loadDialogSize("quick_look") == ModalDialogHost::DialogSize{810, 660});
  }

  static void testProfileDialogs(FileManager& manager, const fs::path& folder) {
    using namespace filemanager;
    auto& parent = manager.m_modalDialog;
    auto& editor = manager.m_customSearchChild;
    ToplevelSurface* presentedParent = nullptr;
    ModalDialogHost::DialogSize presentedDimensions;
    parent.m_presentToplevelForTest = [&](auto&, auto*, auto size) { presentedDimensions = size; };
    editor.m_presentToplevelForTest = [&](auto&, auto* surface, auto size) {
      presentedParent = surface;
      presentedDimensions = size;
    };
    auto cleanup = nythera::test::makeScopeExit([&] {
      editor.forceCloseImmediate();
      parent.forceCloseImmediate();
      parent.m_presentToplevelForTest = {};
      editor.m_presentToplevelForTest = {};
    });
    const auto longLocation = folder / std::string(180, 'x');
    const CustomSearch profile{"Editable root", {longLocation, "/"}, {folder / "exclude"}, true, {true, true}};
    TEST_CHECK(manager.m_settings.editCustomSearch(std::nullopt, profile));

    manager.manageCustomSearches();
    auto* original = parent.surface();
    TEST_CHECK(original && presentedDimensions == ModalDialogHost::DialogSize{620, 470});
    parent.recordConfiguredSize(620, 470);
    parent.recordConfiguredSize(830, 630);
    const auto hasLabel = [&](std::string_view text) {
      std::vector<Label*> labels;
      collect(*parent.m_root, labels);
      return std::ranges::any_of(labels, [text](const auto* label) { return label->text() == text; });
    };
    const auto profileCount = manager.m_settings.searchSettings().profiles.size();
    const auto checkCount = [&](std::size_t count) {
      TEST_CHECK(hasLabel(std::to_string(count) + (count == 1 ? " saved profile" : " saved profiles")));
    };
    TEST_CHECK(parent.hoverTooltipsEnabled());
    TEST_CHECK(hasLabel("Saved search profiles") && hasLabel("+1 more"));
    TEST_CHECK(hasLabel("1 additional exclusion · Mounted filesystems · Hidden items · Follow symbolic links"));
    checkCount(profileCount);

    const auto rowHit = [&](std::string_view profileName) -> InputArea* {
      std::vector<InputArea*> areas;
      collect(*parent.m_root, areas);
      for (auto* area : areas) {
        const auto content = area->tooltipContent();
        const auto* rows = std::get_if<std::vector<TooltipRow>>(&content);
        if (rows && !rows->empty() && rows->front().key == "Profile" && rows->front().value == profileName)
          return area;
      }
      TEST_CHECK(false);
      return nullptr;
    };
    auto* hit = rowHit("Editable root");
    const auto tooltip = std::get<std::vector<TooltipRow>>(hit->tooltipContent());
    std::string fullLocation;
    const auto pathStart = std::ranges::find(tooltip, "Location 1", &TooltipRow::key);
    TEST_CHECK(pathStart != tooltip.end());
    for (auto part = pathStart; part != tooltip.end() && (part == pathStart || part->key.empty()); ++part)
      fullLocation += part->value;
    TEST_CHECK(fullLocation == longLocation.native());
    const auto secondLocation = std::ranges::find(tooltip, "Location 2", &TooltipRow::key);
    TEST_CHECK(secondLocation != tooltip.end() && secondLocation->value == "/");

    const auto activate = [&](std::string_view action, std::string_view profileName = {}) {
      std::vector<Button*> buttons;
      auto* scope = profileName.empty() ? parent.m_root.get() : rowHit(profileName)->parent();
      collect(*scope, buttons);
      const auto button = std::ranges::find_if(buttons, [action](auto* candidate) {
        const auto content = candidate->inputArea()->tooltipContent();
        const auto* tooltipText = std::get_if<std::string>(&content);
        return (candidate->label() && candidate->label()->text() == action) || (tooltipText && *tooltipText == action);
      });
      TEST_CHECK(button != buttons.end());
      parent.setFocus((*button)->inputArea());
      parent.handleKeyboard({.sym = XKB_KEY_Return, .pressed = true});
      TEST_CHECK(editor.isOpen() && presentedParent == original);
    };
    const auto openEdit = [&] { activate("Edit search profile", "Editable root"); };
    {
      TestRenderer renderer;
      UiPhaseScope phase(UiPhase::Layout);
      parent.m_root->setSize(620, 470);
      parent.m_root->layout(renderer);
      float x = 0;
      float y = 0;
      Node::absolutePosition(hit, x, y);
      TEST_CHECK(hit->width() > 300.0F && hit->height() < 100.0F);
      const auto click = [&](std::uint32_t time) {
        parent.m_dispatcher.pointerButton(x + 10, y + 10, BTN_LEFT, true, 1, time, false);
        parent.m_dispatcher.pointerButton(x + 10, y + 10, BTN_LEFT, false, 1, time + 20, false);
      };
      click(1000);
      TEST_CHECK(!editor.isOpen());
      click(1110);
      TEST_CHECK(editor.isOpen() && presentedParent == original);
    }
    editor.closeDialog(0);
    waitFor([&] { return !editor.isOpen(); });

    openEdit();
    TEST_CHECK(presentedDimensions == ModalDialogHost::DialogSize{690, 680});
    editor.recordConfiguredSize(690, 680);
    editor.recordConfiguredSize(930, 760);
    parent.closeDialog(-1);
    TEST_CHECK(!parent.closeQueued());
    editor.closeDialog(0);
    waitFor([&] { return !editor.isOpen(); });
    TEST_CHECK(parent.surface() == original);
    openEdit();
    TEST_CHECK(presentedDimensions == ModalDialogHost::DialogSize{930, 760});
    editor.closeDialog(-1);
    waitFor([&] { return !editor.isOpen(); });
    TEST_CHECK(parent.surface() == original);
    openEdit();

    std::vector<Input*> fields;
    std::vector<Checkbox*> checkboxes;
    collect(*editor.m_root, fields);
    collect(*editor.m_root, checkboxes);
    TEST_CHECK(fields.size() == 3 && checkboxes.size() == 3);
    TEST_CHECK(checkboxes[0]->checked() && checkboxes[1]->checked());
    auto* checkbox = checkboxes.back();
    TEST_CHECK(checkbox->checked() && checkbox->parent()->parent()->visible());
    {
      TestRenderer renderer;
      UiPhaseScope phase(UiPhase::Layout);
      std::vector<ScrollView*> scrolls;
      collect(*editor.m_root, scrolls);
      TEST_CHECK(scrolls.size() == 1);
      editor.m_root->setSize(690, 680);
      editor.m_root->layout(renderer);
      TEST_CHECK(fields[1]->height() >= 150 && fields[2]->height() >= 150);
      TEST_CHECK(!scrolls.front()->scrollable());
      editor.m_root->setSize(430, 330);
      editor.m_root->layout(renderer);
      TEST_CHECK(scrolls.front()->scrollable());
    }
    auto* included = fields[1];
    included->selectAll();
    included->textInputApplyEdit({.commitText = "/", .hasCommitText = true});
    TEST_CHECK(!checkbox->parent()->parent()->visible() && !checkbox->checked());
    TEST_CHECK(checkboxes[0]->parent()->parent()->visible() && checkboxes[1]->parent()->parent()->visible());
    editor.setFocus(included->inputArea());
    editor.handleKeyboard({.sym = XKB_KEY_Return, .pressed = true});
    TEST_CHECK(checkbox->parent()->parent()->visible() && !checkbox->checked());
    included->textInputApplyEdit({.commitText = folder.native(), .hasCommitText = true});
    TEST_CHECK(checkbox->parent()->parent()->visible());

    auto* name = fields[0];
    name->selectAll();
    name->textInputApplyEdit({.commitText = "Renamed profile", .hasCommitText = true});
    editor.closeDialog(1);
    waitFor([&] { return !editor.isOpen(); });
    TEST_CHECK(parent.surface() == original);
    std::vector<Label*> labels;
    collect(*parent.m_root, labels);
    TEST_CHECK(std::ranges::any_of(labels, [](const auto* label) { return label->text() == "Renamed profile"; }));
    const auto& profiles = manager.m_settings.searchSettings().profiles;
    const auto edited = std::ranges::find(profiles, "Renamed profile", &CustomSearch::name);
    TEST_CHECK(edited != profiles.end() && edited->options == SearchOptions{true, true});
    checkCount(profileCount);
    for (const int result : {0, -1}) {
      activate("New search");
      TEST_CHECK(presentedDimensions == ModalDialogHost::DialogSize{930, 760});
      editor.closeDialog(result);
      waitFor([&] { return !editor.isOpen(); });
      TEST_CHECK(parent.surface() == original);
      checkCount(profileCount);
    }
    activate("New search");
    fields.clear();
    collect(*editor.m_root, fields);
    TEST_CHECK(fields.size() == 3 && fields[0]->value().empty());
    fields[0]->textInputApplyEdit({.commitText = "New from management", .hasCommitText = true});
    fields[1]->textInputApplyEdit({.commitText = folder.native(), .hasCommitText = true});
    editor.closeDialog(1);
    waitFor([&] { return !editor.isOpen(); });
    TEST_CHECK(parent.surface() == original && hasLabel("New from management"));
    checkCount(profileCount + 1);
    {
      TestRenderer renderer;
      UiPhaseScope phase(UiPhase::Layout);
      parent.m_root->setSize(420, 240);
      parent.m_root->layout(renderer);
      std::vector<ScrollView*> scrolls;
      collect(*parent.m_root, scrolls);
      TEST_CHECK(scrolls.size() == 1 && scrolls.front()->scrollable());
      TEST_CHECK(scrolls.front()->height() < parent.m_root->height());
    }
    for (const int result : {0, 1}) {
      activate("Delete search profile", "New from management");
      TEST_CHECK(editor.m_title == "Delete Custom Search?");
      editor.closeDialog(result);
      waitFor([&] { return !editor.isOpen(); });
      TEST_CHECK(parent.surface() == original);
      TEST_CHECK(hasLabel("New from management") == (result == 0));
      checkCount(profileCount + (result == 0 ? 1 : 0));
    }
    parent.closeDialog(0);
    waitFor([&] { return !parent.isOpen(); });

    manager.editCustomSearch(std::nullopt);
    TEST_CHECK(parent.isOpen() && !editor.isOpen());
    TEST_CHECK(presentedDimensions == ModalDialogHost::DialogSize{930, 760});
    fields.clear();
    checkboxes.clear();
    collect(*parent.m_root, fields);
    collect(*parent.m_root, checkboxes);
    fields[0]->textInputApplyEdit({.commitText = "Final line", .hasCommitText = true});
    fields[1]->textInputApplyEdit({.commitText = "/", .hasCommitText = true});
    TEST_CHECK(!checkboxes.back()->parent()->parent()->visible());
    parent.closeDialog(1);
    waitFor([&] { return !parent.isOpen(); });
    const auto& saved = manager.m_settings.searchSettings().profiles;
    const auto final = std::ranges::find(saved, "Final line", &CustomSearch::name);
    TEST_CHECK(final != saved.end() && final->included == SearchLocations{"/"} && !final->includeMounts);
    FileManagerSettings reopened(manager.m_configService);
    TEST_CHECK(reopened.loadDialogSize("custom_search_editor") == ModalDialogHost::DialogSize{930, 760});
    TEST_CHECK(reopened.loadDialogSize("custom_search_management") == ModalDialogHost::DialogSize{830, 630});
    TEST_CHECK(ModalDialogHost::initialExtent(930, 690, 420, 750) == 750);
  }

  static void testColumnInteractions(FileManager& manager, const fs::path& root) {
    using namespace filemanager;
    TestRenderer renderer;
    UiPhaseScope phase(UiPhase::Layout);
    auto& header = manager.m_listHeader;
    auto* scene = header.m_header;
    auto scrollbar = header.buildHorizontalScrollbar();
    const FileColumns initial{{200, 104, 170, 144, 152, 152}};
    TEST_CHECK(manager.m_settings.saveListColumnWidths(initial));
    header.reloadColumnPreferences();
    const auto layout = [&](float viewport, bool fit = false) {
      scene->setSize(viewport, 36.0F);
      scene->layout(renderer);
      if (fit)
        header.applyPendingFit(renderer);
      header.applyColumnWidths();
      header.arrangeAfterWidthChange(renderer);
      header.positionColumnMarkers();
    };
    const auto bounds = [&](FileColumn column) {
      return listColumnBounds(
          column, header.listColumns(), header.visibleColumns(), Style::spaceSm, header.scrollOffset()
      );
    };
    const auto event = [&](PointerEvent::Type type, float x, bool pressed = false, std::uint32_t time = 0) {
      return header.handlePointer({.type = type, .time = time, .button = BTN_LEFT, .pressed = pressed}, x, 14.0F);
    };
    const auto divider = [&](FileColumn column) {
      return bounds(column).right + (column == header.visibleColumns().back() ? 0.0F : Style::spaceSm * 0.5F);
    };
    layout(405.0F);
    TEST_CHECK(scrollbar->visible() && header.listColumns().widths == initial.widths);
    const auto left = bounds(FileColumn::Size).left;
    const auto modifiedLeft = bounds(FileColumn::Modified).left;
    const auto sortBefore = header.sortField();
    const auto sortOrderBefore = header.sortOrder();
    const auto x = divider(FileColumn::Size);
    TEST_CHECK(event(PointerEvent::Type::Button, x, true, 100));
    TEST_CHECK(event(PointerEvent::Type::Motion, x + 40, false, 110));
    TEST_CHECK(bounds(FileColumn::Size).left == left && bounds(FileColumn::Modified).left == modifiedLeft + 40);
    TEST_CHECK(header.listColumns()[FileColumn::Name] == initial[FileColumn::Name]);
    TEST_CHECK(header.listColumns()[FileColumn::Modified] == initial[FileColumn::Modified]);
    TEST_CHECK(event(PointerEvent::Type::Button, x + 40, false, 120));
    TEST_CHECK(header.sortField() == sortBefore && header.sortOrder() == sortOrderBefore);
    layout(760.0F);
    TEST_CHECK(!scrollbar->visible() && header.listColumns()[FileColumn::Size] == 144.0F);
    layout(330.0F);
    TEST_CHECK(header.scrollHorizontally(1000.0F) && header.scrollOffset() > 0);
    layout(330.0F);
    const auto finalEdge = divider(FileColumn::Modified);
    TEST_CHECK(header.columnAt(finalEdge - 10) == FileColumn::Modified);
    const auto finalLeft = bounds(FileColumn::Modified).left;
    TEST_CHECK(event(PointerEvent::Type::Button, finalEdge, true, 1000));
    TEST_CHECK(event(PointerEvent::Type::Motion, finalEdge - 300, false, 1010));
    TEST_CHECK(bounds(FileColumn::Modified).left == finalLeft);
    TEST_CHECK(header.listColumns()[FileColumn::Modified] == listColumnWidth(FileColumn::Modified, 0.0F));
    TEST_CHECK(event(PointerEvent::Type::Button, finalEdge - 300, false, 1020));
    TEST_CHECK(header.listColumns()[FileColumn::Size] == 144.0F);

    layout(760.0F);
    const auto current = divider(FileColumn::Name);
    TEST_CHECK(event(PointerEvent::Type::Button, current, true, 2000));
    TEST_CHECK(event(PointerEvent::Type::Button, current, false, 2040));
    TEST_CHECK(event(PointerEvent::Type::Button, current, true, 2100));
    TEST_CHECK(header.m_pendingFit == std::vector{FileColumn::Name});
    layout(760.0F, true);
    TEST_CHECK(header.listColumns()[FileColumn::Name] != initial[FileColumn::Name]);
    TEST_CHECK(header.listColumns()[FileColumn::Size] == 144.0F);

    const auto widths = header.listColumns();
    const auto middle = (bounds(FileColumn::Name).left + bounds(FileColumn::Name).right) * 0.5F;
    TEST_CHECK(!event(PointerEvent::Type::Button, middle, true, 3000));
    TEST_CHECK(event(PointerEvent::Type::Motion, bounds(FileColumn::Modified).right, false, 3100));
    TEST_CHECK(header.m_dropMarker->visible());
    TEST_CHECK(event(PointerEvent::Type::Button, bounds(FileColumn::Modified).right, false, 3200));
    TEST_CHECK(header.visibleColumns().back() == FileColumn::Name);
    TEST_CHECK(header.listColumns().widths == widths.widths && header.sortField() == sortBefore);
    layout(315.0F);
    TEST_CHECK(header.scrollHorizontally(110.0F));
    layout(315.0F);

    FileEntryRow row(1.0F);
    row.setAllowThumbnailsAndMetadata(false);
    row.setColumns(header.visibleColumns(), header.listColumns());
    const FileEntry entry{.name = "file.txt", .absPath = root / "file.txt"};
    row.bind(renderer, entry, 0, 315, false, false, false);
    row.setColumnViewport(renderer, header.contentWidth(), header.scrollOffset());
    auto* cells = row.children()[1].get();
    std::vector<Flex*> shown;
    for (const auto& child : cells->children()) {
      if (child->visible() && child->participatesInLayout())
        shown.push_back(static_cast<Flex*>(child.get()));
    }
    TEST_CHECK(shown.size() == header.visibleColumns().size());
    for (std::size_t i = 0; i < shown.size(); ++i) {
      float hx = 0, hy = 0, rx = 0, ry = 0;
      Node::absolutePosition(header.m_headers[columnIndex(header.visibleColumns()[i])], hx, hy);
      Node::absolutePosition(shown[i], rx, ry);
      TEST_CHECK(std::abs(hx - rx) < 0.1F);
      TEST_CHECK(std::abs(hx - bounds(header.visibleColumns()[i]).left) < 0.1F);
    }
    const auto clickHeading = [&](FileColumn column, std::uint32_t time) {
      layout(760.0F);
      const auto cell = bounds(column);
      const float x = (cell.left + cell.right) * 0.5F;
      TEST_CHECK(!event(PointerEvent::Type::Button, x, true, time));
      manager.m_dispatcher.pointerButton(x, 14.0F, BTN_LEFT, true, 0, time, false);
      TEST_CHECK(!event(PointerEvent::Type::Button, x, false, time + 20));
      manager.m_dispatcher.pointerButton(x, 14.0F, BTN_LEFT, false, 0, time + 20, false);
    };
    clickHeading(FileColumn::Modified, 4100);
    TEST_CHECK(header.sortField() == FileDialogSortField::Modified);
    const auto order = header.sortOrder();
    clickHeading(FileColumn::Modified, 5100);
    TEST_CHECK(header.sortOrder() != order);
    header.m_horizontalTrack = nullptr;
    header.m_horizontalScrollbar = nullptr;
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

  static void testFiltersUi(FileManager& manager, const fs::path& root) {
    const auto folder = root / "filters";
    TEST_CHECK(fs::create_directory(folder));
    writeFile(folder / "one.txt", "needle");
    writeFile(folder / "two.txt", "other words");
    manager.navigate(folder);
    auto& session = manager.activeSession();
    waitFor([&] { return !session.loading(); });
    key(manager, XKB_KEY_f, KeyMod::Ctrl);

    auto& ui = manager.m_searchFilterUi;
    const auto click = [&](Button* button) {
      manager.m_dispatcher.setFocus(button->inputArea());
      key(manager, XKB_KEY_Return);
    };
    click(ui.toggle);
    TEST_CHECK(ui.panel->visible());
    auto* scene = static_cast<Flex*>(manager.m_searchTools->parent());
    TestRenderer renderer;
    const auto layout = [&](float width) {
      scene->setSize(width, 500.0F);
      manager.updateSearchFilterWidth(width, renderer);
      scene->layout(renderer);
    };
    auto& draft = session.searchFilters();
    TEST_CHECK(draft.sizeFirst.empty() && draft.sizeSecond.empty() && draft.dateAmount.empty());
    TEST_CHECK(ui.toggle->label()->text() == "0" && !session.search().active());
    TEST_CHECK(!ui.values[0]->invalid() && !ui.values[2]->invalid());
    using namespace filemanager;
    manager.changeSearchMetadata([](auto& filters) {
      filters.sizeComparison = SizeComparison::Between;
      filters.dateComparison = DateComparison::Between;
    });
    const auto labelsAligned = [&] {
      float sizeX = 0, contentX = 0, y = 0;
      Node::absolutePosition(ui.choices[0], sizeX, y);
      Node::absolutePosition(ui.values.back(), contentX, y);
      return sizeX == contentX;
    };
    for (float width : {1050.0F, 1600.0F}) {
      layout(width);
      TEST_CHECK(ui.metadata->direction() == FlexDirection::Horizontal && ui.separator->visible());
      TEST_CHECK(ui.content->direction() == FlexDirection::Horizontal);
      TEST_CHECK(ui.size->y() == ui.date->y() && ui.dates[0]->visible() && ui.dates[1]->visible());
      TEST_CHECK(ui.values[0]->width() >= 80.0F && ui.values[1]->width() == ui.values[0]->width());
      TEST_CHECK(ui.values.back()->width() > 140.0F && labelsAligned());
      TEST_CHECK(ui.choices[0]->x() - ui.sizeCaption->width() == Style::spaceSm);
      TEST_CHECK(ui.separator->x() - (ui.size->x() + ui.size->width()) == Style::spaceLg);
      TEST_CHECK(ui.date->x() - (ui.separator->x() + ui.separator->width()) == Style::spaceLg);
    }
    layout(390.0F);
    TEST_CHECK(labelsAligned());
    TEST_CHECK(ui.metadata->direction() == FlexDirection::Vertical && !ui.separator->visible());
    TEST_CHECK(ui.date->y() > ui.size->y() && ui.content->direction() == FlexDirection::Vertical);
    TEST_CHECK(ui.clear->parent() == manager.m_searchBar);
    const auto* sizeHeading = ui.choices[0]->parent();
    const auto* firstBound = ui.values[0]->parent();
    const auto* secondBound = ui.values[1]->parent();
    TEST_CHECK(firstBound->y() > sizeHeading->y() || secondBound->y() > firstBound->y());
    TEST_CHECK(firstBound->x() + firstBound->width() <= ui.size->width());
    TEST_CHECK(secondBound->x() + secondBound->width() <= ui.size->width());
    const auto* addressBar = scene->children().front().get();
    float addressX = 0, addressY = 0, searchX = 0, searchY = 0;
    Node::absolutePosition(addressBar, addressX, addressY);
    Node::absolutePosition(manager.m_searchBar, searchX, searchY);
    TEST_CHECK(searchX == addressX && searchY - (addressY + addressBar->height()) == Style::spaceSm);
    const auto edit = [&](Input* input, std::string_view text) {
      manager.m_dispatcher.setFocus(input->inputArea());
      input->selectAll();
      if (text.empty())
        key(manager, XKB_KEY_BackSpace);
      else
        input->textInputApplyEdit({.commitText = std::string(text), .hasCommitText = true});
    };
    manager.changeSearchMetadata([](auto& filters) {
      filters.sizeFirstUnit = SizeUnit::Bytes;
      filters.sizeSecondUnit = SizeUnit::Bytes;
    });
    edit(ui.values[0], "6");
    TEST_CHECK(ui.toggle->label()->text() == "0" && !session.search().active());
    edit(ui.values[1], "invalid");
    TEST_CHECK(ui.values[1]->invalid() && ui.toggle->label()->text() == "0");
    edit(ui.values[1], "6");
    TEST_CHECK(manager.m_searchSpinner->spinning());
    finish(manager);
    TEST_CHECK(manager.m_searchStatus->text() == "1 found" && !manager.m_searchSpinner->spinning());
    TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "one.txt");
    TEST_CHECK(ui.toggle->label()->text() == "1");
    click(ui.toggle);
    TEST_CHECK(!ui.panel->visible() && ui.toggle->label()->text() == "1");
    TEST_CHECK(session.search().active());

    click(ui.toggle);
    click(ui.clear);
    TEST_CHECK(!session.search().active() && session.entries().size() == 2);
    TEST_CHECK(!manager.m_searchStatusArea->visible());
    TEST_CHECK(ui.toggle->label()->text() == "0");

    if (ui.backends.supports(ContentMode::Plain)) {
      draft.content.mode = ContentMode::Plain;
      manager.changeSearchMetadata([](auto& filters) {
        filters.sizeComparison = SizeComparison::Larger;
        filters.sizeFirstUnit = SizeUnit::Bytes;
        filters.sizeFirst = "5";
        filters.dateComparison = DateComparison::Between;
        filters.dateFirst = {2000, 1, 1};
      });
      TEST_CHECK(ui.toggle->label()->text() == "1");
      manager.changeSearchMetadata([](auto& filters) { filters.dateSecond = {1999, 1, 1}; });
      TEST_CHECK(ui.toggle->label()->text() == "1");
      manager.changeSearchMetadata([](auto& filters) { filters.dateSecond = {2099, 12, 31}; });
      TEST_CHECK(ui.toggle->label()->text() == "2");
      query(manager, "txt");
      finish(manager);
      TEST_CHECK(session.entries().size() == 2);

      auto* content = ui.values.back();
      edit(content, "needlex");
      key(manager, XKB_KEY_BackSpace);
      TEST_CHECK(session.path() == folder && content->value() == "needle");
      TEST_CHECK(session.search().status() == Status::Finished && !draft.submitted && session.entries().size() == 2);
      key(manager, XKB_KEY_Return);
      TEST_CHECK(manager.m_searchSpinner->spinning());
      finish(manager);
      TEST_CHECK(manager.m_searchStatus->text() == "1 found" && !manager.m_searchSpinner->spinning());
      TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "one.txt");
      TEST_CHECK(draft.submitted && ui.toggle->label()->text() == "3");
      edit(content, "other");
      TEST_CHECK(session.search().status() == Status::Finished && session.entries().front().name == "one.txt");
      click(ui.submit);
      finish(manager);
      TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "two.txt");

      query(manager, "one");
      finish(manager);
      TEST_CHECK(session.entries().empty());
      edit(content, "");
      finish(manager);
      TEST_CHECK(!draft.submitted && ui.toggle->label()->text() == "2");
      TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "one.txt");
      manager.changeSearchMetadata([](auto& filters) { filters.dateSecond = {}; });
      TEST_CHECK(ui.toggle->label()->text() == "1");
      edit(ui.values[0], "");
      TEST_CHECK(ui.toggle->label()->text() == "0");
      click(ui.clear);
      TEST_CHECK(session.search().query() == "one" && !draft.dateFirst.year && draft.sizeFirst.empty());
    }
    manager.closeSearch();
    TEST_CHECK(!ui.panel->visible() && !session.search().active());
  }

  static void testContentResultActions(FileManager& manager, const fs::path& root) {
    g_autofree char* ffmpeg = g_find_program_in_path("ffmpeg");
    if (!ffmpeg || !filemanager::findContentBackends().supports(filemanager::ContentMode::Extracted))
      return;

    g_auto(GStrv) savedMimeDirs = g_strdupv(const_cast<char**>(g_content_type_get_mime_dirs()));
    const char* mimeDirs[] = {"/usr/share/mime", nullptr};
    g_content_type_set_mime_dirs(mimeDirs);
    auto restoreMimeDirs = nythera::test::makeScopeExit([&] { g_content_type_set_mime_dirs(savedMimeDirs); });

    const auto folder = root / "content-actions";
    TEST_CHECK(fs::create_directory(folder));
    const auto video = folder / "metadata-fixture.mp4";
    const char* args[] = {ffmpeg,        "-nostdin",
                          "-v",          "error",
                          "-f",          "lavfi",
                          "-i",          "color=c=black:s=16x16:r=1",
                          "-t",          "1",
                          "-c:v",        "mpeg4",
                          "-metadata",   "title=Tagged search fixture",
                          video.c_str(), nullptr};
    g_autoptr(GSubprocess) encoder = g_subprocess_newv(args, G_SUBPROCESS_FLAGS_NONE, nullptr);
    TEST_CHECK(encoder && g_subprocess_wait_check(encoder, nullptr, nullptr));

    manager.navigate(folder);
    auto& session = manager.activeSession();
    waitFor([&] { return !session.loading(); });
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    auto& draft = session.searchFilters();
    draft.content = {"Tagged search fixture", filemanager::ContentMode::Extracted};
    manager.submitSearchFilters();
    finish(manager);
    TEST_CHECK(session.entries().size() == 1);
    const auto& entry = session.entries().front();
    TEST_CHECK(entry.absPath == video && entry.type == "video/mp4" && entry.hasIdentity && !entry.isSymlink);
    g_autoptr(GIcon) icon = g_content_type_get_icon(entry.type.c_str());
    TEST_CHECK(icon && G_IS_THEMED_ICON(icon));
    const auto* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    TEST_CHECK(names && names[0] && std::string_view(names[0]) == "video-mp4");

    std::vector<ContextMenuControlEntry> entries{{.id = 10, .label = "Open"}};
    manager.m_menus.appendOpenWithEntries(entry, entries);
    TEST_CHECK(entries.front().label == "Open with Isolated video player");
    TEST_CHECK(entries.size() == 2 && entries.back().label == "Open With");
    TEST_CHECK(std::ranges::any_of(entries.back().submenuEntries, [](const auto& item) {
      return item.label == "Alternate video player";
    }));
    manager.m_menus.setContextMenuTarget(video);
    manager.activateContextMenuEntry(entries.front());
    const auto opened = fs::path(video.native() + ".opened");
    waitFor([&] {
      std::error_code error;
      return fs::file_size(opened, error) == video.native().size() && !error;
    });
    std::ifstream launch(opened);
    std::string openedPath;
    std::getline(launch, openedPath);
    TEST_CHECK(openedPath == video.native());
    key(manager, XKB_KEY_Escape);
  }

  static void testProgressAndCancellation(FileManager& manager, const fs::path& bulk) {
    manager.navigate(bulk);
    auto& session = manager.activeSession();
    waitFor([&] { return !session.loading(); });
    using namespace filemanager;
    FileColumns manuallySized;
    manuallySized[FileColumn::Name] = 275.0F;
    manuallySized[kSearchLocationSlot] = 360.0F;
    TEST_CHECK(manager.m_settings.saveListColumnWidths(manuallySized, true));
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
    manager.m_listHeader.m_header->setSize(320.0F, 36.0F);
    manager.m_listHeader.applyColumnWidths();
    TEST_CHECK(manager.m_listHeader.listColumns().widths == manuallySized.widths);
    TEST_CHECK(session.search().status() == Status::Searching);
    TEST_CHECK(manager.m_searchSpinner->spinning());
    TEST_CHECK(manager.m_searchStatus->text() != "Searching… 0 found");
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
    const char* locale = std::getenv("LC_ALL");
    const bool grouped = locale && std::string_view(locale) == "en_GB.utf8";
    if (grouped) {
      TEST_CHECK(manager.m_searchStatus->text() == "12,000 found");
      TEST_CHECK(manager.m_statusDetailLabel->text().starts_with("  |  12,000 files"));
      for (const auto& entry : session.entries() | std::views::take(1000))
        manager.activeTab().selectedPaths.insert(entry.absPath.string());
      manager.updateSelectionSummary();
      TEST_CHECK(manager.m_statusLabel->text() == "1,000 selected");
      TEST_CHECK(manager.m_statusDetailLabel->text().starts_with("  |  0 folders, 1,000 files"));
    }
    TEST_CHECK(heartbeats - beatsBefore > 5);
    TEST_CHECK(session.entries().front().name == "fixture-0");
    TEST_CHECK(session.entries().back().name == "fixture-11999");

    query(manager, "fixture");
    session.submitSearch();
    waitFor([&] { return !session.entries().empty(); });
    const auto found = session.entries().size();
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(session.searchOpen() && session.search().status() == Status::Cancelled);
    TEST_CHECK(session.entries().size() == found && !manager.m_searchSpinner->spinning());
    TEST_CHECK(manager.m_searchStatus->text().starts_with("Search cancelled"));
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(!session.searchOpen() && !session.search().active());
    TEST_CHECK(!manager.m_searchStatusArea->visible() && !manager.m_searchSpinner->spinning());
    TEST_CHECK(manager.m_statusLabel->text() == "12 folders");
    TEST_CHECK(session.entries().size() == 12);
    // A replacement search following Escape must publish only its own results.
    key(manager, XKB_KEY_f, KeyMod::Ctrl);
    query(manager, "replacement");
    finish(manager);
    TEST_CHECK(session.entries().size() == 1 && session.entries().front().name == "Replacement.txt");
    key(manager, XKB_KEY_Escape);

    if (grouped) {
      manager.navigate(bulk / "0");
      waitFor([&] { return !session.loading(); });
      TEST_CHECK(manager.m_statusDetailLabel->text().starts_with("  |  1,001 files"));
    }
  }

  static void testBackgroundContentSearch(FileManager& manager, const fs::path& work) {
    using namespace filemanager;
    const auto folder = work / "lifecycle-files";
    const auto other = work / "lifecycle-other";
    const auto bin = work / "lifecycle-bin";
    fs::create_directories(folder);
    fs::create_directories(other);
    fs::create_directories(bin);
    for (int index = 0; index < 128; ++index)
      writeFile(folder / ("fixture-" + std::to_string(index) + ".txt"), std::string((index * 53) % 128 + 1, 'x'));
    writeFile(other / "other-result.txt", "other");
    const auto rg = bin / "rg";
    writeFile(rg, R"sh(#!/bin/sh
while [ "$1" != -- ]; do
  case "$1" in --regexp=*) query=${1#--regexp=} ;; esac
  shift
done
shift
gate="$NYTHERA_TEST_GATES/$query"
if ! mkdir "$gate.first" 2>/dev/null; then
  sleep 30 &
  child=$!
  printf '%s %s\n' "$$" "$child" > "$gate.pids"
  while ! [ -f "$gate.release" ]; do sleep 0.01; done
  kill "$child"
  wait "$child" 2>/dev/null
fi
for path; do printf '%s\000' "$path"; done
)sh");
    fs::permissions(rg, fs::perms::owner_all);
    const nythera::test::ScopedEnvVar gateEnv("NYTHERA_TEST_GATES", work.native());
    const nythera::test::ScopedEnvVar pathEnv("PATH", bin.native() + ":" + g_getenv("PATH"));
    auto release = nythera::test::makeScopeExit([&] {
      for (auto name : {"continuation", "stop", "navigation", "close", "shutdown", "shutdown-other"})
        std::ofstream(work / (std::string(name) + ".release"));
    });

    manager.navigate(folder);
    auto& searching = manager.activeTab();
    waitFor([&] { return !searching.session.loading(); });
    const auto searchingId = searching.id;
    const auto otherId = manager.openFolderInNewTab(other, false);
    TEST_CHECK(otherId != kInvalidTabId);
    manager.m_tabBar.rebuildIfDirty();
    const auto searchingSpinner = [&] { return manager.m_tabBar.findSlot(searchingId)->searchSpinner; };

    const auto begin = [&](BrowserTab& tab, std::string_view name) {
      manager.focusSearch();
      auto& draft = tab.session.searchFilters();
      draft.content = {std::string(name), ContentMode::Plain};
      manager.submitSearchFilters();
      std::array<pid_t, 2> pids{};
      waitFor([&] {
        std::ifstream processes(work / (std::string(name) + ".pids"));
        return static_cast<bool>(processes >> pids[0] >> pids[1]) && !tab.session.entries().empty();
      });
      TEST_CHECK(tab.session.search().status() == Status::Searching);
      TEST_CHECK(manager.m_tabBar.findSlot(tab.id)->searchSpinner->spinning());
      return pids;
    };
    const auto cleanedUp = [&](const auto& pids) {
      waitFor([&] {
        std::ifstream descendant("/proc/" + std::to_string(pids[1]) + "/stat");
        std::string status;
        const bool stopped = !std::getline(descendant, status) || status.substr(status.rfind(')') + 2).starts_with('Z');
        return stopped && !fs::exists("/proc/" + std::to_string(pids[0]));
      });
    };

    const auto orderMatches = [&](FileDialogSortField field, FileDialogSortOrder direction) {
      return std::ranges::is_sorted(searching.session.entries(), [&](const FileEntry& a, const FileEntry& b) {
        return fileEntryLess(a, b, field, direction, searching.session.foldersFirst(), searching.session.hiddenLast());
      });
    };
    const auto browsingField = searching.viewState.sortField;
    const auto browsingDirection = searching.viewState.sortOrder;
    searching.searchViewState.sortField = FileDialogSortField::Name;
    searching.searchViewState.sortOrder = FileDialogSortOrder::Ascending;
    const auto continuation = begin(searching, "continuation");
    TEST_CHECK(searching.session.entries().size() == 64);
    TEST_CHECK(orderMatches(FileDialogSortField::Name, FileDialogSortOrder::Ascending));
    const auto firstBatch = searching.session.entries().front().absPath;

    const auto clickHeader = [&](FileColumn column) {
      manager.m_dispatcher.setFocus(manager.m_listHeader.m_headers[columnIndex(column)]->inputArea());
      manager.m_dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    };
    clickHeader(FileColumn::Name);
    waitFor([&] { return orderMatches(FileDialogSortField::Name, FileDialogSortOrder::Descending); });
    TEST_CHECK(searching.session.entries().size() == 64 && searching.session.entries().back().absPath == firstBatch);
    TEST_CHECK(searching.session.search().status() == Status::Searching);
    clickHeader(FileColumn::Size);
    waitFor([&] { return orderMatches(FileDialogSortField::Size, FileDialogSortOrder::Descending); });
    clickHeader(FileColumn::Name);
    waitFor([&] { return orderMatches(FileDialogSortField::Name, FileDialogSortOrder::Descending); });
    clickHeader(FileColumn::Name);
    waitFor([&] { return orderMatches(FileDialogSortField::Name, FileDialogSortOrder::Ascending); });
    clickHeader(FileColumn::Name);
    waitFor([&] { return orderMatches(FileDialogSortField::Name, FileDialogSortOrder::Descending); });

    TEST_CHECK(manager.activateTab(otherId));
    manager.m_tabBar.rebuildIfDirty();
    TEST_CHECK(searchingSpinner()->spinning() && !manager.m_searchSpinner->spinning());
    manager.focusSearch();
    query(manager, "other-result");
    finish(manager);
    TEST_CHECK(manager.activeSession().entries().size() == 1);
    auto& otherTab = manager.activeTab();

    // A monitor-dirty browsing listing used to restart the active content search on return.
    searching.needsRefresh = true;
    TEST_CHECK(manager.activateTab(searchingId));
    TEST_CHECK(searching.session.entries().size() == 64);
    TEST_CHECK(searching.session.search().status() == Status::Searching && manager.m_searchSpinner->spinning());
    TEST_CHECK(manager.activateTab(otherId));
    manager.m_tabBar.rebuildIfDirty();
    TEST_CHECK(searchingSpinner()->spinning());
    writeFile(work / "continuation.release");
    waitFor([&] { return searching.session.search().status() == Status::Finished; });
    cleanedUp(continuation);
    TEST_CHECK(!searchingSpinner()->spinning() && !searchingSpinner()->visible());
    TEST_CHECK(searching.session.entries().size() == 128);
    TEST_CHECK(orderMatches(FileDialogSortField::Name, FileDialogSortOrder::Descending));
    TEST_CHECK(otherTab.session.search().query() == "other-result" && otherTab.session.entries().size() == 1);
    TEST_CHECK(manager.activateTab(searchingId));
    TEST_CHECK(manager.m_searchStatus->text() == "128 found");
    clickHeader(FileColumn::Name);
    waitFor([&] { return orderMatches(FileDialogSortField::Name, FileDialogSortOrder::Ascending); });
    TEST_CHECK(searching.session.search().status() == Status::Finished && searching.session.entries().size() == 128);
    TEST_CHECK(searching.viewState.sortField == browsingField && searching.viewState.sortOrder == browsingDirection);

    const auto stop = begin(searching, "stop");
    key(manager, XKB_KEY_Escape);
    TEST_CHECK(searching.session.search().status() == Status::Cancelled && searching.session.entries().size() == 64);
    TEST_CHECK(!searchingSpinner()->spinning() && !manager.m_searchSpinner->spinning());
    cleanedUp(stop);

    const auto navigation = begin(searching, "navigation");
    manager.navigate(other);
    TEST_CHECK(!searching.session.searchOpen() && !searching.session.search().active());
    cleanedUp(navigation);
    manager.navigate(folder);
    waitFor([&] { return !searching.session.loading(); });

    const auto closingId = manager.openFolderInNewTab(folder, true);
    manager.m_tabBar.rebuildIfDirty();
    TEST_CHECK(closingId != kInvalidTabId && manager.activeTab().id == closingId);
    waitFor([&] { return !manager.activeSession().loading(); });
    const auto closing = begin(manager.activeTab(), "close");
    TEST_CHECK(manager.activateTab(searchingId) && manager.closeTab(closingId));
    cleanedUp(closing);

    const auto shutdown = begin(searching, "shutdown");
    const auto secondaryId = manager.openFolderInNewTab(folder, true);
    TEST_CHECK(secondaryId != kInvalidTabId && manager.activeTab().id == secondaryId);
    manager.m_tabBar.rebuildIfDirty();
    auto& secondary = manager.activeTab();
    waitFor([&] { return !secondary.session.loading(); });
    const auto shutdownOther = begin(secondary, "shutdown-other");
    TEST_CHECK(searching.session.search().status() == Status::Searching && searchingSpinner()->spinning());
    // Exercise window shutdown with an owned process group in each of two tabs.
    manager.m_tabs.shutdownAll();
    TEST_CHECK(!searching.session.search().active() && !secondary.session.search().active());
    cleanedUp(shutdown);
    cleanedUp(shutdownOther);
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
    writeFile(root / "needle.txt", "Quick Look fixture\n");
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

    Button searchButton;
    FileManager manager(root.string());
    Flex scene;
    scene.setDirection(FlexDirection::Vertical);
    scene.setAlign(FlexAlign::Stretch);
    scene.setGap(2.0F);
    auto addressBar = std::make_unique<Flex>();
    addressBar->setMinHeight(32.0F);
    scene.addChild(std::move(addressBar));
    manager.m_searchButton = &searchButton;
    manager.buildSearchBar(scene, Style::spaceSm - scene.gap());
    scene.addChild(manager.buildStatusRow(12.0F));
    scene.addChild(manager.m_listHeader.buildHeader());
    scene.addChild(manager.m_tabBar.buildUi());
    manager.m_dispatcher.setSceneRoot(&scene);
    auto detach = nythera::test::makeScopeExit([&] {
      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_tabBar.detachUi();
    });
    manager.enumerate(root);
    waitFor([&] { return !manager.activeSession().loading(); });
    testQuickLook(manager, root / "needle.txt");
    testMatchesAndActions(manager, root, outside);
    testProgressAndCancellation(manager, bulk);
    testMetadataAndHiddenEntries(manager, root);
    testScopeUi(manager, root);
    testFzfModeUi(manager);
    testSearchView(manager, root);
    testIdleSearchAndOptionSwitches(manager, root);
    testSearchDropdownPlacement();
    testRootLineEdits();
    testProfileDialogs(manager, root);
    testColumnInteractions(manager, root);
    testFiltersUi(manager, root);
    testContentResultActions(manager, root);
    testBackgroundContentSearch(manager, work);
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
