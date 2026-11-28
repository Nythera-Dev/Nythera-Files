#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace filemanager {
  struct FileBreadcrumb {
    std::string label;
    std::filesystem::path path;
  };

  [[nodiscard]] inline bool isRecentTabPath(const std::filesystem::path& path) {
    return path == "recent:" || path == "recent:/";
  }

  inline std::vector<FileBreadcrumb> fileBreadcrumbs(const std::filesystem::path& path) {
    const bool trash = path == "trash:" || path.string().starts_with("trash:/");
    const bool recent = isRecentTabPath(path);
    if (recent)
      return {{"Recent", std::filesystem::path("recent:/")}};
    auto normalized = path.lexically_normal();
    std::filesystem::path current = trash ? std::filesystem::path("trash:/") : normalized.root_path();
    std::vector<FileBreadcrumb> crumbs;
    if (!current.empty())
      crumbs.push_back({trash ? "Wastebin" : "/", current});
    for (const auto& part : normalized.relative_path()) {
      // A trailing directory separator is an empty filesystem component, not a
      // destination. The URI scheme is likewise not a child of its virtual root.
      if (part.empty() || part == "." || (trash && part == "trash:"))
        continue;
      current /= part;
      crumbs.push_back({part.string(), current});
    }
    return crumbs;
  }
} // namespace filemanager
