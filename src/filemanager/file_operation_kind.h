#pragma once

#include "filemanager/file_operation_engine.h"

namespace filemanager {
  enum class FileOperationKind {
    Copy,
    Duplicate,
    Move,
    Rename,
    CreateFolder,
    CreateFile,
    CreateSymlink,
    Trash,
    TrashToHome,
    Delete,
    Restore,
    EmptyWastebin,
    Properties,
    Undo,
    Redo,
    Compress,
    Extract
  };
  inline const char* operationName(FileOperationKind kind) {
    switch (kind) {
    case FileOperationKind::Copy:
      return "Copy";
    case FileOperationKind::Duplicate:
      return "Duplicate";
    case FileOperationKind::Move:
      return "Move";
    case FileOperationKind::Rename:
      return "Rename";
    case FileOperationKind::CreateFolder:
    case FileOperationKind::CreateFile:
      return "Create";
    case FileOperationKind::CreateSymlink:
      return "Create symlink";
    case FileOperationKind::Trash:
      return "Trash";
    case FileOperationKind::TrashToHome:
      return "Move to Home Wastebin";
    case FileOperationKind::Delete:
      return "Permanent delete";
    case FileOperationKind::Restore:
      return "Restore";
    case FileOperationKind::EmptyWastebin:
      return "Empty Wastebin";
    case FileOperationKind::Properties:
      return "Properties";
    case FileOperationKind::Undo:
      return "Undo";
    case FileOperationKind::Redo:
      return "Redo";
    case FileOperationKind::Compress:
      return "Compression";
    case FileOperationKind::Extract:
      return "Extraction";
    }
    return "File operation";
  }

  inline std::optional<file_operation_engine::Kind> localOperationKind(FileOperationKind kind) {
    switch (kind) {
    case FileOperationKind::Copy:
      return file_operation_engine::Kind::Copy;
    case FileOperationKind::Duplicate:
      return file_operation_engine::Kind::Duplicate;
    case FileOperationKind::Move:
      return file_operation_engine::Kind::Move;
    case FileOperationKind::Rename:
      return file_operation_engine::Kind::Rename;
    case FileOperationKind::CreateFolder:
      return file_operation_engine::Kind::CreateFolder;
    case FileOperationKind::CreateFile:
      return file_operation_engine::Kind::CreateFile;
    case FileOperationKind::CreateSymlink:
      return file_operation_engine::Kind::CreateSymlink;
    case FileOperationKind::Delete:
      return file_operation_engine::Kind::Delete;
    default:
      return std::nullopt;
    }
  }

} // namespace filemanager
