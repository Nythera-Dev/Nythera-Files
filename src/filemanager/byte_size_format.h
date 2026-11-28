#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace filemanager {

  struct ScaledByteSize {
    double value = 0.0;
    std::string_view unit;
    std::size_t unitIndex = 0;
  };

  /// Scales a byte count into binary (1024-based) IEC units up to maxUnitIndex (0=B .. 6=EiB).
  [[nodiscard]] inline ScaledByteSize scaleBinaryByteSize(std::uint64_t bytes, std::size_t maxUnitIndex = 6) {
    constexpr std::array<std::string_view, 7> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    const std::size_t limit = maxUnitIndex < units.size() ? maxUnitIndex : units.size() - 1;
    double value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= 1024.0 && index < limit) {
      value /= 1024.0;
      ++index;
    }
    return {value, units[index], index};
  }

  /// Rounds a binary-scaled size to 1 decimal below 10 and whole units at or above 10.
  [[nodiscard]] inline ScaledByteSize compactBinaryByteSize(std::uint64_t bytes) {
    auto scaled = scaleBinaryByteSize(bytes, 6);
    if (scaled.value >= 10.0)
      scaled.value = std::round(scaled.value);
    else
      scaled.value = std::round(scaled.value * 10.0) / 10.0;
    return scaled;
  }

  /// Formats a compact binary size, omitting ".0" for whole numbers.
  [[nodiscard]] inline std::string formatCompactBinaryByteSize(std::uint64_t bytes) {
    const auto scaled = compactBinaryByteSize(bytes);
    std::ostringstream out;
    out
        << std::fixed
        << std::setprecision(scaled.value == std::round(scaled.value) ? 0 : 1)
        << scaled.value
        << ' '
        << scaled.unit;
    return out.str();
  }

  /// Formats a binary size with 0 decimals for bytes and 1 decimal for KiB..TiB.
  [[nodiscard]] inline std::string formatFixedBinaryByteSize(std::uint64_t bytes) {
    const auto scaled = scaleBinaryByteSize(bytes, 4);
    std::ostringstream out;
    out << std::fixed << std::setprecision(scaled.unitIndex == 0 ? 0 : 1) << scaled.value << ' ' << scaled.unit;
    return out.str();
  }

  /// Formats a binary size up to PiB, rounding values >= 10 or bytes to whole units.
  [[nodiscard]] inline std::string formatAdaptiveBinaryByteSize(std::uint64_t bytes) {
    const auto scaled = scaleBinaryByteSize(bytes, 5);
    std::ostringstream out;
    if (scaled.unitIndex == 0 || scaled.value >= 10.0)
      out << static_cast<std::uint64_t>(std::llround(scaled.value)) << ' ' << scaled.unit;
    else
      out << std::fixed << std::setprecision(1) << scaled.value << ' ' << scaled.unit;
    return out.str();
  }

} // namespace filemanager
