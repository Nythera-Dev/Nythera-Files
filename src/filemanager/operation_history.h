#pragma once

#include "filemanager/file_operation_engine.h"
#include "filemanager/file_operation_kind.h"

namespace filemanager {
  class OperationHistory {
  public:
    using Kind = FileOperationKind;
    using FileIdentity = file_operation_engine::FileIdentity;
    using Revision = filemanager::file_operation_engine::Revision;
    struct Entry {
      Kind kind;
      std::string source;
      std::string destination;
      std::string linkTarget;
      std::optional<std::vector<std::uint8_t>> createdContent;
      std::optional<Revision> sourceRevision;
      Revision resultRevision;
      std::pair<std::uint64_t, std::uint64_t> sourceParent;
      std::pair<std::uint64_t, std::uint64_t> destinationParent;
      bool blocked = false;
    };
    static bool replay(Entry& entry, bool undo, GCancellable* cancellable, GError** error);

    [[nodiscard]] bool replaying() const noexcept { return m_replaying; }
    [[nodiscard]] bool canReplay(bool undoing) const noexcept;
    [[nodiscard]] const Entry& next(bool undoing) const;
    [[nodiscard]] std::string label(bool undoing) const;
    Entry beginReplay(bool undoing);
    void finishReplay(bool undoing, bool succeeded, bool blocked, Entry& after);
    void clearRedo() { m_redo.clear(); }
    void append(std::vector<Entry> entries);
    static void record(
        std::vector<Entry>& entries, Kind kind, const std::string& source, const std::string& destination,
        const std::optional<Revision>& sourceRevision, const std::optional<Revision>& resultRevision,
        FileIdentity sourceParent, FileIdentity destinationParent, std::string linkTarget,
        std::optional<std::vector<std::uint8_t>> createdContent
    );

  private:
    std::vector<Entry> m_undo;
    std::vector<Entry> m_redo;
    bool m_replaying = false;
  };
} // namespace filemanager
