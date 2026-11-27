#include "filemanager/search_filters.h"

#include <array>
#include <charconv>
#include <limits>

namespace {
  using namespace filemanager;

  template <typename Integer> std::optional<Integer> unsignedNumber(std::string_view value) {
    Integer number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size())
      return {};
    return number;
  }

  std::optional<std::uint64_t> bytes(std::string_view number, SizeUnit unit) {
    constexpr std::array<std::uint64_t, 9> multipliers{
        1, 1'000, 1'000'000, 1'000'000'000, 1'000'000'000'000, 1ULL << 10, 1ULL << 20, 1ULL << 30, 1ULL << 40,
    };
    const auto multiplier = multipliers[static_cast<std::size_t>(unit)];
    const auto point = number.find('.');
    const auto whole = unsignedNumber<std::uint64_t>(number.substr(0, point));
    if (!whole || *whole > std::numeric_limits<std::uint64_t>::max() / multiplier)
      return {};

    std::uint64_t fraction = 0;
    if (point != number.npos) {
      const auto digits = number.substr(point + 1);
      const auto numerator = unsignedNumber<std::uint64_t>(digits);
      if (!numerator || digits.size() > 12)
        return {};
      std::uint64_t denominator = 1;
      for (std::size_t index = 0; index < digits.size(); ++index)
        denominator *= 10;
      const auto scaled = static_cast<__uint128_t>(*numerator) * multiplier;
      if (scaled % denominator != 0)
        return {};
      fraction = static_cast<std::uint64_t>(scaled / denominator);
    }
    const auto integral = *whole * multiplier;
    if (fraction > std::numeric_limits<std::uint64_t>::max() - integral)
      return {};
    return integral + fraction;
  }

  GDateTime* midnight(GDateTime* now, SearchDate date) {
    if (date.year < 1
        || date.year > 9999
        || date.month < 1
        || date.month > 12
        || date.day < 1
        || date.day > 31
        || !g_date_valid_dmy(
            static_cast<GDateDay>(date.day), static_cast<GDateMonth>(date.month), static_cast<GDateYear>(date.year)
        ))
      return nullptr;
    return g_date_time_new(g_date_time_get_timezone(now), date.year, date.month, date.day, 0, 0, 0);
  }

  GDateTime* relativeCutoff(GDateTime* now, DateUnit unit, int amount) {
    switch (unit) {
    case DateUnit::Hours:
      return g_date_time_add_hours(now, -amount);
    case DateUnit::Days:
      return g_date_time_add_days(now, -amount);
    case DateUnit::Weeks:
      return g_date_time_add_weeks(now, -amount);
    case DateUnit::Months:
      return g_date_time_add_months(now, -amount);
    case DateUnit::Years:
      return g_date_time_add_years(now, -amount);
    }
    return nullptr;
  }
} // namespace

namespace filemanager {

  bool SizeFilter::matches(std::uint64_t bytes) const {
    switch (comparison) {
    case SizeComparison::Larger:
      return bytes > first;
    case SizeComparison::Smaller:
      return bytes < first;
    case SizeComparison::Exactly:
      return bytes == first;
    case SizeComparison::Between:
      return bytes >= first && bytes <= second;
    }
    return false;
  }

  bool ModifiedFilter::matches(std::int64_t seconds) const {
    return (!start || seconds >= *start) && (!end || seconds < *end);
  }

  std::optional<SizeFilter> resolveSizeFilter(const SearchFilterDraft& draft) {
    const auto first = bytes(draft.sizeFirst, draft.sizeFirstUnit);
    const auto second =
        draft.sizeComparison == SizeComparison::Between ? bytes(draft.sizeSecond, draft.sizeSecondUnit) : first;
    if (!first || !second || *first > *second)
      return {};
    return SizeFilter{draft.sizeComparison, *first, *second};
  }

  std::optional<ModifiedFilter> resolveModifiedFilter(const SearchFilterDraft& draft, GDateTime* now) {
    using enum DateComparison;
    const auto comparison = draft.dateComparison;
    if (comparison == Within || comparison == Older) {
      const auto amount = unsignedNumber<unsigned int>(draft.dateAmount);
      if (!amount || *amount == 0 || *amount > 1'000'000)
        return {};
      g_autoptr(GDateTime) cutoff = relativeCutoff(now, draft.dateUnit, static_cast<int>(*amount));
      if (!cutoff)
        return {};
      const auto seconds = g_date_time_to_unix(cutoff);
      if (comparison == Older)
        return ModifiedFilter{{}, seconds};
      return ModifiedFilter{seconds, g_date_time_to_unix(now) + 1};
    }

    g_autoptr(GDateTime) first = midnight(now, draft.dateFirst);
    if (!first)
      return {};
    if (comparison == Before)
      return ModifiedFilter{{}, g_date_time_to_unix(first)};
    if (comparison == After) {
      g_autoptr(GDateTime) next = g_date_time_add_days(first, 1);
      if (next)
        return ModifiedFilter{g_date_time_to_unix(next), {}};
      return {};
    }
    g_autoptr(GDateTime) second = midnight(now, draft.dateSecond);
    if (!second || g_date_time_compare(first, second) > 0)
      return {};
    g_autoptr(GDateTime) end = g_date_time_add_days(second, 1);
    if (!end)
      return {};
    return ModifiedFilter{g_date_time_to_unix(first), g_date_time_to_unix(end)};
  }

  bool validContentQuery(std::string_view query) {
    return !query.empty() && query.size() <= 8192 && !query.contains('\0') && !query.contains('\n');
  }

  SearchFilters resolveSearchFilters(const SearchFilterDraft& draft, GDateTime* now) {
    return {
        resolveSizeFilter(draft),
        resolveModifiedFilter(draft, now),
        !draft.content.query.empty() && draft.submitted && validContentQuery(draft.submitted->query) ? draft.submitted
                                                                                                     : std::nullopt,
    };
  }
} // namespace filemanager
