#pragma once

#include "filemanager/dialogs/modal_dialog_host.h"

namespace filemanager {
  class SymlinkDialogController {
  public:
    using SymlinkField = ModalDialogHost::SymlinkField;
    using SymlinkError = ModalDialogHost::SymlinkError;
    explicit SymlinkDialogController(ModalDialogHost& host) : m_host(host) {}
    void show(
        std::function<std::optional<SymlinkError>(const std::string&, const std::string&)> onSubmit,
        std::function<void(const std::string&)> onBrowseTarget, std::function<void()> onDismiss
    );
    void setTargetPath(std::string_view path);
    void showTargetError(std::string_view message);

  private:
    friend class ::FileManagerTestAccess;
    void buildFields(Flex& root, float dialogWidth);
    bool onKeyEvent(const KeyboardEvent& event);
    bool submit();
    void dismiss();

    ModalDialogHost& m_host;
    std::function<std::optional<SymlinkError>(const std::string&, const std::string&)> m_symlinkSubmitCallback;
    std::function<void(const std::string&)> m_symlinkBrowseCallback;
    std::function<void()> m_symlinkDismissCallback;
    Input* m_symlinkName = nullptr;
    Input* m_symlinkTarget = nullptr;
    Button* m_symlinkBrowseButton = nullptr;
    Label* m_symlinkError = nullptr;
  };
} // namespace filemanager
