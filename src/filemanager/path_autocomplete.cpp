#include "filemanager/path_autocomplete.h"

#include <algorithm>
#include <system_error>

namespace filemanager {

  namespace {

    std::string longestCommonPrefix(const std::vector<std::string>& items) {
      if (items.empty()) {
        return {};
      }
      std::string prefix = items.front();
      for (std::size_t i = 1; i < items.size() && !prefix.empty(); ++i) {
        const auto& other = items[i];
        std::size_t len = 0;
        const std::size_t maxLen = std::min(prefix.size(), other.size());
        while (len < maxLen && prefix[len] == other[len]) {
          ++len;
        }
        prefix.resize(len);
      }
      return prefix;
    }

  } // namespace

  PathAutocompleteResult completeDirectoryPath(std::string_view input, const std::filesystem::path& homeDir) {
    PathAutocompleteResult result;
    if (input.empty()) {
      return result;
    }

    std::string expanded(input);
    if (expanded == "~") {
      expanded = homeDir.string();
      if (!expanded.ends_with('/')) {
        expanded.push_back('/');
      }
    } else if (expanded.starts_with("~/") && !homeDir.empty()) {
      expanded = homeDir.string() + expanded.substr(1);
    }

    std::filesystem::path scanDir;
    std::string namePrefix;
    if (expanded.back() == '/') {
      scanDir = std::filesystem::path(expanded);
    } else {
      const std::filesystem::path p(expanded);
      scanDir = p.has_parent_path() ? p.parent_path() : std::filesystem::path("/");
      namePrefix = p.filename().string();
    }

    if (scanDir.empty() || !scanDir.is_absolute()) {
      return result;
    }

    const bool includeHidden = !namePrefix.empty() && namePrefix.front() == '.';
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(
             scanDir, std::filesystem::directory_options::skip_permission_denied, ec
         );
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
      const auto& entry = *it;
      std::error_code typeError;
      if (!entry.is_directory(typeError))
        continue;
      const std::string name = entry.path().filename().string();
      if (name.empty() || (!includeHidden && name.front() == '.')) {
        continue;
      }
      if (!name.starts_with(namePrefix)) {
        continue;
      }
      std::string full = (scanDir / name).lexically_normal().string();
      if (!full.ends_with('/')) {
        full.push_back('/');
      }
      result.suggestions.push_back(std::move(full));
    }

    result.commonPrefix = longestCommonPrefix(result.suggestions);
    std::ranges::sort(result.suggestions);
    if (result.suggestions.size() > 50) {
      result.suggestions.resize(50);
    }
    return result;
  }

} // namespace filemanager
