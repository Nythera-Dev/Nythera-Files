// Adapted from Luciole archive_xz_input.h; see the project LICENSE.
#pragma once

#include "archive_input.h"

#include <array>
#include <lzma.h>

namespace filemanager::archive_backend::detail {
  // Decode outside libarchive to validate concatenated frames, integrity checks and trailing padding.
  // Only TAR consumes this stream. Source identity remains owned by Input.
  class XzInput {
  public:
    static bool recognizes(Input& input);
    XzInput(Input& input, Work& work);
    ~XzInput();
    XzInput(const XzInput&) = delete;
    XzInput& operator=(const XzInput&) = delete;
    std::size_t read(std::span<char> buffer);

  private:
    Input& m_input;
    Work& m_work;
    lzma_stream m_stream = LZMA_STREAM_INIT;
    std::array<char, 65536> m_encoded{};
    bool m_eof = false, m_finished = false;
  };
} // namespace filemanager::archive_backend::detail
