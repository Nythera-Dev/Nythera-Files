#pragma once

#include <cstdint>
#include <gio/gio.h>
#include <optional>
#include <string>
#include <string_view>

namespace filemanager {

  enum class SizeComparison { Larger, Smaller, Exactly, Between };
  enum class SizeUnit { Bytes, KB, MB, GB, TB, KiB, MiB, GiB, TiB };
  enum class DateComparison { Within, Older, Before, After, Between };
  enum class DateUnit { Hours, Days, Weeks, Months, Years };
  enum class ContentMode { Plain, Extracted, All };

  struct SearchDate {
    int year = 0;
    int month = 0;
    int day = 0;
    bool operator==(const SearchDate&) const = default;
  };

  struct ContentFilter {
    std::string query;
    ContentMode mode = ContentMode::All;
    bool caseSensitive = false;
    bool regex = false;
    bool operator==(const ContentFilter&) const = default;
  };

  struct SearchFilterDraft {
    SizeComparison sizeComparison = SizeComparison::Larger;
    std::string sizeFirst;
    std::string sizeSecond;
    SizeUnit sizeFirstUnit = SizeUnit::MB;
    SizeUnit sizeSecondUnit = SizeUnit::MB;

    DateComparison dateComparison = DateComparison::Within;
    std::string dateAmount;
    DateUnit dateUnit = DateUnit::Days;
    SearchDate dateFirst;
    SearchDate dateSecond;

    ContentFilter content;
    std::optional<ContentFilter> submitted;
  };

  struct SizeFilter {
    SizeComparison comparison;
    std::uint64_t first;
    std::uint64_t second;
    [[nodiscard]] bool matches(std::uint64_t bytes) const;
    bool operator==(const SizeFilter&) const = default;
  };

  struct ModifiedFilter {
    std::optional<std::int64_t> start;
    std::optional<std::int64_t> end; // exclusive
    [[nodiscard]] bool matches(std::int64_t seconds) const;
    bool operator==(const ModifiedFilter&) const = default;
  };

  struct SearchFilters {
    std::optional<SizeFilter> size;
    std::optional<ModifiedFilter> modified;
    std::optional<ContentFilter> content;
    [[nodiscard]] bool active() const { return size || modified || content; }
  };

  [[nodiscard]] SearchFilters resolveSearchFilters(const SearchFilterDraft& draft, GDateTime* now);
  [[nodiscard]] std::optional<SizeFilter> resolveSizeFilter(const SearchFilterDraft& draft);
  [[nodiscard]] std::optional<ModifiedFilter> resolveModifiedFilter(const SearchFilterDraft& draft, GDateTime* now);
  [[nodiscard]] bool validContentQuery(std::string_view query);

} // namespace filemanager
