#include "core/input/key_symbols.h"
#include "filemanager/custom_search_editor_state.h"
#include "filemanager/file_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_dialog.h"

#include <algorithm>

namespace {
  using filemanager::CustomSearch;
  using filemanager::SearchLocations;

  struct Editor {
    filemanager::CustomSearchEditorState locations;
    bool completingLine = false;
    Input* name = nullptr;
    Input* included = nullptr;
    Input* excluded = nullptr;
    Flex* rootOption = nullptr;
    Checkbox* mounts = nullptr;
    Checkbox* showHidden = nullptr;
    Checkbox* followLinks = nullptr;
    Label* warning = nullptr;
    Label* error = nullptr;
    Button* save = nullptr;
    bool alive = true;
    bool picker = false;
    void setError(std::string_view message) {
      error->setText(message);
      error->setVisible(!message.empty());
      error->setParticipatesInLayout(!message.empty());
    }
    std::string savedName;
  };

  Label* addLabel(Flex& parent, std::string_view text) {
    auto label = std::make_unique<Label>();
    label->setText(text);
    label->setFontSize(Style::fontSizeBody);
    return static_cast<Label*>(parent.addChild(std::move(label)));
  }

  std::string locationText(const SearchLocations& paths) {
    std::string text;
    for (const auto& path : paths) {
      if (!text.empty())
        text += '\n';
      text += path.native();
    }
    return text;
  }
} // namespace

void FileManager::editCustomSearch(std::optional<CustomSearch> profile, std::function<void()> refreshParent) {
  auto& host = refreshParent ? m_customSearchChild : m_modalDialog;
  auto* parent = refreshParent ? m_modalDialog.surface() : nullptr;
  const auto origin = activeTab().id;
  const bool selectSaved = !profile || activeSession().searchSelection().profile == profile->name;
  const auto editor = std::make_shared<Editor>();
  const auto refresh = [&host, editor] {
    const auto included = filemanager::searchLocationLines(editor->included->value());
    const auto excluded = filemanager::searchLocationLines(editor->excluded->value());
    const bool root = editor->locations.hasCompletedRoot();
    if (!root)
      editor->mounts->setChecked(false);
    editor->rootOption->setVisible(root);
    editor->rootOption->setParticipatesInLayout(root);
    editor->warning->setText(
        editor->mounts->checked()
            ? "Searching the root filesystem and mounted local and network storage may take significantly longer."
            : "Searching the root filesystem may take significantly longer."
    );
    editor->save->setEnabled(
        filemanager::validCustomSearchName(filemanager::trimSearchText(editor->name->value()))
        && !included.empty()
        && std::ranges::all_of(included, filemanager::validSearchLocation)
        && std::ranges::all_of(excluded, filemanager::validSearchLocation)
    );
    editor->setError("");
    host.requestLayout();
  };

  const auto browse = [&host, editor, refresh, start = activeSession().path()](bool exclusions) {
    if (editor->picker || host.closeQueued())
      return;
    editor->picker = true;
    FileDialogOptions options{
        .mode = FileDialogMode::SelectFolder,
        .startDirectory = start,
        .title = exclusions ? "Choose an Excluded Folder" : "Choose an Included Folder"
    };
    if (!FileDialog::open(std::move(options), [&host, editor, refresh, exclusions](auto path) {
          editor->picker = false;
          if (!editor->alive || host.closeQueued() || !path)
            return;
          if (path->native().find('\n') != std::string::npos) {
            editor->setError("The folder path contains a line break.");
            host.requestLayout();
            return;
          }
          auto* field = exclusions ? editor->excluded : editor->included;
          auto value = field->value();
          if (!value.empty() && !value.ends_with('\n'))
            value += '\n';
          value += path->native();
          if (!exclusions)
            value += '\n';
          field->setValue(value);
          if (!exclusions) {
            const auto completedLine = static_cast<std::size_t>(std::ranges::count(value, '\n') - 1);
            editor->locations.edit(value, value.size(), completedLine);
          }
          refresh();
        })) {
      editor->picker = false;
      editor->setError("Could not open the folder picker.");
      host.requestLayout();
    }
  };

  filemanager::ModalDialogHost::CustomContent content{
      .buildFields =
          [editor, profile, browse, refresh](Flex& dialog, float) {
            auto scroll = std::make_unique<ScrollView>();
            scroll->setFlexGrow(1.0F);
            auto& root = *scroll->content();
            root.setAlign(FlexAlign::Stretch);
            root.setGap(Style::spaceSm);
            const auto field = [&root](std::string_view label) {
              auto container = std::make_unique<Flex>();
              container->setDirection(FlexDirection::Vertical);
              container->setAlign(FlexAlign::Stretch);
              container->setGap(Style::spaceXs);
              addLabel(*container, label);
              return static_cast<Flex*>(root.addChild(std::move(container)));
            };
            auto* nameField = field("Search name");
            auto name = std::make_unique<Input>();
            editor->name = name.get();
            if (profile)
              name->setValue(profile->name);
            name->setOnChange([refresh](const auto&) { refresh(); });
            nameField->addChild(std::move(name));
            const auto addPaths = [&](std::string_view label, bool exclusions) {
              auto* pathsField = field(label);
              auto row = std::make_unique<Flex>();
              row->setGap(Style::spaceSm);
              row->setAlign(FlexAlign::Start);
              auto input = std::make_unique<Input>();
              input->setMultiline(true);
              input->setSize(0.0F, 155.0F);
              input->setFlexGrow(1.0F);
              input->setPlaceholder("One absolute folder path per line");
              if (profile)
                input->setValue(locationText(exclusions ? profile->excluded : profile->included));
              if (exclusions) {
                input->setOnChange([refresh](const auto&) { refresh(); });
              } else {
                editor->locations.load(input->value());
                input->setOnKeyEvent([editor](std::uint32_t key, std::uint32_t modifiers) {
                  editor->completingLine = KeySymbol::isEnter(key) && !(modifiers & KeyMod::Ctrl);
                  return false;
                });
                input->setOnChange([editor, refresh](const std::string& value) {
                  std::optional<std::size_t> completedLine;
                  const auto cursor = static_cast<std::size_t>(editor->included->textInputState().cursor);
                  if (std::exchange(editor->completingLine, false)) {
                    const auto before = std::string_view(value).substr(0, cursor);
                    if (before.ends_with('\n'))
                      completedLine = static_cast<std::size_t>(std::ranges::count(before, '\n') - 1);
                  }
                  editor->locations.edit(value, cursor, completedLine);
                  refresh();
                });
              }
              (exclusions ? editor->excluded : editor->included) = input.get();
              row->addChild(std::move(input));
              auto button = std::make_unique<Button>();
              button->setVariant(ButtonVariant::Outline);
              button->setText("Browse…");
              button->setOnClick([browse, exclusions] { browse(exclusions); });
              row->addChild(std::move(button));
              pathsField->addChild(std::move(row));
            };
            addPaths("Included locations", false);
            auto preferences = std::make_unique<Flex>();
            preferences->setGap(Style::spaceMd);
            preferences->setWrap(true);
            const auto addCheckbox = [&](std::string_view label, bool checked) {
              auto row = std::make_unique<Flex>();
              row->setAlign(FlexAlign::Center);
              row->setGap(Style::spaceSm);
              auto checkbox = std::make_unique<Checkbox>();
              checkbox->setChecked(checked);
              auto* control = static_cast<Checkbox*>(row->addChild(std::move(checkbox)));
              addLabel(*row, label);
              preferences->addChild(std::move(row));
              return control;
            };
            editor->showHidden = addCheckbox("Show hidden items", profile && profile->options.showHidden);
            editor->followLinks = addCheckbox("Follow symbolic links", profile && profile->options.followSymlinks);
            root.addChild(std::move(preferences));
            auto option = std::make_unique<Flex>();
            option->setDirection(FlexDirection::Vertical);
            option->setGap(Style::spaceSm);
            option->setAlign(FlexAlign::Stretch);
            editor->rootOption = option.get();
            auto row = std::make_unique<Flex>();
            row->setAlign(FlexAlign::Center);
            row->setGap(Style::spaceSm);
            auto checkbox = std::make_unique<Checkbox>();
            editor->mounts = checkbox.get();
            checkbox->setChecked(profile && profile->includeMounts);
            checkbox->setOnChange([refresh](bool) { refresh(); });
            row->addChild(std::move(checkbox));
            addLabel(*row, "Include mounted filesystems");
            option->addChild(std::move(row));
            auto notice = std::make_unique<Flex>();
            notice->setAlign(FlexAlign::Start);
            notice->setGap(Style::spaceSm);
            const auto warningColor = colorSpecFromRole(ColorRole::Error);
            auto icon = std::make_unique<Glyph>();
            icon->setGlyph("alert-triangle");
            icon->setGlyphSize(Style::fontSizeTitle);
            icon->setColor(warningColor);
            notice->addChild(std::move(icon));
            editor->warning = addLabel(*notice, "");
            editor->warning->setFlexGrow(1.0F);
            editor->warning->setColor(warningColor);
            option->addChild(std::move(notice));
            root.addChild(std::move(option));
            addPaths("Additional exclusions", true);
            addLabel(root, "Global search exclusions also apply.");
            editor->error = addLabel(root, "");
            editor->error->setColor(colorSpecFromRole(ColorRole::Error));
            editor->error->setMaxLines(2);
            dialog.addChild(std::move(scroll));
          },
      .bindChoiceButton =
          [editor](int result, Button* button) {
            if (result == 1)
              editor->save = button;
          },
      .afterBuild = refresh,
      .initialFocus = [editor] { return editor->name->inputArea(); },
      .interceptResponse =
          [this, &host, editor, profile](int result) {
            if (result != 1)
              return false;
            if (!editor->save || !editor->save->enabled())
              return true;
            CustomSearch replacement{
                filemanager::trimSearchText(editor->name->value()),
                filemanager::searchLocationLines(editor->included->value()),
                filemanager::searchLocationLines(editor->excluded->value()),
                false,
                {editor->showHidden->checked(), editor->followLinks->checked()},
            };
            replacement.includeMounts =
                filemanager::CustomSearchEditorState::saveWithMounts(replacement.included, editor->mounts->checked());
            if (!m_settings.editCustomSearch(profile, replacement)) {
              editor->setError(
                  "Could not save the search. Check that its name is unique and the configuration is writable."
              );
              host.requestLayout();
              return true;
            }
            editor->savedName = std::move(replacement.name);
            return false;
          },
      .onDismiss =
          [this, editor] {
            editor->alive = false;
            if (std::exchange(editor->picker, false)) {
              if (m_destinationPicker && m_destinationPicker->isOpen())
                m_destinationPicker->closeFileDialogWithoutResult();
              FileDialog::cancelIfPending();
            }
          },
      .fillContent = true,
      .usePreferredInitialHeight = true,
      .preferredWidth = 690,
      .preferredHeight = 680,
      .minWidth = 420,
      .minHeight = 320,
      .sizeStateKey = "custom_search_editor",
  };
  host.showDialog(
      profile ? "Edit Custom Search" : "Define Custom Search", "", {{"Cancel", 0}, {"Save", 1}},
      [this, editor, origin, selectSaved, refreshParent = std::move(refreshParent)](int result) {
        if (result == 1 && refreshParent)
          refreshParent();
        if (result == 1 && selectSaved && m_windowOpen && activeTab().id == origin)
          selectSearchScope({.scope = filemanager::SearchScope::Custom, .profile = std::move(editor->savedName)});
      },
      parent, false, false, std::move(content)
  );
}
