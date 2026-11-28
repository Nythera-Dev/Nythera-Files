#include "filemanager/file_clipboard_controller.h"

#include "filemanager/file_search_session.h"
#include "filemanager/file_transfer_payload.h"
#include "render/core/renderer.h"
#include "ui/controls/virtual_grid_view.h"

#include <algorithm>
#include <utility>

namespace filemanager {

  namespace {
    std::filesystem::path normalizeDirectoryPath(const std::filesystem::path& path) {
      if (path.empty())
        return {};
      auto normalized = path.lexically_normal();
      if (normalized.has_filename() && normalized.filename() == ".")
        normalized = normalized.parent_path();
      const auto text = normalized.string();
      if (text.size() > 1 && text.ends_with('/'))
        return std::filesystem::path(text.substr(0, text.size() - 1));
      return normalized;
    }

    std::string normalizeCutPathKey(const std::filesystem::path& path) { return normalizeDirectoryPath(path).string(); }

    std::optional<filemanager::FileTransferPayload> decodeClipboardPayload(const NativeDataTransfer::Payload& payload) {
      try {
        if (payload.contains("text/uri-list")) {
          bool cut =
              payload.contains("application/x-kde-cutselection") && payload.at("application/x-kde-cutselection") == "1";
          if (!payload.contains("application/x-kde-cutselection") && payload.contains("x-special/gnome-copied-files")) {
            cut = filemanager::decodeFileTransfer(payload.at("x-special/gnome-copied-files"), true).cut;
          }
          return filemanager::decodeFileTransfer(payload.at("text/uri-list"), false, cut);
        }
        if (payload.contains("x-special/gnome-copied-files"))
          return filemanager::decodeFileTransfer(payload.at("x-special/gnome-copied-files"), true);
      } catch (const std::exception&) {
      }
      return std::nullopt;
    }
  } // namespace

  FileClipboardController::FileClipboardController(
      WaylandConnection& wayland, NativeDataTransfer& transfer, FileOperationManager& operations, TabManager& tabs,
      PlacesSidebarController& places, PathResolutionCache& pathCache, Callbacks callbacks
  )
      : m_wayland(wayland), m_transfer(transfer), m_operations(operations), m_tabs(tabs), m_places(places),
        m_pathCache(pathCache), m_callbacks(std::move(callbacks)) {}

  FileClipboardController::~FileClipboardController() { shutdown(); }

  void FileClipboardController::shutdown() {
    if (m_alive)
      *m_alive = false;
  }

  void FileClipboardController::onBeforePathChange(BrowserTab& tab) {
    tab.pendingPasteSelection.reset();
    tab.pendingPasteRevealPath.reset();
    tab.pendingDropScrollGeneration.reset();
    tab.pendingRevealOnActivate = false;
  }

  void FileClipboardController::onEnumerateStarted(BrowserTab& tab) { tab.identityCheckedPaths.clear(); }

  void FileClipboardController::registerPasteRequest(std::uint64_t opId, PasteRequest request) {
    m_pasteRequests[opId] = std::move(request);
  }

  bool FileClipboardController::onOperationFinished(const FileOperationManager::Operation& operation) {
    const bool pastedEntries = recordPasteCompletion(operation);
    if (auto cut = m_cutOperations.find(operation.id); cut != m_cutOperations.end()) {
      const auto cutGeneration = cut->second;
      auto srcIt = m_cutOperationSources.find(operation.id);
      std::vector<std::string> originalSources =
          srcIt != m_cutOperationSources.end() ? std::move(srcIt->second) : m_activeCutOrdered;
      if (srcIt != m_cutOperationSources.end())
        m_cutOperationSources.erase(srcIt);
      m_cutOperations.erase(cut);

      if (cutGeneration == m_transfer.selectionGeneration()) {
        if (operation.succeeded
            && (!operation.adminOrigin || operation.movedSources.size() == originalSources.size())) {
          m_transfer.consumeSelection(cutGeneration);
          m_consumedCutSelection = cutGeneration;
          const bool hadActiveCut = !m_activeCutPaths.empty();
          m_activeCutPaths.clear();
          m_activeCutOrdered.clear();
          if (hadActiveCut && m_callbacks.onCutStateChanged)
            m_callbacks.onCutStateChanged();
        } else if (!operation.movedSources.empty()) {
          std::unordered_set<std::string> movedSet;
          movedSet.reserve(operation.movedSources.size());
          for (const auto& moved : operation.movedSources)
            movedSet.insert(normalizeCutPathKey(moved));

          std::vector<std::string> remainingOrdered;
          std::unordered_set<std::string> remainingSet;
          for (const auto& candidate : originalSources) {
            const auto key = normalizeCutPathKey(candidate);
            if (!movedSet.contains(key)) {
              remainingOrdered.push_back(candidate);
              remainingSet.insert(key);
            }
          }
          m_activeCutOrdered = remainingOrdered;
          m_activeCutPaths = std::move(remainingSet);
          if (m_activeCutOrdered.empty()) {
            m_transfer.consumeSelection(cutGeneration);
            m_consumedCutSelection = cutGeneration;
          } else if (m_transfer.ownsSelection()) {
            try {
              m_transfer.updateOwnedSelection(filemanager::encodeFileTransfer(m_activeCutOrdered, true));
            } catch (const std::exception&) {
            }
          }
          if (m_callbacks.onCutStateChanged)
            m_callbacks.onCutStateChanged();
        }
      }
    } else if (
        (operation.succeeded || operation.mutated)
        && (operation.kind == FileOperationManager::Kind::Rename
            || operation.kind == FileOperationManager::Kind::Delete
            || operation.kind == FileOperationManager::Kind::Trash
            || operation.kind == FileOperationManager::Kind::TrashToHome)
        && !m_activeCutPaths.empty()
    ) {
      bool changed = false;
      const auto removeKey = [&](const std::string& raw) {
        if (raw.empty())
          return;
        const auto key = normalizeCutPathKey(raw);
        if (m_activeCutPaths.erase(key) > 0) {
          std::erase_if(m_activeCutOrdered, [&](const std::string& p) { return normalizeCutPathKey(p) == key; });
          changed = true;
        }
      };
      if (operation.kind == FileOperationManager::Kind::Rename && operation.succeeded)
        removeKey(operation.source);
      else if (operation.kind == FileOperationManager::Kind::Delete && operation.succeeded)
        removeKey(operation.source);
      else
        for (const auto& moved : operation.movedSources)
          removeKey(moved);
      if (changed) {
        if (m_activeCutOrdered.empty() && m_transfer.ownsSelection())
          m_transfer.consumeSelection(m_transfer.selectionGeneration());
        else if (m_transfer.ownsSelection()) {
          try {
            m_transfer.updateOwnedSelection(filemanager::encodeFileTransfer(m_activeCutOrdered, true));
          } catch (const std::exception&) {
          }
        }
        if (m_callbacks.onCutStateChanged)
          m_callbacks.onCutStateChanged();
      }
    }
    return pastedEntries;
  }

  void FileClipboardController::pasteClipboard() {
    const auto& active = m_tabs.activeTab();
    const auto caps = active.capabilities();
    const bool adminPaste = active.administratorBrowsing;
    if (!caps.canMutate
        || active.session.browsingTrash()
        || m_places.hasActiveDeviceOperations()
        || m_operations.replaying()) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("This action is currently unavailable");
      return;
    }
    const TabId tabId = active.id;
    const auto destination = active.session.path().string();
    const auto navigation = active.session.navigationGeneration();
    const auto adminGeneration = active.adminSession.sessionGeneration();
    const auto listing = active.session.privilegedListingId();
    const auto generation = m_transfer.selectionGeneration();
    if (m_consumedCutSelection == generation) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("These cut files have already been moved");
      return;
    }
    ++m_pendingClipboardReads;
    auto onSelection = [this, alive = std::weak_ptr<bool>(m_alive), tabId, destination, navigation, generation,
                        adminPaste, adminGeneration, listing](std::optional<NativeDataTransfer::Payload> payload) {
      const auto locked = alive.lock();
      if (!locked || !*locked)
        return;
      --m_pendingClipboardReads;
      const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
      const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
      auto* tab = m_tabs.findTab(tabId);
      const bool tabStillOpen = tab != nullptr;
      const bool canPaste = tabStillOpen
          && (adminPaste ? tab->administratorBrowsing
                      && tab->capabilities().canMutate
                      && tab->adminSession.sessionGeneration() == adminGeneration
                      && tab->session.navigationGeneration() == navigation
                      && tab->session.path() == destination
                      && listing != kInvalidAdminRequestId
                      && tab->session.privilegedListingId() == listing
                      && m_transfer.selectionGeneration() == generation
                         : tab->capabilities().canMutate && !tab->administratorBrowsing);
      const auto reportStatus = [this, tabId](std::string message) {
        if (m_tabs.activeTabId() == tabId && m_callbacks.onStatus)
          m_callbacks.onStatus(std::move(message));
        else if (auto* originTab = m_tabs.findTab(tabId))
          originTab->lastStatusMessage = std::move(message);
      };
      if (!quitting
          && windowOpen
          && canPaste
          && payload
          && !m_places.hasActiveDeviceOperations()
          && !m_operations.replaying()) {
        try {
          filemanager::FileTransferPayload transfer;
          if (payload->contains("text/uri-list")) {
            bool cut = payload->contains("application/x-kde-cutselection")
                && payload->at("application/x-kde-cutselection") == "1";
            if (!payload->contains("application/x-kde-cutselection")
                && payload->contains("x-special/gnome-copied-files"))
              cut = filemanager::decodeFileTransfer(payload->at("x-special/gnome-copied-files"), true).cut;
            transfer = filemanager::decodeFileTransfer(payload->at("text/uri-list"), false, cut);
          } else {
            transfer = filemanager::decodeFileTransfer(payload->at("x-special/gnome-copied-files"), true);
          }
          if (adminPaste) {
            const auto kde = payload->find("application/x-kde-cutselection");
            const auto gnome = payload->find("x-special/gnome-copied-files");
            if (kde != payload->end() && kde->second != "0" && kde->second != "1")
              throw std::invalid_argument("Invalid clipboard copy or cut marker");
            if (gnome != payload->end()) {
              const auto gnomeTransfer = filemanager::decodeFileTransfer(gnome->second, true);
              if (gnomeTransfer.paths != transfer.paths)
                throw std::invalid_argument("Clipboard file lists disagree");
              if (kde != payload->end() && (kde->second == "1") != gnomeTransfer.cut)
                throw std::invalid_argument("Clipboard copy and cut markers disagree");
              transfer.cut = gnomeTransfer.cut;
            }
          }
          auto* adminTab = adminPaste ? tab : nullptr;
          if (transfer.paths.empty()) {
            reportStatus("Clipboard contains no local files");
          } else if (transfer.cut) {
            if (std::ranges::any_of(m_cutOperations, [generation](const auto& item) {
                  return item.second == generation;
                })) {
              reportStatus("These cut files are already being moved");
              return;
            }
            auto cutSources = transfer.paths;
            const auto id = m_operations.move(std::move(transfer.paths), destination, adminOperationContext(adminTab));
            if (adminPaste && !id) {
              reportStatus("Wait for the current Administrator operation to finish");
            } else {
              m_cutOperations[id] = generation;
              m_cutOperationSources[id] = std::move(cutSources);
              m_pasteRequests[id] = PasteRequest{tabId, destination, navigation};
            }
          } else {
            const auto id = m_operations.copy(std::move(transfer.paths), destination, adminOperationContext(adminTab));
            if (adminPaste && !id)
              reportStatus("Wait for the current Administrator operation to finish");
            else
              m_pasteRequests[id] = PasteRequest{tabId, destination, navigation};
          }
        } catch (const std::exception& error) {
          reportStatus(std::string("Cannot paste files: ") + error.what());
        }
      } else if (!quitting && windowOpen && tabStillOpen && (!adminPaste || canPaste)) {
        reportStatus("File clipboard unavailable or changed during paste");
      }
      if (m_callbacks.onMaybeStopMainLoop)
        m_callbacks.onMaybeStopMainLoop();
    };
    if (m_readPasteSelectionForTesting)
      m_readPasteSelectionForTesting(std::move(onSelection));
    else
      m_transfer.readSelection(std::move(onSelection), adminPaste);
  }

  bool FileClipboardController::recordPasteCompletion(const FileOperationManager::Operation& operation) {
    const auto found = m_pasteRequests.find(operation.id);
    if (found == m_pasteRequests.end())
      return false;
    const auto request = found->second;
    m_pasteRequests.erase(found);
    const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
    BrowserTab* tab = m_tabs.findTab(request.tabId);
    if (!windowOpen
        || tab == nullptr
        || request.navigation != tab->session.navigationGeneration()
        || operation.completedEntries.empty())
      return false;
    const auto applyMatch = [this](
                                BrowserTab& targetTab, const PasteRequest& matchedRequest,
                                const std::vector<FileOperationManager::CompletedEntry>& completedEntries
                            ) {
      const auto samePending = targetTab.pendingPasteSelection
          && (targetTab.administratorBrowsing
                  ? targetTab.pendingPasteSelection->request.destination == matchedRequest.destination
                  : m_pathCache
                        .sameDirectoryPathCached(
                            targetTab.pendingPasteSelection->request.destination, matchedRequest.destination
                        )
                        .value_or(false));
      if (!targetTab.pendingPasteSelection
          || targetTab.pendingPasteSelection->request.tabId != matchedRequest.tabId
          || targetTab.pendingPasteSelection->request.navigation != matchedRequest.navigation
          || !samePending)
        targetTab.pendingPasteSelection = PasteSelection{matchedRequest, {}};
      auto& entries = targetTab.pendingPasteSelection->entries;
      entries.insert(entries.end(), completedEntries.begin(), completedEntries.end());
    };
    if (operation.adminOrigin) {
      if (!tab->hasActiveAdminSession()
          || tab->adminSession.sessionGeneration() != operation.adminOrigin->sessionGeneration
          || tab->session.path() != request.destination)
        return false;
      applyMatch(*tab, request, operation.completedEntries);
      return true;
    }
    if (const auto sameDest = m_pathCache.sameDirectoryPathCached(request.destination, tab->session.path())) {
      if (!*sameDest)
        return false;
      applyMatch(*tab, request, operation.completedEntries);
      return true;
    }
    const TabId tabId = tab->id;
    const auto expectedPath = tab->session.path();
    auto completedEntries = operation.completedEntries;
    m_pathCache.compareDirectoriesAsync(
        request.destination, expectedPath,
        [this, alive = std::weak_ptr<bool>(m_alive), tabAlive = std::weak_ptr<bool>(tab->alive), tabId, request,
         expectedPath, completedEntries = std::move(completedEntries), applyMatch](bool equivalent) {
          const auto locked = alive.lock();
          const auto tabLocked = tabAlive.lock();
          const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
          const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
          BrowserTab* targetTab = m_tabs.findTab(tabId);
          if (!locked
              || !*locked
              || !tabLocked
              || !*tabLocked
              || quitting
              || !stillOpen
              || targetTab == nullptr
              || request.navigation != targetTab->session.navigationGeneration()
              || targetTab->session.path() != expectedPath
              || !equivalent)
            return;
          applyMatch(*targetTab, request, completedEntries);
          if (!targetTab->session.loading())
            applyPendingPasteSelection(*targetTab);
        }
    );
    return false;
  }

  void FileClipboardController::applyPendingPasteSelection(BrowserTab& tab) {
    if (!tab.pendingPasteSelection
        || tab.session.loading()
        || tab.session.search().status() == FileSearchSession::Status::Searching)
      return;
    auto pending = std::move(*tab.pendingPasteSelection);
    tab.pendingPasteSelection.reset();
    const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
    if (!windowOpen
        || pending.request.tabId != tab.id
        || pending.request.navigation != tab.session.navigationGeneration())
      return;
    const bool admin = tab.administratorBrowsing;
    if (const auto sameDest = admin
            ? std::optional(pending.request.destination == tab.session.path())
            : m_pathCache.sameDirectoryPathCached(pending.request.destination, tab.session.path())) {
      if (!*sameDest)
        return;
    } else {
      const TabId tabId = tab.id;
      const auto expectedPath = tab.session.path();
      m_pathCache.compareDirectoriesAsync(
          pending.request.destination, expectedPath,
          [this, alive = std::weak_ptr<bool>(m_alive), tabAlive = std::weak_ptr<bool>(tab.alive), tabId,
           pending = std::move(pending), expectedPath](bool equivalent) mutable {
            const auto locked = alive.lock();
            const auto tabLocked = tabAlive.lock();
            const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
            const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
            BrowserTab* targetTab = m_tabs.findTab(tabId);
            if (!locked
                || !*locked
                || !tabLocked
                || !*tabLocked
                || quitting
                || !stillOpen
                || targetTab == nullptr
                || pending.request.navigation != targetTab->session.navigationGeneration()
                || targetTab->session.path() != expectedPath
                || !equivalent)
              return;
            targetTab->pendingPasteSelection = std::move(pending);
            if (!targetTab->session.loading())
              applyPendingPasteSelection(*targetTab);
          }
      );
      return;
    }
    std::vector<std::filesystem::path> unresolvedCandidates;
    for (const auto& entry : tab.session.entries()) {
      if (admin || entry.hasIdentity || entry.absPath.parent_path() / "" != tab.session.path() / "")
        continue;
      if (tab.identityCheckedPaths.contains(entry.absPath.string()))
        continue;
      const bool nameMatches = std::ranges::any_of(pending.entries, [&](const auto& completed) {
        const std::filesystem::path reported(completed.path);
        return reported == entry.absPath
            || (reported.filename() == entry.absPath.filename()
                && m_pathCache.sameDirectoryPathCached(reported.parent_path(), entry.absPath.parent_path())
                       .value_or(false));
      });
      if (nameMatches)
        unresolvedCandidates.push_back(entry.absPath);
    }
    if (!unresolvedCandidates.empty()) {
      struct IdentityBatch {
        TabId tabId = kInvalidTabId;
        std::weak_ptr<bool> tabAlive;
        PasteSelection pending;
        std::filesystem::path expectedPath;
        std::uint64_t enumGeneration = 0;
        std::size_t remaining = 0;
      };
      auto batch = std::make_shared<IdentityBatch>(IdentityBatch{
          .tabId = tab.id,
          .tabAlive = tab.alive,
          .pending = std::move(pending),
          .expectedPath = tab.session.path(),
          .enumGeneration = tab.session.enumerationGeneration(),
          .remaining = unresolvedCandidates.size(),
      });
      GCancellable* cancellable = tab.session.ensureEnumerationCancellable();
      for (const auto& candidate : unresolvedCandidates) {
        tab.identityCheckedPaths.insert(candidate.string());
        struct IdentityQuery {
          FileClipboardController* controller;
          std::weak_ptr<bool> alive;
          std::filesystem::path path;
          std::shared_ptr<IdentityBatch> batch;
        };
        GFile* file = g_file_new_for_path(candidate.c_str());
        g_file_query_info_async(
            file, "unix::device,unix::inode", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, cancellable,
            [](GObject* source, GAsyncResult* result, gpointer userData) {
              std::unique_ptr<IdentityQuery> query(static_cast<IdentityQuery*>(userData));
              GError* error = nullptr;
              GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
              const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
              g_clear_error(&error);
              const auto locked = query->alive.lock();
              const auto tabLocked = query->batch->tabAlive.lock();
              auto* self = query->controller;
              const bool quitting = self && self->m_callbacks.isQuitting && self->m_callbacks.isQuitting();
              const bool stillOpen = self && self->m_callbacks.isWindowOpen && self->m_callbacks.isWindowOpen();
              BrowserTab* targetTab = self ? self->m_tabs.findTab(query->batch->tabId) : nullptr;
              if (cancelled
                  || !locked
                  || !*locked
                  || !tabLocked
                  || !*tabLocked
                  || quitting
                  || !stillOpen
                  || targetTab == nullptr
                  || query->batch->enumGeneration != targetTab->session.enumerationGeneration()
                  || query->batch->pending.request.navigation != targetTab->session.navigationGeneration()
                  || targetTab->session.path() != query->batch->expectedPath) {
                g_clear_object(&info);
                return;
              }
              if (info != nullptr
                  && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE)
                  && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE)) {
                const auto dev =
                    static_cast<std::uint64_t>(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE));
                const auto ino = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE);
                if (auto it = std::ranges::find(targetTab->session.entries(), query->path, &FileEntry::absPath);
                    it != targetTab->session.entries().end()) {
                  it->device = dev;
                  it->inode = ino;
                  it->hasIdentity = true;
                }
              }
              g_clear_object(&info);
              if (--query->batch->remaining == 0) {
                targetTab->pendingPasteSelection = std::move(query->batch->pending);
                if (!targetTab->session.loading())
                  self->applyPendingPasteSelection(*targetTab);
              }
            },
            new IdentityQuery{this, m_alive, candidate, batch}
        );
        g_object_unref(file);
      }
      return;
    }

    std::unordered_set<std::string> paths;
    std::optional<std::size_t> first;
    for (std::size_t index = 0; index < tab.session.entries().size(); ++index) {
      const auto& current = tab.session.entries()[index];
      const auto& path = current.absPath;
      if (path.parent_path() / "" != tab.session.path() / "" || !current.hasIdentity)
        continue;
      if (std::ranges::none_of(pending.entries, [&](const auto& entry) {
            if (entry.device != current.device || entry.inode != current.inode)
              return false;
            const std::filesystem::path reported(entry.path);
            return reported == path
                || (!admin
                    && reported.filename() == path.filename()
                    && m_pathCache.sameDirectoryPathCached(reported.parent_path(), path.parent_path()).value_or(false));
          }))
        continue;
      paths.insert(path.string());
      if (!first)
        first = index;
    }
    if (!first)
      return;
    const bool isActive = (tab.id == m_tabs.activeTabId());
    auto* grid = isActive && m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const auto primaryPath = tab.session.entries()[*first].absPath;
    tab.pendingDropScrollGeneration = pending.request.dropScrollGeneration;
    const bool reveal = !tab.pendingDropScrollGeneration
        || (*tab.pendingDropScrollGeneration == tab.displayedScrollState().userScrollGeneration);
    tab.pendingPasteRevealPath = reveal ? std::optional(primaryPath) : std::nullopt;
    tab.session.clearChooseFirstAfterLoad();
    if (m_callbacks.onApplyPasteSelection)
      m_callbacks.onApplyPasteSelection(tab.id, std::move(paths), primaryPath);
    if (isActive) {
      tab.pendingRevealOnActivate = false;
      if (grid) {
        grid->notifyDataChanged();
        if (reveal && !tab.pendingDropScrollGeneration)
          grid->scrollToIndex(*first);
      }
      if (m_callbacks.onUpdateSelectionSummary)
        m_callbacks.onUpdateSelectionSummary();
      if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
        surface->requestLayout();
    } else {
      tab.pendingRevealOnActivate = reveal;
    }
  }

  void FileClipboardController::finishPendingTransferReveal(Renderer& renderer) {
    auto& tab = m_tabs.activeTab();
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (tab.pendingDropScrollGeneration) {
      if (!grid || *tab.pendingDropScrollGeneration != tab.displayedScrollState().userScrollGeneration) {
        tab.pendingPasteRevealPath.reset();
        tab.pendingDropScrollGeneration.reset();
        return;
      }
      if (tab.session.loading() || tab.session.pendingMonitorQueries() != 0 || tab.session.hasPendingMonitorChanges())
        return;
      if (tab.pendingPasteRevealPath && tab.selectedPath() == tab.pendingPasteRevealPath) {
        const auto entry = std::ranges::find(tab.session.entries(), *tab.pendingPasteRevealPath, &FileEntry::absPath);
        if (entry != tab.session.entries().end()) {
          grid->scrollToIndex(static_cast<std::size_t>(entry - tab.session.entries().begin()));
          grid->layout(renderer);
        }
      }
      tab.pendingDropScrollGeneration.reset();
    }
    tab.pendingPasteRevealPath.reset();
  }

  void FileClipboardController::copyToDesktopClipboard(bool cut) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    if (session.browsingTrash() || !caps.canCopyItems || (cut && !caps.canMutate)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus(
            session.browsingTrash() ? "This action is unavailable in Wastebin"
                                    : "This action is unavailable in Administrator Mode"
        );
      return;
    }
    const auto paths =
        m_callbacks.selectedPathsOrdered ? m_callbacks.selectedPathsOrdered() : std::vector<std::string>{};
    if (paths.empty())
      return;
    try {
      if (!m_transfer.setSelection(filemanager::encodeFileTransfer(paths, cut), m_wayland.lastInputSerial())) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Could not publish the file clipboard");
        return;
      }
    } catch (const std::exception& error) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus(std::string("Cannot copy files: ") + error.what());
    }
  }

  void FileClipboardController::writeDesktopClipboard(std::string payload, const char* mime) {
    if (!m_transfer.setSelection({{mime, std::move(payload)}}, m_wayland.lastInputSerial()) && m_callbacks.onStatus)
      m_callbacks.onStatus("Could not publish the clipboard");
  }

  void FileClipboardController::setCutPaths(std::vector<std::string> paths) {
    std::unordered_set<std::string> nextSet;
    std::vector<std::string> nextOrdered;
    nextSet.reserve(paths.size());
    nextOrdered.reserve(paths.size());
    for (auto& path : paths) {
      const auto key = normalizeCutPathKey(path);
      if (!key.empty() && nextSet.insert(key).second)
        nextOrdered.push_back(std::move(path));
    }
    const bool changed = nextSet != m_activeCutPaths;
    m_activeCutPaths = std::move(nextSet);
    m_activeCutOrdered = std::move(nextOrdered);
    if (!m_activeCutPaths.empty() && m_consumedCutSelection == m_transfer.selectionGeneration())
      m_consumedCutSelection.reset();
    if (changed && m_callbacks.onCutStateChanged)
      m_callbacks.onCutStateChanged();
  }

  bool FileClipboardController::cancelCutSelection() {
    const bool hasOwnedCut = m_transfer.ownsSelection()
        && m_transfer.ownedSelectionPayload() != nullptr
        && decodeClipboardPayload(*m_transfer.ownedSelectionPayload())
               .transform([](const auto& p) { return p.cut && !p.paths.empty(); })
               .value_or(false);
    if (m_activeCutPaths.empty() && !hasOwnedCut)
      return false;
    const auto gen = m_transfer.selectionGeneration();
    m_consumedCutSelection = gen;
    const bool hadActive = !m_activeCutPaths.empty();
    m_activeCutPaths.clear();
    m_activeCutOrdered.clear();
    if (m_transfer.ownsSelection())
      m_transfer.consumeSelection(gen);
    if (hadActive && m_callbacks.onCutStateChanged)
      m_callbacks.onCutStateChanged();
    return true;
  }

  void FileClipboardController::syncCutStateFromClipboard() {
    ++m_externalCutQueryGeneration;
    const auto generation = m_transfer.selectionGeneration();
    if (m_consumedCutSelection && *m_consumedCutSelection == generation) {
      const bool hadActive = !m_activeCutPaths.empty();
      m_activeCutPaths.clear();
      m_activeCutOrdered.clear();
      if (hadActive && m_callbacks.onCutStateChanged)
        m_callbacks.onCutStateChanged();
      return;
    }

    if (const auto* owned = m_transfer.ownedSelectionPayload()) {
      const auto decoded = decodeClipboardPayload(*owned);
      if (decoded && decoded->cut && !decoded->paths.empty())
        setCutPaths(decoded->paths);
      else
        setCutPaths({});
      return;
    }

    if (!m_transfer.ownsSelection() && m_transfer.selectionOffersFileCut()) {
      const auto queryGen = m_externalCutQueryGeneration;
      ++m_pendingClipboardReads;
      m_transfer.readSelection([this, alive = std::weak_ptr<bool>(m_alive), queryGen,
                                generation](std::optional<NativeDataTransfer::Payload> payload) {
        const auto locked = alive.lock();
        if (!locked || !*locked)
          return;
        --m_pendingClipboardReads;
        if (queryGen == m_externalCutQueryGeneration
            && generation == m_transfer.selectionGeneration()
            && (!m_consumedCutSelection || *m_consumedCutSelection != generation)) {
          const auto decoded = payload ? decodeClipboardPayload(*payload) : std::nullopt;
          if (decoded && decoded->cut && !decoded->paths.empty())
            setCutPaths(decoded->paths);
          else
            setCutPaths({});
        }
        if (m_callbacks.onMaybeStopMainLoop)
          m_callbacks.onMaybeStopMainLoop();
      });
      return;
    }

    setCutPaths({});
  }

  bool FileClipboardController::isPathCut(const std::filesystem::path& path) const {
    if (m_activeCutPaths.empty() || path.empty())
      return false;
    const auto normalized = normalizeCutPathKey(path);
    if (m_activeCutPaths.contains(normalized))
      return true;
    const auto filename = path.filename();
    const auto parent = path.parent_path();
    if (filename.empty() || parent.empty())
      return false;
    for (const auto& cutRaw : m_activeCutOrdered) {
      const std::filesystem::path cutPath(cutRaw);
      if (cutPath.filename() == filename
          && m_pathCache.sameDirectoryPathCached(cutPath.parent_path(), parent).value_or(false)) {
        return true;
      }
    }
    return false;
  }

} // namespace filemanager
