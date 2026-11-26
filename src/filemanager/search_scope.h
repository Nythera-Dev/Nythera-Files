#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace filemanager {

  enum class SearchScope { Recursive, Folder, Home, Local, All, Custom };
  inline constexpr std::array<std::string_view, 6> kSearchScopeLabels{
      "Current folder & subfolders", "Current folder only", "Home", "Local drives", "All drives", "Custom searches"
  };

  using SearchLocations = std::vector<std::filesystem::path>;

  struct SearchOptions {
    bool showHidden = false;
    bool followSymlinks = false;
    bool operator==(const SearchOptions&) const = default;
  };

  struct CustomSearch {
    std::string name;
    SearchLocations included;
    SearchLocations excluded;
    bool includeMounts = false;
    SearchOptions options;
    bool operator==(const CustomSearch&) const = default;
  };

  struct SearchSelection {
    SearchScope scope = SearchScope::Recursive;
    bool includeSystem = false;
    std::string profile;
    bool operator==(const SearchSelection&) const = default;
  };

  struct SearchMount {
    enum class Kind { Local, Network, Other, Automount, Blocked };
    std::filesystem::path path;
    std::filesystem::path filesystemPath;
    std::string device;
    Kind kind;
  };

  struct SearchRoot {
    std::filesystem::path path;
    bool network = false;
    std::string device;
    // Implicit nested mounts must obey the same hidden-ancestor rules as traversal.
    std::filesystem::path visibleFrom;
  };

  struct SearchPathHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view path) const noexcept { return std::hash<std::string_view>{}(path); }
  };

  using SearchPathSet = std::unordered_set<std::string, SearchPathHash, std::equal_to<>>;

  struct SearchPlan {
    std::vector<SearchRoot> roots;
    std::vector<SearchMount> nestedMounts;
    SearchPathSet boundaries;
    SearchPathSet exclusions;
    SearchPathSet suppressedMounts;
    bool recursive = true;
    bool followSymlinks = false;
    bool reportRootError = false;
  };

  [[nodiscard]] std::string trimSearchText(std::string_view text);
  [[nodiscard]] SearchLocations searchLocationLines(std::string_view text);
  [[nodiscard]] bool validSearchLocation(const std::filesystem::path& path);
  [[nodiscard]] bool validCustomSearchName(std::string_view name);
  [[nodiscard]] bool explicitlyIncludesRoot(const SearchLocations& paths);
  [[nodiscard]] std::vector<SearchMount> readSearchMounts(std::istream& input);
  [[nodiscard]] std::vector<SearchMount> discoverSearchMounts();
  [[nodiscard]] bool searchPathExcluded(const SearchPlan& plan, const std::filesystem::path& path);
  [[nodiscard]] int openSearchDescendant(int root, const char* relative, unsigned long flags);

  struct SearchPathBoundary {
    const std::filesystem::path& root;
    int fd;
    std::function<bool(const std::filesystem::path&, int)> allowChild;
  };

  // Pin each component before advancing so an untriggered automount is never entered.
  class OpenSearchPath {
  public:
    OpenSearchPath(
        const SearchPlan& plan, const std::filesystem::path& path, const SearchPathBoundary* boundary = nullptr
    );
    ~OpenSearchPath();
    OpenSearchPath(const OpenSearchPath&) = delete;
    OpenSearchPath& operator=(const OpenSearchPath&) = delete;

    [[nodiscard]] int fd() const { return m_fd; }
    [[nodiscard]] const std::filesystem::path& resolved() const { return m_resolved; }
    [[nodiscard]] int release();
    [[nodiscard]] static std::filesystem::path fdPath(int fd);

  private:
    int m_fd = -1;
    std::filesystem::path m_resolved;
  };
  [[nodiscard]] SearchPlan resolveSearchScope(
      const SearchSelection& selection, const std::filesystem::path& current, const std::filesystem::path& home,
      const CustomSearch* profile, const SearchLocations& globalExclusions, const std::vector<SearchMount>& mounts,
      bool followSymlinks = false
  );

} // namespace filemanager
