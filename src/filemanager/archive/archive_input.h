// Single-file adaptation of Luciole archive_input.h; see the project LICENSE.
#pragma once

#include "archive_safety.h"

namespace filemanager::archive_backend::detail {
  class Input {
  public:
    Input(const Source& source, Work& work) : m_source(source), m_work(work) {}
    std::size_t read(std::span<char> buffer);
    std::int64_t seek(std::int64_t offset, int whence);
    void unchanged() const;

  private:
    const Source& m_source;
    Work& m_work;
    std::int64_t m_position = 0;
  };
} // namespace filemanager::archive_backend::detail
