#include "core/files/display_text.h"
#include "filemanager/file_archive_actions.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/file_dialog_popup.h"
#include "ui/style.h"

#include <algorithm>
#include <array>

namespace filemanager {
  ArchiveSelection archiveSelection(const BrowserTab& tab, const std::vector<std::string>& paths) {
    ArchiveSelection result;
#ifdef NYTHERA_FILES_ARCHIVES
    const auto& session = tab.session;
    if (session.browsingArchive()
        || paths.empty()
        || tab.administratorBrowsing
        || tab.hasActiveAdminSession()
        || session.browsingTrash()
        || session.loading()
        || !session.path().is_absolute())
      return result;
    const auto available = archive_backend::capabilities();
    bool extraction = true;
    for (const auto& path : paths) {
      if (!std::filesystem::path(path).is_absolute())
        return {};
      const auto found = std::ranges::find(session.entries(), std::filesystem::path(path), &FileEntry::absPath);
      if (found == session.entries().end() || found->isSymlink || (!found->isDir && found->type.starts_with("inode/")))
        return {};
      const auto name = found->absPath.filename().native();
      const auto format = archive_backend::formatFromName(name);
      const auto capability =
          format ? std::ranges::find(available, *format, &archive_backend::Capability::format) : available.end();
      extraction &= !found->isDir && capability != available.end() && capability->extract;
      if (paths.size() == 1) {
        result.baseName = found->isDir ? name : archive_backend::archiveStem(name);
        result.canGzip = !found->isDir;
      }
    }
    if (paths.size() > 1)
      result.baseName = "Archive";
    for (const auto& capability : available) {
      if (!capability.create)
        continue;
      result.canCompress |= capability.format != archive_backend::Format::Gzip || result.canGzip;
      result.canZip |= capability.format == archive_backend::Format::Zip;
      result.canTarGzip |= capability.format == archive_backend::Format::TarGzip;
    }
    result.canExtract = extraction;
    result.canWriteHere = session.currentDirectoryWritable();
#else
    (void)tab;
    (void)paths;
#endif
    return result;
  }
} // namespace filemanager

void FileManager::navigateLocation(filemanager::BrowserLocation location, bool recordHistory) {
  if (!location.archiveDirectory) {
    navigate(location.path, recordHistory);
    return;
  }
  if (!activeTab().hasActiveAdminSession() && !activeTab().administratorBrowsing)
    activeSession().navigateLocation(std::move(location), recordHistory);
}

void FileManager::navigateBack() {
  auto& session = activeSession();
  if (session.backHistory().empty())
    return;
  auto path = session.backHistory().back();
  session.backHistory().pop_back();
  session.forwardHistory().push_back(session.location());
  navigateLocation(std::move(path), false);
}

void FileManager::navigateForward() {
  auto& session = activeSession();
  if (session.forwardHistory().empty())
    return;
  auto path = session.forwardHistory().back();
  session.forwardHistory().pop_back();
  session.backHistory().push_back(session.location());
  navigateLocation(std::move(path), false);
}

void FileManager::navigateUp() {
  const auto& session = activeSession();
  if (session.browsingArchive() && !session.archiveDirectory()->empty()) {
    navigateLocation({session.path(), std::filesystem::path(*session.archiveDirectory()).parent_path().native()});
  } else if (
      !(session.browsingTrash() && session.path() == std::filesystem::path("trash:/"))
      && session.path().has_parent_path()
      && session.path().parent_path() != session.path()
  ) {
    navigate(session.path().parent_path());
  }
}

void FileManager::materialiseArchiveSelection(
    std::vector<std::string> members, std::function<void(const FileOperationManager::Operation&)> ready
) {
  const auto catalogue = activeSession().archiveCatalogue();
  if (!catalogue || members.empty())
    return;
  g_autoptr(GError) error = nullptr;
  g_autofree gchar* path = g_dir_make_tmp("nythera-archive-members-XXXXXX", &error);
  if (!path) {
    setStatus(error ? error->message : "Could not create a temporary archive directory.");
    return;
  }
  const auto temporary = std::shared_ptr<const std::filesystem::path>(
      new std::filesystem::path(path), [](const std::filesystem::path* owned) {
        std::error_code cleanupError;
        std::filesystem::remove_all(*owned, cleanupError);
        delete owned;
      }
  );
  const std::weak_ptr<bool> alive = m_alive;
  if (!m_operations.extractSelected(
          {catalogue, std::move(members)}, temporary->native(), activeSession().archivePassword(),
          [this, alive, temporary, ready = std::move(ready)](const auto& operation) {
            const auto life = alive.lock();
            if (!life || !*life || !operation.succeeded || operation.completedEntries.empty())
              return;
            m_archiveTemporary.push_back(temporary);
            ready(operation);
          }
      ))
    setStatus("Could not start extracting the archive selection.");
}

void FileManager::openArchiveMember(const std::filesystem::path& member, bool withApplication) {
  const auto catalogue = activeSession().archiveCatalogue();
  if (!catalogue)
    return;
  const auto found = catalogue->entries.find(member.native());
  if (found == catalogue->entries.end())
    return;
  using filemanager::archive_backend::EntryKind;
  if (found->second.kind == EntryKind::Directory || found->second.kind == EntryKind::Symlink)
    return;
  const auto tabId = activeTab().id;
  const auto navigation = activeSession().navigationGeneration();
  materialiseArchiveSelection({member.native()}, [this, tabId, navigation, withApplication](const auto& operation) {
    const auto* tab = m_tabs.findTab(tabId);
    if (!tab || tab->session.navigationGeneration() != navigation)
      return;
    const auto& path = operation.completedEntries.front().path;
    if (withApplication) {
      if (tabId == m_tabs.activeTabId()) {
        m_menus.setContextMenuTarget(path);
        m_launcher.openApplicationChooser(path);
      }
    } else {
      g_autoptr(GFile) file = g_file_new_for_path(path.c_str());
      m_launcher.launchFileUri(tabId, file);
    }
  });
}

void FileManager::copyArchiveMembers() {
  const auto request = ++m_archiveCopyGeneration;
  const auto generation = m_transfer.selectionGeneration();
  const auto serial = m_wayland.lastInputSerial();
  materialiseArchiveSelection(selectedPaths(), [this, request, generation, serial](const auto& operation) {
    if (request != m_archiveCopyGeneration || generation != m_transfer.selectionGeneration())
      return;
    std::vector<std::string> paths;
    for (const auto& entry : operation.completedEntries)
      paths.push_back(entry.path);
    try {
      if (!m_transfer.setSelection(filemanager::encodeFileTransfer(std::move(paths), false), serial))
        setStatus("Could not publish the archive selection to the clipboard.");
    } catch (const std::exception& error) {
      setStatus(error.what());
    }
  });
}

void FileManager::extractArchiveMembersTo() {
  auto catalogue = activeSession().archiveCatalogue();
  auto members = selectedPaths();
  if (!catalogue || members.empty())
    return;
  const std::weak_ptr<bool> alive = m_alive;
  const auto tabId = activeTab().id;
  const auto navigation = activeSession().navigationGeneration();
  const auto startDirectory = catalogue->source.parent_path();
  if (!FileDialog::open(
          {.mode = FileDialogMode::SelectFolder, .startDirectory = startDirectory, .title = "Extract Selected…"},
          [this, alive, tabId, navigation,
           selection = filemanager::archive_backend::Selection{std::move(catalogue), std::move(members)}](
              auto destination
          ) mutable {
            const auto life = alive.lock();
            if (!life || !*life || !destination)
              return;
            const auto* tab = m_tabs.findTab(tabId);
            if (!tab || tab->session.navigationGeneration() != navigation)
              return;
            if (!m_operations.extractSelected(
                    std::move(selection), destination->native(), tab->session.archivePassword()
                ))
              setStatus("Could not start extracting the archive selection.");
          }
      ))
    setStatus("Could not open the destination picker.");
}

void FileManager::showArchiveMemberProperties(const std::filesystem::path& member) {
  const auto catalogue = activeSession().archiveCatalogue();
  if (!catalogue)
    return;
  const auto found = catalogue->entries.find(member.native());
  if (found == catalogue->entries.end())
    return;
  using filemanager::archive_backend::EntryKind;
  const auto& entry = found->second;
  const char* type = entry.kind == EntryKind::Directory ? "Folder"
      : entry.kind == EntryKind::Symlink                ? "Symbolic link"
      : entry.kind == EntryKind::Hardlink               ? "Hard link"
                                                        : "File";
  std::string summary = "Type: ";
  summary += type;
  if (entry.size)
    summary += "\nSize: " + std::to_string(*entry.size) + " bytes";
  if (!entry.link.empty())
    summary += "\nTarget: " + filemanager::sanitizeFilenameForDisplay(entry.link);
  if (entry.modified) {
    g_autoptr(GDateTime) stamp = g_date_time_new_from_unix_local(*entry.modified);
    if (stamp) {
      g_autofree gchar* formatted = g_date_time_format(stamp, "%x %X");
      if (formatted)
        summary += "\nModified: " + std::string(formatted);
    }
  }
  m_modalDialog.showDialog(
      filemanager::sanitizeFilenameForDisplay(member.filename().native()), std::move(summary), {{"Close", 0}}, {}
  );
}

bool FileManager::activateArchiveAction(int id) {
  using namespace filemanager::menu_id;
  if (id == kExtractSelected) {
    if (activeSession().browsingArchive())
      extractArchiveMembersTo();
    return true;
  }
  if (id == kOpenArchiveMemberWith) {
    if (activeSession().browsingArchive() && activeTab().selectedPath())
      openArchiveMember(*activeTab().selectedPath(), true);
    return true;
  }
  if (id < kArchiveCompress || id > kAvoidRedundantExtractionFolders)
    return false;
#ifdef NYTHERA_FILES_ARCHIVES
  using namespace filemanager::archive_backend;
  if (id == kAvoidRedundantExtractionFolders) {
    if (!m_settings.setAvoidRedundantExtractionFolders(!m_settings.avoidRedundantExtractionFolders()))
      setStatus("Could not save the extraction preference.");
    return true;
  }
  if (m_places.hasActiveDeviceOperations() || m_operations.replaying())
    return true;
  auto sources = selectedPaths();
  const auto selection = filemanager::archiveSelection(activeTab(), sources);
  const auto directory = activeSession().path();
  if (id == kCompressTo && selection.canCompress) {
    showCompressDialog(std::move(sources), selection.baseName, selection.canGzip);
  } else if ((id == kCompressTarGzip || id == kCompressZip) && selection.canCompress && selection.canWriteHere) {
    if (id == kCompressZip ? !selection.canZip : !selection.canTarGzip)
      return true;
    const auto format = id == kCompressZip ? Format::Zip : Format::TarGzip;
    const auto output = directory / (selection.baseName + std::string(extension(format)));
    m_operations.compress(std::move(sources), output.native(), format, Level::Balanced);
  } else if ((id == kExtractHere || id == kExtractFolder) && selection.canExtract && selection.canWriteHere) {
    const auto mode = id == kExtractHere               ? Extraction::Direct
        : m_settings.avoidRedundantExtractionFolders() ? Extraction::SmartFolder
                                                       : Extraction::Folder;
    for (auto& source : sources)
      m_operations.extract(std::move(source), directory.native(), mode);
  } else if (id == kExtractTo && selection.canExtract) {
    const std::weak_ptr<bool> alive = m_alive;
    const auto tabId = activeTab().id;
    const auto navigation = activeSession().navigationGeneration();
    if (!FileDialog::open(
            {.mode = FileDialogMode::SelectFolder, .startDirectory = directory, .title = "Extract To…"},
            [this, alive, tabId, navigation, sources = std::move(sources)](auto destination) mutable {
              const auto life = alive.lock();
              if (!life || !*life || !m_windowOpen || !destination)
                return;
              const auto* tab = m_tabs.findTab(tabId);
              if (!tab || tab->session.navigationGeneration() != navigation || tab->hasActiveAdminSession())
                return;
              for (auto& source : sources)
                m_operations.extract(std::move(source), destination->native());
            }
        ))
      setStatus("Could not open the destination picker.");
  }
#endif
  return true;
}

void FileManager::showCompressDialog(std::vector<std::string> sources, const std::string& baseName, bool canGzip) {
#ifdef NYTHERA_FILES_ARCHIVES
  using namespace filemanager::archive_backend;
  if (m_modalDialog.isOpen())
    return;
  struct Form {
    Format format = Format::TarGzip;
    Method method = Method::Default;
    Level level = Level::Balanced;
    bool open = true;
    bool settingsOpen = false;
    bool passwordsOpen = false;
    Input *destination = nullptr, *name = nullptr, *password = nullptr, *confirmation = nullptr;
    Label *suffix = nullptr, *error = nullptr;
    Flex *methods = nullptr, *levels = nullptr, *passwords = nullptr;
    ScrollView* scroll = nullptr;
    Button *settingsToggle = nullptr, *passwordToggle = nullptr;
    Select* methodSelect = nullptr;
    std::vector<Method> methodOptions;
    std::vector<std::pair<Level, Button*>> levelButtons;
    std::string output;
    EncryptionOptions options;
  };
  const auto form = std::make_shared<Form>();
  constexpr std::array formatNames{
      std::pair{Format::TarGzip, "TAR.GZ"},   std::pair{Format::Zip, "ZIP"},      std::pair{Format::SevenZip, "7z"},
      std::pair{Format::Tar, "TAR"},          std::pair{Format::TarXz, "TAR.XZ"}, std::pair{Format::TarZstd, "TAR.ZST"},
      std::pair{Format::TarBzip2, "TAR.BZ2"}, std::pair{Format::Gzip, "Gzip"},
  };
  const auto available = capabilities();
  const auto writable = [available, canGzip](Format format) {
    const auto it = std::ranges::find(available, format, &Capability::format);
    return it != available.end() && it->create && (format != Format::Gzip || canGzip);
  };
  if (!writable(form->format)) {
    const auto it = std::ranges::find_if(formatNames, [&](const auto& item) { return writable(item.first); });
    if (it == formatNames.end())
      return;
    form->format = it->first;
  }
  auto sortedFormats = formatNames;
  std::ranges::sort(sortedFormats, {}, [](const auto& item) { return std::string_view(item.second); });
  const auto initial = activeSession().path();
  const auto tabId = activeTab().id;
  const auto navigation = activeSession().navigationGeneration();
  const auto setShown = [](Node* node, bool shown) {
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  };
  const auto encryptionAvailable = [](Format format) {
    return format == Format::Zip ? canEncryptZip() : format == Format::SevenZip && sevenZipProgram().has_value();
  };
  const auto showError = [this, form, setShown](std::string_view message) {
    form->error->setText(message);
    setShown(form->error, !message.empty());
    m_modalDialog.requestLayout();
  };
  const auto fittedHeight = [form](float height) {
    return height - form->scroll->contentViewportHeight() + form->scroll->content()->height();
  };
  const auto refreshMethods = [form] {
    constexpr std::array methodNames{
        std::pair{Method::Copy, "Copy"},   std::pair{Method::Deflate, "Deflate"}, std::pair{Method::Lzma, "LZMA"},
        std::pair{Method::Lzma2, "LZMA2"}, std::pair{Method::Store, "Store"},
    };
    std::vector<std::string> options;
    form->methodOptions.clear();
    if (form->format == Format::Zip || form->format == Format::SevenZip) {
      for (const auto& [method, label] : methodNames) {
        if (canWriteMethod(form->format, method)) {
          form->methodOptions.push_back(method);
          options.emplace_back(label);
        }
      }
    }
    form->methodSelect->setOptions(std::move(options));
    const auto defaultMethod = form->format == Format::Zip ? Method::Deflate : Method::Lzma2;
    const auto selected = std::ranges::find(form->methodOptions, defaultMethod);
    if (selected != form->methodOptions.end())
      form->methodSelect->setSelectedIndexSilently(selected - form->methodOptions.begin());
    else
      form->methodSelect->clearSelection();
  };
  const auto sync = [this, form, setShown, encryptionAvailable] {
    form->suffix->setText(extension(form->format));
    setShown(form->methods, form->settingsOpen && !form->methodOptions.empty());
    setShown(form->levels, form->settingsOpen);
    const bool usesLevel = form->format != Format::Tar && form->method != Method::Store && form->method != Method::Copy;
    for (const auto& [level, button] : form->levelButtons) {
      button->setEnabled(usesLevel);
      button->setVariant(form->level == level ? ButtonVariant::Primary : ButtonVariant::Outline);
    }
    const bool encryption = encryptionAvailable(form->format);
    form->passwordToggle->setEnabled(encryption);
    setShown(form->passwords, encryption && form->passwordsOpen);
    form->settingsToggle->setGlyph(form->settingsOpen ? "chevron-down" : "chevron-right");
    form->passwordToggle->setGlyph(form->passwordsOpen ? "chevron-down" : "chevron-right");
    m_modalDialog.requestLayout();
  };
  m_modalDialog.showDialog(
      "Compress To…", {}, {{"Cancel", 0, filemanager::DialogChoiceStyle::Cancel}, {"Compress", 1}},
      [this, form, sources = std::move(sources), tabId, navigation](int result) mutable {
        const auto* tab = m_tabs.findTab(tabId);
        if (result == 1
            && m_windowOpen
            && tab
            && tab->session.navigationGeneration() == navigation
            && !tab->hasActiveAdminSession())
          m_operations.compress(
              std::move(sources), std::move(form->output), form->format, form->level, std::move(form->options)
          );
      },
      m_surface.get(),
      filemanager::ModalDialogHost::CustomContent{
          .buildFields =
              [=, this](Flex& root, float width) {
                auto scroll = std::make_unique<ScrollView>();
                scroll->setFlexGrow(1);
                scroll->setViewportPaddingH(0);
                scroll->setViewportPaddingV(0);
                form->scroll = scroll.get();
                auto& content = *scroll->content();
                content.setGap(Style::spaceSm);
                content.setAlign(FlexAlign::Stretch);
                auto label = [](Flex& box, std::string_view text) {
                  auto node = std::make_unique<Label>();
                  node->setText(text);
                  return static_cast<Label*>(box.addChild(std::move(node)));
                };
                auto row = [](Flex& box, bool wrap = false) {
                  auto node = std::make_unique<Flex>();
                  node->setAlign(FlexAlign::Center);
                  node->setGap(Style::spaceXs);
                  node->setWrap(wrap);
                  return static_cast<Flex*>(box.addChild(std::move(node)));
                };
                auto input = [](Flex& box, std::string_view value, std::string_view placeholder, bool secret = false) {
                  auto node = std::make_unique<Input>();
                  node->setPasswordMode(secret);
                  node->setPlaceholder(placeholder);
                  node->setValue(value);
                  return static_cast<Input*>(box.addChild(std::move(node)));
                };
                auto button = [](Flex& box, std::string_view text, std::function<void()> action) {
                  auto node = std::make_unique<Button>();
                  node->setText(text);
                  node->setControlHeight(Style::controlHeightSm);
                  node->setVariant(ButtonVariant::Outline);
                  node->setOnClick(std::move(action));
                  return static_cast<Button*>(box.addChild(std::move(node)));
                };
                const auto field = [&](std::string_view title) -> Flex& {
                  auto group = std::make_unique<Flex>();
                  group->setDirection(FlexDirection::Vertical);
                  group->setAlign(FlexAlign::Stretch);
                  group->setGap(Style::spaceXs);
                  label(*group, title);
                  return *static_cast<Flex*>(content.addChild(std::move(group)));
                };
                auto& destRow = *row(field("Destination"));
                form->destination = input(destRow, initial.native(), "Destination folder");
                form->destination->setFlexGrow(1);
                auto* browse = button(destRow, "Browse…", [form, initial, showError] {
                  const std::filesystem::path current(form->destination->value());
                  if (!FileDialog::open(
                          {.mode = FileDialogMode::SelectFolder,
                           .startDirectory = current.is_absolute() ? current : initial,
                           .title = "Choose archive destination"},
                          [form](auto location) {
                            if (form->open && location)
                              form->destination->setValue(location->native());
                          }
                      ))
                    showError("Could not open the destination picker.");
                });
                browse->setControlHeight(Style::controlHeight);
                auto& nameRow = *row(field("Archive name"));
                form->name = input(nameRow, filemanager::sanitizeFilenameForDisplay(baseName), "Archive name");
                form->name->setFlexGrow(1);
                form->suffix = label(nameRow, {});
                auto formatSelect = std::make_unique<Select>();
                std::vector<std::string> options;
                std::vector<Format> formats;
                for (const auto& [format, title] : sortedFormats) {
                  if (!writable(format))
                    continue;
                  formats.push_back(format);
                  options.emplace_back(title);
                }
                formatSelect->setOptions(std::move(options));
                formatSelect->setSelectedIndexSilently(std::ranges::find(formats, form->format) - formats.begin());
                formatSelect->setOnSelectionChanged([form, formats = std::move(formats), refreshMethods,
                                                     sync](std::size_t index, auto) {
                  const auto format = formats[index];
                  if (form->format == format)
                    return;
                  form->format = format;
                  form->method = Method::Default;
                  form->password->setValue("");
                  form->confirmation->setValue("");
                  form->passwordsOpen = false;
                  refreshMethods();
                  sync();
                });
                field("Format").addChild(std::move(formatSelect));
                form->settingsToggle = button(*row(content), "Compression settings", [form, sync] {
                  form->settingsOpen = !form->settingsOpen;
                  sync();
                });
                form->settingsToggle->setContentAlign(ButtonContentAlign::Start);
                form->methods = &field("Compression method");
                auto methodSelect = std::make_unique<Select>();
                methodSelect->setOnSelectionChanged([form, sync](std::size_t index, auto) {
                  form->method = form->methodOptions[index];
                  sync();
                });
                form->methodSelect = static_cast<Select*>(form->methods->addChild(std::move(methodSelect)));
                refreshMethods();
                form->levels = &field("Compression level");
                auto& levelRow = *row(*form->levels, true);
                for (const auto& [level, title] : std::array{
                         std::pair{Level::Fast, "Fast"}, std::pair{Level::Balanced, "Normal"},
                         std::pair{Level::Maximum, "Maximum"}
                     }) {
                  form->levelButtons.emplace_back(level, button(levelRow, title, [form, level, sync] {
                                                    form->level = level;
                                                    sync();
                                                  }));
                }
                form->passwordToggle = button(*row(content), "Password protection", [form, sync] {
                  form->passwordsOpen = !form->passwordsOpen;
                  if (!form->passwordsOpen) {
                    form->password->setValue("");
                    form->confirmation->setValue("");
                  }
                  sync();
                });
                form->passwordToggle->setContentAlign(ButtonContentAlign::Start);
                auto passwords = std::make_unique<Flex>();
                passwords->setDirection(FlexDirection::Vertical);
                passwords->setAlign(FlexAlign::Stretch);
                passwords->setGap(Style::spaceXs);
                form->passwords = static_cast<Flex*>(content.addChild(std::move(passwords)));
                form->password = input(*form->passwords, {}, "Password", true);
                form->confirmation = input(*form->passwords, {}, "Confirm password", true);
                form->error = label(content, {});
                form->error->setColor(colorSpecFromRole(ColorRole::Error));
                form->error->setMaxWidth(width - 56);
                form->error->setMaxLines(2);
                setShown(form->error, false);
                root.addChild(std::move(scroll));
                sync();
              },
          .bindChoiceButton =
              [](int result, Button* button) {
                if (result == 1)
                  button->setVariant(ButtonVariant::Primary);
              },
          .initialFocus = [form] { return form->name ? form->name->inputArea() : nullptr; },
          .onPrepareLayout =
              [this, fittedHeight](Renderer&) {
                m_modalDialog.setCompactHeight(fittedHeight(static_cast<float>(m_modalDialog.surface()->height())));
              },
          .onInitialLayout =
              [fittedHeight](Renderer&, float height, std::optional<float>& compactHeight) {
                compactHeight = fittedHeight(height);
                return true;
              },
          .interceptResponse =
              [form, encryptionAvailable, showError](int result) {
                if (result != 1)
                  return false;
                const auto reject = [&](std::string_view message) {
                  showError(message);
                  return true;
                };
                if (!form->destination || !form->name)
                  return true;
                const std::filesystem::path destination(form->destination->value());
                auto name = form->name->value();
                const auto suffix = extension(form->format);
                if (name.ends_with(suffix))
                  name.resize(name.size() - suffix.size());
                if (!destination.is_absolute())
                  return reject("Choose an absolute destination folder.");
                if (!filemanager::validFileName(name) || name.contains('\\'))
                  return reject("Enter a valid archive name.");
                const auto& password = form->password->value();
                if (form->passwordsOpen && encryptionAvailable(form->format)) {
                  if (password.empty() || password != form->confirmation->value())
                    return reject("Enter matching passwords.");
                  if (form->format == Format::SevenZip && password.find_first_of("\r\n") != std::string::npos)
                    return reject("7z passwords cannot contain line breaks.");
                  form->options.password = password;
                }
                form->options.method = form->method;
                form->output = (destination / (name + std::string(suffix))).native();
                return false;
              },
          .onDismiss =
              [this, form] {
                form->open = false;
                if (form->password) {
                  form->password->setValue("");
                  form->confirmation->setValue("");
                }
                if (m_destinationPicker && m_destinationPicker->isOpen())
                  m_destinationPicker->closeFileDialogWithoutResult();
              },
          .fillContent = true,
          .usePreferredInitialHeight = true,
          .headerCloseButton = true,
          .compositorBorder = true,
          .compactHeightMode = true,
          .selectDropdowns = true,
          .rememberSize = false,
          .preferredWidth = 480,
          .preferredHeight = 640,
          .minWidth = 420,
      }
  );
#else
  (void)sources;
  (void)baseName;
  (void)canGzip;
#endif
}
