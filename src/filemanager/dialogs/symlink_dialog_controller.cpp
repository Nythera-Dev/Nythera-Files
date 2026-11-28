#include "filemanager/dialogs/symlink_dialog_controller.h"

#include "filemanager/file_interaction.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"

#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace filemanager {
  void SymlinkDialogController::show(
      std::function<std::optional<SymlinkError>(const std::string&, const std::string&)> onSubmit,
      std::function<void(const std::string&)> onBrowseTarget, std::function<void()> onDismiss
  ) {
    if (m_host.isOpen())
      return;
    m_symlinkSubmitCallback = std::move(onSubmit);
    m_symlinkBrowseCallback = std::move(onBrowseTarget);
    m_symlinkDismissCallback = std::move(onDismiss);
    m_host.showDialog(
        "Create Symlink", "Enter a name and target path. Relative targets are resolved from this folder.",
        {{"Cancel", 0, DialogChoiceStyle::Cancel}, {"Create Symlink", 1}}, [this](int) { dismiss(); }, nullptr,
        ModalDialogHost::CustomContent{
            .buildFields = [this](Flex& root, float width) { buildFields(root, width); },
            .initialFocus = [this] { return m_symlinkName->inputArea(); },
            .onKeyEvent = [this](const KeyboardEvent& event) { return onKeyEvent(event); },
            .interceptResponse = [this](int response) { return response == 1 && !submit(); },
            .onDismiss = [this] { dismiss(); },
        }
    );
  }

  void SymlinkDialogController::dismiss() {
    if (auto callback = std::exchange(m_symlinkDismissCallback, nullptr))
      callback();
    m_symlinkSubmitCallback = nullptr;
    m_symlinkBrowseCallback = nullptr;
    m_symlinkName = nullptr;
    m_symlinkTarget = nullptr;
    m_symlinkBrowseButton = nullptr;
    m_symlinkError = nullptr;
  }

  void SymlinkDialogController::setTargetPath(std::string_view path) {
    if (m_host.closeQueued() || m_symlinkTarget == nullptr)
      return;
    m_symlinkTarget->setValue(path);
    m_symlinkTarget->setInvalid(false);
    if (m_symlinkError != nullptr)
      m_symlinkError->setText(" ");
    m_host.setFocus(m_symlinkTarget->inputArea());
    m_host.requestLayout();
  }

  void SymlinkDialogController::showTargetError(std::string_view message) {
    if (m_host.closeQueued() || m_symlinkError == nullptr)
      return;
    m_symlinkError->setText(message);
    m_host.requestLayout();
  }

  bool SymlinkDialogController::submit() {
    const auto fail = [this](std::string_view message, Input* field) {
      m_symlinkError->setText(message);
      if (field) {
        field->setInvalid(true);
        m_host.setFocus(field->inputArea());
      }
      for (auto* parent = m_symlinkError->parent(); parent; parent = parent->parent()) {
        if (auto* scroll = dynamic_cast<ScrollView*>(parent)) {
          scroll->requestScrollIntoView(m_symlinkError);
          break;
        }
      }
      m_host.requestLayout();
      return false;
    };
    if (!m_symlinkSubmitCallback)
      return true;
    if (const auto error = m_symlinkSubmitCallback(m_symlinkName->value(), m_symlinkTarget->value())) {
      Input* field = error->field == SymlinkField::Name ? m_symlinkName
          : error->field == SymlinkField::Target        ? m_symlinkTarget
                                                        : nullptr;
      return fail(error->message, field);
    }
    return true;
  }

  bool SymlinkDialogController::onKeyEvent(const KeyboardEvent& event) {
    if (!event.repeat && event.modifiers == 0U && (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter)) {
      auto* focus = m_host.focusedArea();
      if (focus == m_symlinkTarget->inputArea()) {
        m_host.closeDialog(1);
        return true;
      }
      if (focus == m_symlinkName->inputArea()) {
        if (validFileName(m_symlinkName->value()) && m_symlinkTarget->value().empty())
          m_host.setFocus(m_symlinkTarget->inputArea());
        else
          m_host.closeDialog(1);
        return true;
      }
    }
    return false;
  }

  void SymlinkDialogController::buildFields(Flex& root, float dialogWidth) {
    const auto wireInput = [this](Input& control) {
      control.setControlHeight(34.0F);
      control.setMinLayoutWidth(0.0F);
      control.setOnChange([this, &control](const std::string&) {
        control.setInvalid(false);
        if (m_symlinkError != nullptr)
          m_symlinkError->setText(" ");
        m_host.requestLayout();
      });
    };
    const auto makeFieldContainer = [](const char* text) {
      auto field = std::make_unique<Flex>();
      field->setDirection(FlexDirection::Vertical);
      field->setAlign(FlexAlign::Stretch);
      field->setGap(4.0F);
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setFontWeight(FontWeight::SemiBold);
      field->addChild(std::move(label));
      return field;
    };

    auto nameField = makeFieldContainer("Symlink name");
    auto nameInput = std::make_unique<Input>();
    m_symlinkName = nameInput.get();
    wireInput(*m_symlinkName);
    nameField->addChild(std::move(nameInput));
    root.addChild(std::move(nameField));

    auto targetField = makeFieldContainer("Target path");
    auto targetRow = std::make_unique<Flex>();
    targetRow->setDirection(FlexDirection::Horizontal);
    targetRow->setAlign(FlexAlign::Center);
    targetRow->setGap(8.0F);
    auto targetInput = std::make_unique<Input>();
    m_symlinkTarget = targetInput.get();
    wireInput(*m_symlinkTarget);
    m_symlinkTarget->setFlexGrow(1.0F);
    targetRow->addChild(std::move(targetInput));
    auto browse = std::make_unique<Button>();
    browse->setText("Browse…");
    browse->setVariant(ButtonVariant::Outline);
    browse->setControlHeight(34.0F);
    browse->setOnClick([this] {
      if (m_symlinkBrowseCallback && m_symlinkTarget && !m_host.closeQueued())
        m_symlinkBrowseCallback(m_symlinkTarget->value());
    });
    m_symlinkBrowseButton = browse.get();
    targetRow->addChild(std::move(browse));
    targetField->addChild(std::move(targetRow));
    root.addChild(std::move(targetField));

    m_symlinkName->setOnSubmit([this](const std::string&) { m_host.setFocus(m_symlinkTarget->inputArea()); });
    m_symlinkTarget->setOnSubmit([this](const std::string&) { m_host.closeDialog(1); });
    auto error = std::make_unique<Label>();
    error->setText(" ");
    error->setColor(colorSpecFromRole(ColorRole::Error));
    error->setMaxWidth(dialogWidth - 48.0F);
    error->setMaxLines(2);
    m_symlinkError = error.get();
    root.addChild(std::move(error));
  }
} // namespace filemanager
