#pragma once

#include "filemanager/dialogs/modal_dialog_host.h"

#include <future>
#include <mutex>

namespace filemanager {
  class ConflictDialogController {
  public:
    ConflictDialogController(
        ModalDialogHost& host, const ModalDialogHost::Callbacks& callbacks, std::weak_ptr<bool> alive
    );
    [[nodiscard]] FileOperationManager::ConflictResolver makeResolver(ConflictPathAccess pathAccess);
    [[nodiscard]] bool backgroundPending() const noexcept { return m_backgroundConflictPending; }
    [[nodiscard]] std::shared_ptr<std::timed_mutex> serialMutex() const { return m_conflictMutex; }
    void cancelPresentation();

  private:
    friend class ::FileManagerTestAccess;
    using Response = std::function<void(int, std::string)>;
    void present(ConflictDialogData data, bool hasRemaining, ToplevelSurface* parent, Response response);
    void buildFields(Flex& root, float dialogWidth, bool hasRemaining);
    [[nodiscard]] bool interceptResponse(int response);
    void finishBackground();

    ModalDialogHost& m_host;
    const ModalDialogHost::Callbacks& m_callbacks;
    std::weak_ptr<bool> m_alive;
    std::shared_ptr<std::timed_mutex> m_conflictMutex = std::make_shared<std::timed_mutex>();
    std::shared_ptr<std::promise<std::pair<int, std::string>>> m_conflictRequest;
    std::function<bool(const ConflictDialogData&, bool, Response)> m_conflictDialogHook;
    bool m_backgroundConflictPending = false;
    std::optional<ConflictDialogData> m_data;
    bool m_applyToAllChecked = false;
    std::string m_alternativeName;
    Input* m_alternativeNameInput = nullptr;
    Label* m_alternativeNameError = nullptr;
  };
} // namespace filemanager
