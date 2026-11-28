#include "filemanager/dialogs/conflict_dialog_controller.h"

#include "core/files/display_text.h"
#include "filemanager/file_interaction.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <gio/gio.h>
#include <utility>

namespace filemanager {
  ConflictDialogController::ConflictDialogController(
      ModalDialogHost& host, const ModalDialogHost::Callbacks& callbacks, std::weak_ptr<bool> alive
  )
      : m_host(host), m_callbacks(callbacks), m_alive(std::move(alive)) {}

  FileOperationManager::ConflictResolver ConflictDialogController::makeResolver(ConflictPathAccess pathAccess) {
    return [this, pathAccess, alive = m_alive, conflictMutex = m_conflictMutex](
               const std::string& source, const std::string& destination, bool hasRemaining,
               const file_operation_engine::ConflictMetadata& metadata, GCancellable* cancellable
           ) {
      if (const auto initialAlive = alive.lock();
          !initialAlive || !*initialAlive || g_cancellable_is_cancelled(cancellable))
        return FileOperationManager::ConflictDecision{};
      std::unique_lock lock(*conflictMutex, std::defer_lock);
      while (!lock.try_lock_for(std::chrono::milliseconds(5))) {
        if (g_cancellable_is_cancelled(cancellable))
          return FileOperationManager::ConflictDecision{};
      }
      if (const auto lockedAlive = alive.lock();
          !lockedAlive || !*lockedAlive || g_cancellable_is_cancelled(cancellable))
        return FileOperationManager::ConflictDecision{};
      auto promise = std::make_shared<std::promise<std::pair<int, std::string>>>();
      auto answer = promise->get_future();
      const auto describe = [](const file_operation_engine::ConflictItemMetadata& item) {
        if (!item.inspected)
          return std::string{};
        const bool directory = item.type == G_FILE_TYPE_DIRECTORY;
        g_autofree gchar* typeName =
            item.contentType.empty() ? nullptr : g_content_type_get_description(item.contentType.c_str());
        std::string details = directory ? "Folder" : typeName == nullptr ? "File" : typeName;
        if (!directory && item.size) {
          g_autofree gchar* size = g_format_size(static_cast<guint64>(*item.size));
          details += " · ";
          details += size;
        }
        if (item.modified) {
          GDateTime* date = g_date_time_new_from_unix_local(static_cast<gint64>(*item.modified));
          if (date != nullptr) {
            g_autofree gchar* formatted = g_date_time_format(date, "%e %b %Y, %H:%M");
            details += " · Modified ";
            details += formatted;
            g_date_time_unref(date);
          }
        }
        return details;
      };
      const auto sourcePath = std::filesystem::path(source);
      const auto destinationPath = std::filesystem::path(destination);
      const auto& incoming = metadata.incoming;
      const auto& existing = metadata.existing;
      const bool differentTypes = incoming.type == G_FILE_TYPE_UNKNOWN
          || existing.type == G_FILE_TYPE_UNKNOWN
          || incoming.type != existing.type;
      const bool directoryConflict = incoming.type == G_FILE_TYPE_DIRECTORY && existing.type == G_FILE_TYPE_DIRECTORY;
      std::string comparisonSummary;
      if (differentTypes)
        comparisonSummary = "The item types differ";
      else if (!directoryConflict) {
        if (incoming.size && existing.size && *incoming.size != *existing.size)
          comparisonSummary = *incoming.size > *existing.size ? "Incoming file is larger" : "Incoming file is smaller";
        if (incoming.modified && existing.modified && *incoming.modified != *existing.modified) {
          if (!comparisonSummary.empty())
            comparisonSummary += " · ";
          comparisonSummary +=
              *incoming.modified > *existing.modified ? "Incoming item is newer" : "Incoming item is older";
        }
      }
      const ConflictDialogKind kind = metadata.sameItem       ? ConflictDialogKind::SelfConflict
          : differentTypes                                    ? ConflictDialogKind::TypeMismatch
          : directoryConflict && source.starts_with("trash:") ? ConflictDialogKind::RestoreDirectory
          : directoryConflict                                 ? ConflictDialogKind::Directory
                                                              : ConflictDialogKind::File;
      ConflictDialogData conflictData{
          .kind = kind,
          .incoming =
              ConflictItemInfo{
                  .name = sourcePath.filename().string(),
                  .metadata = describe(incoming),
                  .path = sourcePath,
                  .isDirectory = incoming.type == G_FILE_TYPE_DIRECTORY,
              },
          .existing =
              ConflictItemInfo{
                  .name = destinationPath.filename().string(),
                  .metadata = describe(existing),
                  .path = destinationPath,
                  .isDirectory = existing.type == G_FILE_TYPE_DIRECTORY,
              },
          .comparisonSummary = std::move(comparisonSummary),
          .pathAccess = pathAccess,
      };
      struct ConflictDialogRequest {
        ConflictDialogController* self;
        std::weak_ptr<bool> alive;
        std::shared_ptr<std::promise<std::pair<int, std::string>>> promise;
        ConflictDialogData data;
        std::shared_ptr<GCancellable> cancellable;
        bool hasRemaining;
      };
      g_idle_add(
          [](gpointer data) -> gboolean {
            std::unique_ptr<ConflictDialogRequest> request(static_cast<ConflictDialogRequest*>(data));
            const auto idleAlive = request->alive.lock();
            if (!idleAlive || !*idleAlive || g_cancellable_is_cancelled(request->cancellable.get())) {
              try {
                request->promise->set_value({4, {}});
              } catch (...) {
              }
              return G_SOURCE_REMOVE;
            }
            auto* self = request->self;
            if (self->m_conflictDialogHook) {
              auto respond = [responseAlive = request->alive, promise = request->promise](int choice, std::string alt) {
                const auto dialogAlive = responseAlive.lock();
                if (!dialogAlive || !*dialogAlive) {
                  try {
                    promise->set_value({4, {}});
                  } catch (...) {
                  }
                  return;
                }
                try {
                  promise->set_value({choice, std::move(alt)});
                } catch (...) {
                }
              };
              if (self->m_conflictDialogHook(request->data, request->hasRemaining, std::move(respond)))
                return G_SOURCE_REMOVE;
            }
            const bool mainWindowOpen =
                self->m_callbacks.isMainWindowOpen ? self->m_callbacks.isMainWindowOpen() : true;
            if (!mainWindowOpen) {
              self->m_backgroundConflictPending = true;
              if (self->m_callbacks.syncProgressWindow)
                self->m_callbacks.syncProgressWindow();
            }
            self->m_conflictRequest = request->promise;
            ToplevelSurface* parent = mainWindowOpen
                ? (self->m_callbacks.mainSurface ? self->m_callbacks.mainSurface() : nullptr)
                : (self->m_callbacks.progressSurface ? self->m_callbacks.progressSurface() : nullptr);
            self->present(
                std::move(request->data), request->hasRemaining, parent,
                [self, responseAlive = request->alive, promise = request->promise](int choice, std::string name) {
                  const auto dialogAlive = responseAlive.lock();
                  if (!dialogAlive || !*dialogAlive)
                    choice = 4;
                  try {
                    promise->set_value({choice, std::move(name)});
                  } catch (...) {
                  }
                  if (dialogAlive && *dialogAlive && self->m_conflictRequest == promise) {
                    self->m_conflictRequest.reset();
                    self->finishBackground();
                  }
                }
            );
            return G_SOURCE_REMOVE;
          },
          new ConflictDialogRequest{
              this, alive, promise, std::move(conflictData),
              cancellable ? std::shared_ptr<GCancellable>{G_CANCELLABLE(g_object_ref(cancellable)), g_object_unref}
                          : nullptr,
              hasRemaining
          }
      );
      while (answer.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
        const auto waitAlive = alive.lock();
        if (!waitAlive || !*waitAlive || g_cancellable_is_cancelled(cancellable)) {
          struct CloseRequest {
            ConflictDialogController* self;
            std::weak_ptr<bool> alive;
            std::shared_ptr<void> conflict;
          };
          g_idle_add(
              [](gpointer data) -> gboolean {
                std::unique_ptr<CloseRequest> request(static_cast<CloseRequest*>(data));
                if (const auto closeAlive = request->alive.lock();
                    closeAlive && *closeAlive && request->self->m_conflictRequest == request->conflict)
                  request->self->m_host.closeDialog(-1);
                return G_SOURCE_REMOVE;
              },
              new CloseRequest{this, alive, promise}
          );
          return FileOperationManager::ConflictDecision{};
        }
      }
      const auto [choice, alternativeName] = answer.get();
      using Action = FileOperationManager::ConflictAction;
      switch (choice >= 10 ? choice - 10 : choice) {
      case 1:
        return FileOperationManager::ConflictDecision{Action::Replace, choice >= 11, {}};
      case 2:
        return FileOperationManager::ConflictDecision{Action::Rename, choice >= 12, alternativeName};
      case 3:
        return FileOperationManager::ConflictDecision{Action::Skip, choice >= 13, {}};
      case 5:
        return FileOperationManager::ConflictDecision{Action::Merge, choice >= 15, {}};
      default:
        return FileOperationManager::ConflictDecision{Action::Cancel, false};
      }
    };
  }

  void ConflictDialogController::finishBackground() {
    m_backgroundConflictPending = false;
    if (m_callbacks.syncProgressWindow)
      m_callbacks.syncProgressWindow();
  }

  void ConflictDialogController::cancelPresentation() {
    if (auto promise = std::exchange(m_conflictRequest, nullptr)) {
      try {
        promise->set_value({4, {}});
      } catch (...) {
      }
      const bool wasPending = std::exchange(m_backgroundConflictPending, false);
      if (const auto alive = m_alive.lock(); wasPending && alive && *alive && m_callbacks.syncProgressWindow)
        m_callbacks.syncProgressWindow();
    }
  }

  void ConflictDialogController::present(
      ConflictDialogData data, bool hasRemaining, ToplevelSurface* parent, Response response
  ) {
    if (m_host.isOpen()) {
      response(-1, {});
      return;
    }
    const auto kind = data.kind;
    const bool selfConflict = kind == ConflictDialogKind::SelfConflict;
    const bool incompatible = kind == ConflictDialogKind::TypeMismatch;
    const bool restoreFolders = kind == ConflictDialogKind::RestoreDirectory;
    const bool folders = restoreFolders || kind == ConflictDialogKind::Directory;
    m_data = std::move(data);
    m_alternativeName.clear();
    m_applyToAllChecked = false;
    std::vector<DialogChoice> choices;
    if (kind == ConflictDialogKind::Directory)
      choices.push_back({"Merge", 5});
    else if (kind == ConflictDialogKind::File)
      choices.push_back({"Replace", 1, DialogChoiceStyle::Destructive});
    choices.push_back({"Keep Both", 2});
    if (!selfConflict)
      choices.push_back({"Skip", 3});
    choices.push_back({"Cancel", 4, DialogChoiceStyle::Cancel});
    m_host.showDialog(
        selfConflict       ? "Create a duplicate?"
            : incompatible ? "Items have different types"
            : folders      ? "Folder already exists"
                           : "File already exists",
        {}, std::move(choices),
        [this, response = std::move(response)](int choice) {
          const bool apply = m_applyToAllChecked && choice >= 1 && (choice <= 3 || choice == 5);
          response(choice + (apply ? 10 : 0), std::move(m_alternativeName));
        },
        parent,
        ModalDialogHost::CustomContent{
            .buildFields = [this, hasRemaining](Flex& root, float width) { buildFields(root, width, hasRemaining); },
            .interceptResponse = [this](int choice) { return interceptResponse(choice); },
            .onBeforeClose =
                [this] {
                  if (m_alternativeNameInput)
                    m_alternativeName = m_alternativeNameInput->value();
                },
            .onDismiss =
                [this] {
                  m_alternativeNameInput = nullptr;
                  m_alternativeNameError = nullptr;
                  m_data.reset();
                },
        },
        DialogStyle::Conflict
    );
  }

  bool ConflictDialogController::interceptResponse(int response) {
    if (response == 2 && m_alternativeNameInput) {
      const auto& name = m_alternativeNameInput->value();
      if (!name.empty()) {
        const auto nameError = invalidFileNameReason(name);
        const bool checkDestination = m_data->pathAccess == ConflictPathAccess::Local;
        std::error_code error;
        const bool occupied = nameError.empty()
            && checkDestination
            && std::filesystem::exists(m_data->existing.path.parent_path() / name, error);
        if (!nameError.empty() || occupied || error) {
          m_alternativeNameError->setText(
              !nameError.empty() ? std::string(nameError)
                  : occupied     ? "An item with that name already exists."
                                 : error.message()
          );
          m_alternativeNameError->setVisible(true);
          m_alternativeNameInput->setInvalid(true);
          m_host.setFocus(m_alternativeNameInput->inputArea());
          m_host.requestLayout();
          return true;
        }
      }
    }
    return false;
  }

  void ConflictDialogController::buildFields(Flex& root, float dialogWidth, bool hasRemaining) {
    auto explanation = std::make_unique<Label>();
    explanation->setText("Choose what to do with the incoming item.");
    explanation->setMaxWidth(dialogWidth - 48.0F);
    root.addChild(std::move(explanation));
    const auto addComparisonCard = [this, &root,
                                    dialogWidth](const ConflictItemInfo& item, std::string_view sectionTitle) {
      auto card = std::make_unique<Flex>();
      m_host.registerCard(card.get());
      card->setDirection(FlexDirection::Vertical);
      card->setAlign(FlexAlign::Stretch);
      card->setGap(4.0F);
      card->setPadding(10.0F, 12.0F);
      card->setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
      card->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
      card->setRadius(Style::scaledRadiusSm(1.0F));
      auto headingLabel = std::make_unique<Label>();
      headingLabel->setText(sectionTitle);
      headingLabel->setFontWeight(FontWeight::SemiBold);
      headingLabel->setColor(colorSpecFromRole(ColorRole::Primary));
      card->addChild(std::move(headingLabel));
      auto itemRow = std::make_unique<Flex>();
      itemRow->setDirection(FlexDirection::Horizontal);
      itemRow->setAlign(FlexAlign::Center);
      itemRow->setGap(10.0F);
      auto icon = std::make_unique<FileIcon>();
      icon->setAllowFilesystemQueries(m_data->pathAccess == ConflictPathAccess::Local && item.path.is_absolute());
      // Pass the exact unmodified filesystem path and directory flag to FileIcon.
      icon->setFile(item.path, item.isDirectory);
      icon->setGlyphSize(28.0F);
      itemRow->addChild(std::move(icon));
      const std::string displayName = sanitizeFilenameForDisplay(item.name);
      const std::string displayPath = sanitizeFilenameForDisplay(item.path.string());
      auto details = std::make_unique<Label>();
      details->setText(displayName + "\n" + item.metadata);
      details->setMaxWidth(dialogWidth - 110.0F);
      details->setMaxLines(3);
      details->setEllipsize(TextEllipsize::End);
      details->setFlexGrow(1.0F);
      details->setTooltip(item.metadata);
      itemRow->addChild(std::move(details));
      card->addChild(std::move(itemRow));
      auto path = std::make_unique<Label>();
      path->setText(displayPath);
      path->setMaxWidth(dialogWidth - 56.0F);
      path->setMaxLines(2);
      path->setTooltip(displayPath);
      card->addChild(std::move(path));
      root.addChild(std::move(card));
    };
    addComparisonCard(m_data->incoming, "INCOMING ITEM");
    addComparisonCard(m_data->existing, "EXISTING DESTINATION");
    if (!m_data->comparisonSummary.empty()) {
      auto summary = std::make_unique<Label>();
      summary->setText(m_data->comparisonSummary);
      summary->setMaxWidth(dialogWidth - 48.0F);
      summary->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      root.addChild(std::move(summary));
    }
    if (hasRemaining) {
      auto applyRow = std::make_unique<Flex>();
      applyRow->setDirection(FlexDirection::Horizontal);
      applyRow->setAlign(FlexAlign::Center);
      applyRow->setGap(6.0F);
      auto checkbox = std::make_unique<Checkbox>();
      checkbox->setOnChange([this](bool checked) { m_applyToAllChecked = checked; });
      applyRow->addChild(std::move(checkbox));
      auto label = std::make_unique<Label>();
      label->setText("Apply this decision to remaining conflicts");
      label->setMaxWidth(dialogWidth - 80.0F);
      applyRow->addChild(std::move(label));
      root.addChild(std::move(applyRow));
    }
    auto renameRow = std::make_unique<Flex>();
    renameRow->setDirection(FlexDirection::Horizontal);
    renameRow->setAlign(FlexAlign::Center);
    renameRow->setGap(8.0F);
    auto input = std::make_unique<Input>();
    m_alternativeNameInput = input.get();
    input->setPlaceholder("Alternative name (for Keep Both)");
    input->setControlHeight(34.0F);
    input->setFlexGrow(1.0F);
    input->setOnSubmit([this](const std::string&) { m_host.closeDialog(2); });
    renameRow->addChild(std::move(input));
    auto suggest = std::make_unique<Button>();
    suggest->setText("Suggest New Name");
    suggest->setVariant(ButtonVariant::Ghost);
    const auto& incomingName = m_data->incoming.name;
    const auto& destinationPath = m_data->existing.path;
    const auto clearNameError = [this] {
      m_alternativeNameInput->setInvalid(false);
      m_alternativeNameError->setVisible(false);
      m_host.requestLayout();
    };
    m_alternativeNameInput->setOnChange([clearNameError](const std::string&) { clearNameError(); });
    const bool directory = m_data->incoming.isDirectory;
    const auto stem = directory ? incomingName : std::filesystem::path(incomingName).stem().string();
    const auto extension = directory ? std::string{} : std::filesystem::path(incomingName).extension().string();
    const bool checkDestination = m_data->pathAccess == ConflictPathAccess::Local;
    suggest->setOnClick([this, parentPath = destinationPath.parent_path(), stem, extension, checkDestination,
                         clearNameError, nextSuffix = 1U]() mutable {
      for (unsigned suffix = checkDestination ? 1U : nextSuffix; suffix < 10000; ++suffix) {
        const auto candidate =
            stem + " (copy" + (suffix == 1 ? std::string{} : " " + std::to_string(suffix)) + ")" + extension;
        if (!checkDestination) {
          nextSuffix = suffix + 1;
          m_alternativeNameInput->setValue(candidate);
          clearNameError();
          break;
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(parentPath / candidate, error);
        if (error)
          break;
        if (!exists) {
          m_alternativeNameInput->setValue(candidate);
          clearNameError();
          break;
        }
      }
    });
    renameRow->addChild(std::move(suggest));
    root.addChild(std::move(renameRow));
    auto error = std::make_unique<Label>();
    error->setVisible(false);
    error->setColor(colorSpecFromRole(ColorRole::Error));
    error->setMaxWidth(dialogWidth - 48.0F);
    error->setMaxLines(2);
    m_alternativeNameError = error.get();
    root.addChild(std::move(error));
  }
} // namespace filemanager
