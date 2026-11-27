// Adapted from Luciole archive_xz_input.cpp; see the project LICENSE.
#include "archive_xz_input.h"

#include <cstdio>
#include <limits>

namespace {
  [[noreturn]] void decodeError(lzma_ret status) {
    using namespace filemanager::archive_backend::detail;
    if (status == LZMA_OPTIONS_ERROR || status == LZMA_UNSUPPORTED_CHECK) {
      throwFileError(FileErrorKind::Unsupported, "XZ archive uses unsupported compression or integrity checks.");
    }
    if (status == LZMA_MEM_ERROR) {
      throwFileError(FileErrorKind::Other, "Could not allocate memory for the XZ decoder.");
    }
    throwFileError(FileErrorKind::InvalidArgument, "XZ archive is corrupt or truncated.");
  }
} // namespace

bool filemanager::archive_backend::detail::XzInput::recognizes(Input& input) {
  std::array<char, 6> signature{};
  const auto count = input.read(signature);
  input.seek(0, SEEK_SET);
  constexpr std::array<char, 6> magic{static_cast<char>(0xFD), '7', 'z', 'X', 'Z', 0};
  return count == signature.size() && signature == magic;
}
filemanager::archive_backend::detail::XzInput::XzInput(Input& input, Work& work) : m_input(input), m_work(work) {
  const auto status = lzma_stream_decoder(
      &m_stream, std::numeric_limits<std::uint64_t>::max(), LZMA_CONCATENATED | LZMA_TELL_UNSUPPORTED_CHECK
  );
  if (status != LZMA_OK) {
    lzma_end(&m_stream);
    decodeError(status);
  }
}
filemanager::archive_backend::detail::XzInput::~XzInput() { lzma_end(&m_stream); }
std::size_t filemanager::archive_backend::detail::XzInput::read(std::span<char> buffer) {
  m_work.check();
  if (m_finished || buffer.empty()) {
    return 0;
  }
  m_stream.next_out = reinterpret_cast<std::uint8_t*>(buffer.data());
  m_stream.avail_out = buffer.size();
  while (m_stream.avail_out != 0 && !m_finished) {
    m_work.check();
    if (m_stream.avail_in == 0 && !m_eof) {
      m_stream.avail_in = m_input.read(m_encoded);
      m_stream.next_in = reinterpret_cast<const std::uint8_t*>(m_encoded.data());
      m_eof = m_stream.avail_in == 0;
    }
    const auto status = lzma_code(&m_stream, m_eof ? LZMA_FINISH : LZMA_RUN);
    if (status == LZMA_STREAM_END) {
      m_finished = true;
    } else if (status != LZMA_OK) {
      decodeError(status);
    }
  }
  const auto produced = buffer.size() - m_stream.avail_out;
  if (m_finished) {
    lzma_end(&m_stream);
  }
  return produced;
}
