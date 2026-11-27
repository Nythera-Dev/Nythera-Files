// Adapted from Luciole (noctalia-dev), MIT. See LICENSE.luciole.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <gio/gio.h>
#include <string>
#include <vector>

namespace filemanager::preview {
  // Private writable memory per helper. This, not a pixel count, bounds how large a source image can be decoded.
  inline constexpr std::uint64_t kProviderMemoryBytes = 768U * 1024U * 1024U;
  // Worker-only execution with descriptor 3 as input and CPU, memory, time and file limits.
  // Optional stdout capture duplicates the supplied descriptor; the caller retains ownership.
  std::expected<void, std::string> runProviderProcess(
      std::vector<std::string> args, int input, const std::function<bool()>& canceled,
      std::uint64_t fileBytes = 8U * 1024U * 1024U, unsigned maxCpus = 0, int output = -1,
      std::chrono::milliseconds timeout = std::chrono::seconds(8)
  );
} // namespace filemanager::preview
