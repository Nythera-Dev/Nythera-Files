#pragma once

#include <array>
#include <cstddef>
#include <gio/gio.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {

  enum class SearchMode { Contiguous, Fuzzy, StartsWith, EndsWith, Exclude, Exact, None };
  inline constexpr std::array<std::string_view, 7> kSearchModeLabels{"Contains",  "Fuzzy",   "Starts with",
                                                                     "Ends with", "Exclude", "Exact match",
                                                                     "None"};

  [[nodiscard]] SearchMode searchModeFromConfig(std::string_view value);
  [[nodiscard]] std::optional<SearchMode> manualSearchMode(std::string_view query);
  [[nodiscard]] std::string findFzf();

  struct FzfSearch {
    std::string executable;
    SearchMode mode = SearchMode::Contiguous;
    bool manual = false;
  };

  // A null result indicates failure or cancellation. Returned indices only refer
  // to the supplied filenames, regardless of how the child formats its output.
  [[nodiscard]] std::optional<std::vector<std::size_t>> filterWithFzf(
      const FzfSearch& options, std::string_view query, std::span<const std::string_view> names, GCancellable* cancel
  );

} // namespace filemanager
