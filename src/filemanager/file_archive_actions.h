#pragma once

#include "filemanager/menu_ids.h"

#include <string>
#include <vector>

namespace filemanager {
  class BrowserTab;

  struct ArchiveSelection {
    std::string baseName;
    bool canCompress = false;
    bool canExtract = false;
    bool canGzip = false;
    bool canZip = false;
    bool canTarGzip = false;
    bool canWriteHere = false;
  };
  ArchiveSelection archiveSelection(const BrowserTab& tab, const std::vector<std::string>& paths);
} // namespace filemanager
