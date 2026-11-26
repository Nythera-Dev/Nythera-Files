// Adapted from Luciole src/fs/name_matching.h (cb6375a5e046).
// Copyright (c) 2026 noctalia-dev. Distributed under the MIT License in LICENSE.
#pragma once

#include <algorithm>
#include <glib.h>
#include <string>

namespace filemanager {

  inline std::string foldSearchName(std::string text) {
    if (std::ranges::all_of(text, [](unsigned char ch) { return ch != 0 && ch < 0x80; })) {
      for (auto& ch : text)
        ch = g_ascii_tolower(ch);
      return text;
    }

    g_autofree char* valid = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
    g_autofree char* lower = g_utf8_casefold(valid, -1);
    g_autofree char* normalized = g_utf8_normalize(lower, -1, G_NORMALIZE_DEFAULT_COMPOSE);
    return normalized;
  }

} // namespace filemanager
