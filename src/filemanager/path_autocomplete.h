#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {

  struct PathAutocompleteResult {
    std::vector<std::string> suggestions;
    std::string commonPrefix;
  };

  [[nodiscard]] PathAutocompleteResult
  completeDirectoryPath(std::string_view input, const std::filesystem::path& homeDir);

} // namespace filemanager
