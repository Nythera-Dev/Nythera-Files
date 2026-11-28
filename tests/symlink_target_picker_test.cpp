#include "filemanager/dialogs/conflict_dialog_controller.h"
#include "filemanager/dialogs/symlink_dialog_controller.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_manager.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/scroll_into_view.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

  class StubTextures final : public TextureManager {
  public:
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRgba(const std::uint8_t*, int, int, bool) override { return {}; }
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
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * fontSize * 0.5F;
      return {.width = maxWidth > 0.0F ? std::min(natural, maxWidth) : natural, .bottom = fontSize};
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.width = size, .bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
        const TextFont&
    ) override {
      outStops.assign(byteOffsets.size(), 0.0F);
    }
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>& byteOffsets, float,
        std::vector<TextCursorStop>& outStops, const TextFont&
    ) override {
      outStops.assign(byteOffsets.size(), TextCursorStop{});
    }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .bottom = size}; }
    TextureManager& textureManager() override { return m_textures; }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    StubTextures m_textures;
  };

  class StubPresenter final : public FileDialogPresenter {
  public:
    bool openResult = true;
    int openCalls = 0;
    int closeCalls = 0;
    std::optional<FileDialogOptions> lastOptions;

    [[nodiscard]] bool openFileDialog() override {
      ++openCalls;
      lastOptions = FileDialog::currentOptions();
      return openResult;
    }

    void closeFileDialogWithoutResult() override { ++closeCalls; }
  };

  template <typename Predicate>
  void pumpUntil(
      Predicate&& predicate, int timeoutMs = 5000, std::source_location caller = std::source_location::current()
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      if (!g_main_context_iteration(nullptr, FALSE))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!predicate())
      g_printerr("Timed out at %s:%u\n", caller.file_name(), caller.line());
    TEST_CHECK(predicate());
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
  }

  void writeEmptyFile(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
  }

} // namespace

class FileManagerTestAccess {
public:
  static void waitReady(FileManager& manager, const std::filesystem::path& dir) {
    manager.navigate(dir, false);
    pumpUntil([&] {
      return !manager.activeSession().loading()
          && manager.activeSession().path() == dir
          && manager.activeTab().capabilities().canCreate
          && !manager.m_operations.active();
    });
    manager.activeSession().closeMonitor();
  }

  static void openHeadlessSymlinkDialog(FileManager& manager, StubRenderer& renderer) {
    auto& dialog = manager.m_modalDialog;
    auto handlers = manager.makeSymlinkDialogHandlers();
    dialog.m_presentToplevelForTest = [](auto&, auto*, auto) {};
    dialog.showSymlinkDialog(
        std::move(handlers.onSubmit), std::move(handlers.onBrowseTarget), std::move(handlers.onDismiss)
    );
    dialog.m_root->setSize(640.0F, 480.0F);
    dialog.m_root->layout(renderer);
  }

  static void clickBrowseButton(filemanager::ModalDialogHost& dialog, StubRenderer& renderer) {
    TEST_CHECK(dialog.m_symlinkController->m_symlinkBrowseButton != nullptr);
    dialog.m_root->layout(renderer);
    float absX = 0.0F;
    float absY = 0.0F;
    Node::absolutePosition(dialog.m_symlinkController->m_symlinkBrowseButton->inputArea(), absX, absY);
    const float clickX = absX + dialog.m_symlinkController->m_symlinkBrowseButton->inputArea()->width() * 0.5F;
    const float clickY = absY + dialog.m_symlinkController->m_symlinkBrowseButton->inputArea()->height() * 0.5F;
    TEST_CHECK(dialog.m_dispatcher.pointerButton(clickX, clickY, BTN_LEFT, true, 1, 10, false));
    TEST_CHECK(dialog.m_dispatcher.pointerButton(clickX, clickY, BTN_LEFT, false, 2, 20, false));
  }

  static void run(const nythera::test::ScopedTestEnv& env) {
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::stateDir(), env.root()));
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::configDir(), env.root()));
    TEST_CHECK(std::filesystem::path(g_get_home_dir()) == env.home());

    const auto work = env.work() / "workspace";
    const auto subDir = work / "sub_dir";
    const auto emptyDir = work / "empty_dir";
    const auto targetFile = subDir / "target.txt";
    const auto siblingDir = env.work() / "sibling";
    const auto siblingFile = siblingDir / "notes.md";
    const auto homeDocs = env.home() / "docs";
    const auto homeGuide = homeDocs / "guide.txt";

    for (const auto* dir : {&work, &subDir, &emptyDir, &siblingDir, &homeDocs})
      TEST_CHECK(std::filesystem::create_directory(*dir));
    writeEmptyFile(targetFile);
    writeEmptyFile(siblingFile);
    writeEmptyFile(homeGuide);

    // 1. Start-directory resolution for the Browse... target picker.
    TEST_CHECK(filemanager::resolveSymlinkTargetPickerStartDirectory("", work, env.home().c_str()) == work);
    TEST_CHECK(
        filemanager::resolveSymlinkTargetPickerStartDirectory(std::string_view("a\0b", 3), work, env.home().c_str())
        == work
    );
    TEST_CHECK(
        filemanager::resolveSymlinkTargetPickerStartDirectory(subDir.string(), work, env.home().c_str()) == subDir
    );
    TEST_CHECK(
        filemanager::resolveSymlinkTargetPickerStartDirectory(targetFile.string(), work, env.home().c_str()) == subDir
    );
    TEST_CHECK(filemanager::resolveSymlinkTargetPickerStartDirectory("sub_dir", work, env.home().c_str()) == subDir);
    TEST_CHECK(
        filemanager::resolveSymlinkTargetPickerStartDirectory("sub_dir/target.txt", work, env.home().c_str()) == subDir
    );
    {
      std::error_code ec;
      const auto resolvedSibling =
          filemanager::resolveSymlinkTargetPickerStartDirectory("../sibling/notes.md", work, env.home().c_str());
      TEST_CHECK(std::filesystem::equivalent(resolvedSibling, siblingDir, ec) && !ec);
    }
    TEST_CHECK(filemanager::resolveSymlinkTargetPickerStartDirectory("~/docs", work, env.home().c_str()) == homeDocs);
    TEST_CHECK(
        filemanager::resolveSymlinkTargetPickerStartDirectory("~/docs/guide.txt", work, env.home().c_str()) == homeDocs
    );
    TEST_CHECK(
        filemanager::resolveSymlinkTargetPickerStartDirectory("~/missing/nested/file", work, env.home().c_str()) == work
    );
    TEST_CHECK(
        filemanager::resolveSymlinkTargetPickerStartDirectory(
            (work / "nonexistent" / "file").string(), work, env.home().c_str()
        )
        == work
    );
    TEST_CHECK(filemanager::resolveSymlinkTargetPickerStartDirectory("~", work, nullptr) == work);
    TEST_CHECK(filemanager::resolveSymlinkTargetPickerStartDirectory("~", work, "") == work);

    StubPresenter presenter;
    FileDialog::setPresenter(&presenter);
    const auto presenterCleanup = nythera::test::makeScopeExit([] {
      FileDialog::cancelIfPending();
      FileDialog::setPresenter(nullptr);
    });

    FileManager manager(work.string());
    waitReady(manager, work);

    // Disconnected Wayland call exits safely without opening the picker.
    manager.showSymlinkDialog();
    TEST_CHECK(!manager.m_modalDialog.isOpen());
    TEST_CHECK(presenter.openCalls == 0);

    StubRenderer renderer;
    auto& dialog = manager.m_modalDialog;
    auto& ops = manager.m_operations;

    {
      TEST_CHECK(dialog.m_settings.saveDialogSize("conflict_dialog_size", 640, 620));
      dialog.m_initialLayoutRendererForTest = &renderer;
      std::pair<std::uint32_t, std::uint32_t> configured;
      dialog.m_presentToplevelForTest = [&](auto&, auto*, auto size) { configured = size; };
      dialog.m_conflictController->present(
          {.incoming = {.name = "incoming", .path = targetFile}, .existing = {.name = "existing", .path = targetFile}},
          false, nullptr, [](int, std::string) {}
      );
      TEST_CHECK(dialog.m_root && !dialog.m_spacer->participatesInLayout());
      auto* error = dialog.m_conflictController->m_alternativeNameError;
      TEST_CHECK(error && !error->visible());
      auto* scroll = findEnclosingScrollView(error);
      TEST_CHECK(scroll && scroll->viewportPaddingV() == 0.0F && scroll->viewportPaddingH() == 0.0F);
      TEST_CHECK(configured.first == 640 && configured.second > 260 && configured.second < 620);
      const float width = static_cast<float>(configured.first);
      const float height = static_cast<float>(configured.second);
      dialog.m_root->setSize(width, height);
      dialog.m_root->layout(renderer);
      TEST_CHECK(!scroll->scrollable());
      TEST_CHECK(dialog.m_footer->y() - (scroll->y() + scroll->height()) <= 11.0F);
      dialog.m_root->setSize(width, height - 40.0F);
      dialog.m_root->layout(renderer);
      TEST_CHECK(scroll->scrollable());
      dialog.forceCloseImmediate();
      dialog.m_initialLayoutRendererForTest = nullptr;
    }

    // 2. Dialog layout, Browse... opens SelectFileOrFolder, file selection populates field and clears errors.
    openHeadlessSymlinkDialog(manager, renderer);
    TEST_CHECK(
        dialog.m_symlinkController->m_symlinkName != nullptr && dialog.m_symlinkController->m_symlinkTarget != nullptr
    );
    TEST_CHECK(
        dialog.m_symlinkController->m_symlinkBrowseButton != nullptr
        && dialog.m_symlinkController->m_symlinkError != nullptr
    );
    TEST_CHECK(dialog.m_symlinkController->m_symlinkBrowseButton->label()->text() == "Browse…");
    TEST_CHECK(dialog.m_symlinkController->m_symlinkBrowseButton->variant() == ButtonVariant::Outline);
    TEST_CHECK(dialog.m_symlinkController->m_symlinkTarget->flexGrow() == 1.0F);
    TEST_CHECK(
        dialog.m_symlinkController->m_symlinkTarget->parent()
        == dialog.m_symlinkController->m_symlinkBrowseButton->parent()
    );
    const auto* targetRow = dynamic_cast<const Flex*>(dialog.m_symlinkController->m_symlinkTarget->parent());
    TEST_CHECK(targetRow != nullptr && targetRow->direction() == FlexDirection::Horizontal);

    // Trigger target validation error first, then clear it via Browse... selection.
    dialog.m_symlinkController->m_symlinkName->setValue("file-link");
    dialog.closeDialog(1);
    TEST_CHECK(!dialog.closeQueued());
    TEST_CHECK(dialog.m_symlinkController->m_symlinkTarget->invalid());
    TEST_CHECK(dialog.m_symlinkController->m_symlinkError->text() == "Enter a target path.");

    clickBrowseButton(dialog, renderer);
    TEST_CHECK(presenter.openCalls == 1);
    TEST_CHECK(presenter.lastOptions.has_value());
    TEST_CHECK(presenter.lastOptions->mode == FileDialogMode::SelectFileOrFolder);
    TEST_CHECK(presenter.lastOptions->startDirectory == work);
    TEST_CHECK(presenter.lastOptions->title == "Choose Symlink Target");

    FileDialog::complete(targetFile);
    TEST_CHECK(dialog.m_symlinkController->m_symlinkTarget->value() == targetFile.string());
    TEST_CHECK(!dialog.m_symlinkController->m_symlinkTarget->invalid());
    TEST_CHECK(dialog.m_symlinkController->m_symlinkError->text() == " ");
    TEST_CHECK(dialog.m_dispatcher.focusedArea() == dialog.m_symlinkController->m_symlinkTarget->inputArea());

    // Selecting a directory via Browse... uses the relative path's parent directory as startDirectory and populates it.
    dialog.m_symlinkController->m_symlinkTarget->setValue("sub_dir/target.txt");
    clickBrowseButton(dialog, renderer);
    TEST_CHECK(presenter.openCalls == 2);
    TEST_CHECK(presenter.lastOptions->mode == FileDialogMode::SelectFileOrFolder);
    TEST_CHECK(presenter.lastOptions->startDirectory == subDir);

    FileDialog::complete(emptyDir);
    TEST_CHECK(dialog.m_symlinkController->m_symlinkTarget->value() == emptyDir.string());
    dialog.closeDialog(1);
    TEST_CHECK(dialog.closeQueued());
    const auto dirLink = work / "file-link";
    pumpUntil([&] { return !dialog.isOpen() && !ops.operations().empty() && ops.operations().back().finished; });
    TEST_CHECK(ops.operations().back().succeeded);
    TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(dirLink)));
    TEST_CHECK(std::filesystem::read_symlink(dirLink) == emptyDir);

    // 3. Manual absolute, relative, and tilde target entry remain intact alongside validation.
    openHeadlessSymlinkDialog(manager, renderer);
    dialog.m_symlinkController->m_symlinkName->setValue("bad/name");
    dialog.closeDialog(1);
    TEST_CHECK(!dialog.closeQueued());
    TEST_CHECK(dialog.m_symlinkController->m_symlinkName->invalid());

    dialog.m_symlinkController->m_symlinkName->setValue("rel-link");
    dialog.m_symlinkController->m_symlinkTarget->setValue("sub_dir/target.txt");
    const std::size_t opCountBeforeRel = ops.operations().size();
    dialog.closeDialog(1);
    TEST_CHECK(dialog.closeQueued());
    pumpUntil([&] {
      return !dialog.isOpen() && ops.operations().size() == opCountBeforeRel + 1 && ops.operations().back().finished;
    });
    TEST_CHECK(ops.operations().back().succeeded);
    const auto relLink = work / "rel-link";
    TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(relLink)));
    TEST_CHECK(std::filesystem::read_symlink(relLink) == "sub_dir/target.txt");

    // Closing via closeDialog cancels any pending owned picker before scene teardown.
    openHeadlessSymlinkDialog(manager, renderer);
    clickBrowseButton(dialog, renderer);
    TEST_CHECK(presenter.openCalls == 3);
    TEST_CHECK(FileDialog::currentOptions().title == "Choose Symlink Target");
    dialog.closeDialog(0);
    pumpUntil([&] { return !dialog.isOpen(); });
    TEST_CHECK(FileDialog::currentOptions().title.empty());
    FileDialog::complete(targetFile); // Late result after close must be a safe no-op.

    // 4. Presenter failure shows non-destructive error message; forceCloseImmediate also dismisses pending picker.
    openHeadlessSymlinkDialog(manager, renderer);
    presenter.openResult = false;
    clickBrowseButton(dialog, renderer);
    TEST_CHECK(dialog.m_symlinkController->m_symlinkError->text() == "Could not open the target picker.");

    presenter.openResult = true;
    clickBrowseButton(dialog, renderer);
    TEST_CHECK(FileDialog::currentOptions().title == "Choose Symlink Target");
    dialog.forceCloseImmediate();
    TEST_CHECK(!dialog.isOpen());
    TEST_CHECK(FileDialog::currentOptions().title.empty());
    FileDialog::complete(targetFile);

    // 5. Selection-based symlink creation remains unchanged.
    waitReady(manager, work);
    auto& tab = manager.activeTab();
    tab.m_selectedPaths = {subDir.string()};
    const std::size_t opCountBeforeSel = ops.operations().size();
    manager.createSelectionSymlink();
    pumpUntil([&] { return ops.operations().size() == opCountBeforeSel + 1 && ops.operations().back().finished; });
    TEST_CHECK(ops.operations().back().succeeded);
    const auto selLink = work / "sub_dir-link";
    TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(selLink)));
    TEST_CHECK(std::filesystem::read_symlink(selLink) == subDir);
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-symlink-picker"});
    FileManagerTestAccess::run(env);
  });
}
