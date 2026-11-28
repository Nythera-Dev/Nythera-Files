#pragma once

#include <string>
#include <vector>

namespace filemanager {
  class BrowserTab;

  namespace menu_id {
    inline constexpr int kArchiveCompress = 30420;
    inline constexpr int kArchiveExtract = 30421;
    inline constexpr int kCompressTarGzip = 30422;
    inline constexpr int kCompressZip = 30423;
    inline constexpr int kCompressTo = 30424;
    inline constexpr int kExtractHere = 30425;
    inline constexpr int kExtractFolder = 30426;
    inline constexpr int kExtractTo = 30427;
    inline constexpr int kAvoidRedundantExtractionFolders = 30428;
    inline constexpr int kExtractSelected = 30429;
    inline constexpr int kOpenArchiveMemberWith = 30430;
  } // namespace menu_id

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
