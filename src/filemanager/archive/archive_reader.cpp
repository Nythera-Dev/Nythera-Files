// Reader, sparse extent accounting and catalogue validation adapted from Luciole archive_reader.cpp.
#include "archive_rar_input.h"
#include "archive_xz_input.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <unistd.h>
#include <zlib.h>

namespace {
  using namespace filemanager::archive_backend;
  using namespace filemanager::archive_backend::detail;
  using Archive = struct archive;
  using ReaderHandle = std::unique_ptr<Archive, decltype(&archive_read_free)>;

  class GzipInput {
  public:
    GzipInput(Input& input, Work& work) : m_input(input), m_work(work) {
      if (::inflateInit2(&m_stream, MAX_WBITS + 16) != Z_OK)
        throwFileError(FileErrorKind::Other, "Could not initialise gzip validation.");
    }
    ~GzipInput() { ::inflateEnd(&m_stream); }
    GzipInput(const GzipInput&) = delete;
    GzipInput& operator=(const GzipInput&) = delete;

    std::size_t read(std::span<char> out) {
      m_stream.next_out = reinterpret_cast<Bytef*>(out.data());
      m_stream.avail_out = static_cast<uInt>(out.size());
      while (m_stream.avail_out && !m_finished) {
        m_work.check();
        if (!m_stream.avail_in) {
          m_stream.avail_in = static_cast<uInt>(m_input.read(m_encoded));
          m_stream.next_in = reinterpret_cast<Bytef*>(m_encoded.data());
          if (!m_stream.avail_in) {
            if (!m_atEnd)
              throwFileError(FileErrorKind::InvalidArgument, "Truncated gzip stream.");
            m_finished = true;
            break;
          }
        }
        if (m_atEnd) {
          if (::inflateReset(&m_stream) != Z_OK)
            throwFileError(FileErrorKind::Other, "Could not reset the gzip decoder.");
          m_atEnd = false;
        }
        const auto status = ::inflate(&m_stream, Z_NO_FLUSH);
        if (status == Z_STREAM_END)
          m_atEnd = true;
        else if (status != Z_OK)
          throwFileError(FileErrorKind::InvalidArgument, "Corrupt gzip stream or invalid CRC/ISIZE.");
      }
      return out.size() - m_stream.avail_out;
    }

  private:
    Input& m_input;
    Work& m_work;
    z_stream m_stream{};
    std::array<char, 65536> m_encoded{};
    bool m_atEnd = false, m_finished = false;
  };

  struct Reader {
    Work& work;
    Input input;
    std::optional<Format> expected, detected, outer;
    std::optional<GzipInput> gzip;
    std::optional<XzInput> xz;
    ReaderHandle filter{nullptr, archive_read_free};
    ReaderHandle value{archive_read_new(), archive_read_free};
    std::array<char, 65536> encoded{}, decoded{};
    std::int64_t headerOffset = 0;
    bool sevenZipSignature = false;
    std::string passphrase;
    std::size_t passwordAttempts = 0;

    Reader(const Source& source, Work& request)
        : work(request), input(source, work), expected(formatFromName(source.name)) {
      if (!value)
        throwFileError(FileErrorKind::Other, "Could not allocate an archive reader.");
      std::array<char, 4> signature{};
      const auto count = input.read(signature);
      input.seek(0, SEEK_SET);
      const auto magic = std::string_view(signature.data(), count);
      sevenZipSignature = magic == "7z\xbc\xaf";
      if (magic.starts_with("\x1f\x8b")) {
        outer = Format::TarGzip;
        gzip.emplace(input, work);
      } else if (XzInput::recognizes(input)) {
        outer = Format::TarXz;
        xz.emplace(input, work);
      } else if (magic.starts_with("BZh")) {
        outer = Format::TarBzip2;
        openFilter(archive_read_support_filter_bzip2, ARCHIVE_FILTER_BZIP2);
      } else if (
          magic == "\x28\xb5\x2f\xfd"
          || (magic.size() == 4
              && (static_cast<unsigned char>(magic[0]) & 0xf0U) == 0x50U
              && magic.substr(1) == "\x2a\x4d\x18")
      ) {
        outer = Format::TarZstd;
        openFilter(archive_read_support_filter_zstd, ARCHIVE_FILTER_ZSTD);
      }

      auto* a = value.get();
      if (outer) {
        requireArchive(archive_read_support_format_tar(a), a, work);
        if (outer == Format::TarGzip && expected == Format::Gzip) {
          requireArchive(archive_read_support_format_raw(a), a, work);
          requireArchive(archive_read_support_format_empty(a), a, work);
        }
      } else {
        const auto rar = validateRarInput(input);
        if (rar != RarInput::None) {
          requireArchive(
              rar == RarInput::Rar4 ? archive_read_support_format_rar(a) : archive_read_support_format_rar5(a), a, work
          );
        } else {
          requireArchive(archive_read_support_format_zip_seekable(a), a, work);
          requireArchive(archive_read_support_format_tar(a), a, work);
          requireArchive(archive_read_support_format_7zip(a), a, work);
        }
        requireArchive(archive_read_set_seek_callback(a, seekCallback), a, work);
      }
      if (work.callbacks.password) {
        requireArchive(
            archive_read_set_passphrase_callback(
                a, this,
                [](Archive*, void* context) -> const char* {
                  auto& reader = *static_cast<Reader*>(context);
                  try {
                    reader.work.check();
                    auto answer = reader.work.callbacks.password(reader.passwordAttempts++ > 0);
                    if (!answer || answer->empty())
                      throwFileError(FileErrorKind::Cancelled, "Archive password entry cancelled.");
                    if (answer->contains('\0'))
                      throwFileError(FileErrorKind::InvalidArgument, "Archive passwords cannot contain a NUL byte.");
                    explicit_bzero(reader.passphrase.data(), reader.passphrase.size());
                    reader.passphrase = std::move(*answer);
                    return reader.passphrase.c_str();
                  } catch (...) {
                    reader.work.failure = std::current_exception();
                    return nullptr;
                  }
                }
            ),
            a, work
        );
      }
      requireArchive(archive_read_support_filter_none(a), a, work);
      requireArchive(archive_read_open(a, this, nullptr, readDecoded, nullptr), a, work);
    }

    void openFilter(int (*support)(Archive*), int code) {
      filter.reset(archive_read_new());
      auto* a = filter.get();
      if (!a || !nativeFilter(*outer))
        throwFileError(FileErrorKind::Unsupported, "Native archive filter is unavailable.");
      requireArchive(support(a), a, work);
      requireArchive(archive_read_support_format_raw(a), a, work);
      requireArchive(archive_read_open(a, this, nullptr, readEncoded, nullptr), a, work);
      archive_entry* header = nullptr;
      requireArchive(archive_read_next_header(a, &header), a, work);
      if (archive_filter_count(a) != 2 || archive_filter_code(a, 0) != code)
        throwFileError(FileErrorKind::Unsupported, "Unexpected or nested compression filters.");
    }

    std::size_t decode(std::span<char> buffer) {
      work.check();
      if (gzip)
        return gzip->read(buffer);
      if (xz)
        return xz->read(buffer);
      if (!filter)
        return input.read(buffer);
      const auto size = archive_read_data(filter.get(), buffer.data(), buffer.size());
      if (size < 0)
        archiveError(filter.get(), work);
      return static_cast<std::size_t>(size);
    }

    static la_ssize_t readEncoded(Archive* a, void* context, const void** block) {
      auto& reader = *static_cast<Reader*>(context);
      try {
        *block = reader.encoded.data();
        return static_cast<la_ssize_t>(reader.input.read(reader.encoded));
      } catch (...) {
        reader.work.failure = std::current_exception();
        archive_set_error(a, EIO, "Could not read compressed archive input.");
        return -1;
      }
    }

    static la_ssize_t readDecoded(Archive* a, void* context, const void** block) {
      auto& reader = *static_cast<Reader*>(context);
      try {
        *block = reader.decoded.data();
        return static_cast<la_ssize_t>(reader.decode(reader.decoded));
      } catch (...) {
        reader.work.failure = std::current_exception();
        archive_set_error(a, EIO, "Could not read archive input.");
        return -1;
      }
    }

    static la_int64_t seekCallback(Archive* a, void* context, la_int64_t offset, int whence) {
      auto& reader = *static_cast<Reader*>(context);
      try {
        reader.work.check();
        return reader.input.seek(offset, whence);
      } catch (...) {
        reader.work.failure = std::current_exception();
        archive_set_error(a, EIO, "Could not seek in archive input.");
        return ARCHIVE_FATAL;
      }
    }

    bool next(archive_entry*& entry) {
      work.check();
      const auto before = archive_filter_bytes(value.get(), -1);
      const auto status = archive_read_next_header(value.get(), &entry);
      const int format = archive_format(value.get()) & ARCHIVE_FORMAT_BASE_MASK;
      if (archive_read_has_encrypted_entries(value.get()) > 0
          || (status == ARCHIVE_OK && archive_entry_is_encrypted(entry) > 0)) {
        if ((format == ARCHIVE_FORMAT_7ZIP || sevenZipSignature) && (!expected || *expected == Format::SevenZip))
          throwFileError(FileErrorKind::EncryptedSevenZip, "Encrypted 7z requires the optional 7-Zip executable.");
        if (format != ARCHIVE_FORMAT_ZIP || !work.callbacks.password)
          throwFileError(FileErrorKind::Unsupported, "Encrypted archives are unsupported.");
      }
      passwordAttempts = 0;
      if (work.failure)
        std::rethrow_exception(work.failure);
      if (status != ARCHIVE_EOF)
        requireArchive(status, value.get(), work);

      const int base = archive_format(value.get()) & ARCHIVE_FORMAT_BASE_MASK;
      if (!detected) {
        switch (base) {
        case ARCHIVE_FORMAT_ZIP:
          detected = Format::Zip;
          break;
        case ARCHIVE_FORMAT_7ZIP:
          detected = Format::SevenZip;
          break;
        case ARCHIVE_FORMAT_TAR:
          detected = outer.value_or(Format::Tar);
          break;
        case ARCHIVE_FORMAT_RAR:
        case ARCHIVE_FORMAT_RAR_V5:
          detected = Format::Rar;
          break;
        case ARCHIVE_FORMAT_RAW:
        case ARCHIVE_FORMAT_EMPTY:
          if (outer == Format::TarGzip && expected == Format::Gzip)
            detected = Format::Gzip;
          break;
        default:
          break;
        }
        if (!detected || (expected && detected != expected))
          throwFileError(
              FileErrorKind::Unsupported, "Archive content does not match its format or compression filter."
          );
        const auto all = capabilities();
        const auto cap = std::ranges::find(all, *detected, &Capability::format);
        if (cap == all.end() || !cap->extract)
          throwFileError(FileErrorKind::Unsupported, "Archive extraction format is unavailable.");
      }
      if (status == ARCHIVE_EOF) {
        if (base == ARCHIVE_FORMAT_RAR_V5)
          validateRar5End(input, before, work);
        if (base == ARCHIVE_FORMAT_RAR
            && archive_filter_bytes(value.get(), -1) == before
            && input.seek(0, SEEK_END) != before)
          throwFileError(FileErrorKind::InvalidArgument, "RAR archive has a truncated trailing header.");
        return false;
      }
      headerOffset = before;
      if (base == ARCHIVE_FORMAT_RAR && archive_entry_filetype(entry) == AE_IFLNK)
        verifyRar4Stored(input, headerOffset, work);
      return true;
    }

    void finish() {
      // The TAR reader stops at its end marker. Drain the same decoder to validate its trailer.
      if (outer) {
        while (decode(decoded) != 0) {
        }
      }
      input.unchanged();
    }
  };
  void extractData(Reader& reader, int output, std::optional<std::uint64_t> expected, bool sparse) {
    if ((archive_format(reader.value.get()) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_RAR) {
      verifyRar4Stored(reader.input, reader.headerOffset, reader.work);
    }
    std::uint64_t end = 0;
    for (;;) {
      reader.work.check();
      const void* data = nullptr;
      std::size_t size = 0;
      la_int64_t offset = 0;
      const auto status = archive_read_data_block(reader.value.get(), &data, &size, &offset);
      if (status == ARCHIVE_EOF) {
        break;
      }
      requireArchive(status, reader.value.get(), reader.work);
      if (offset < 0
          || static_cast<std::uint64_t>(offset) < end
          || size > static_cast<std::uint64_t>(INT64_MAX) - static_cast<std::uint64_t>(offset)) {
        throwFileError(FileErrorKind::InvalidArgument, "Archive member has an invalid data extent.");
      }
      if (!sparse && static_cast<std::uint64_t>(offset) != end)
        throwFileError(FileErrorKind::InvalidArgument, "Archive member data has an unexpected gap.");
      end = static_cast<std::uint64_t>(offset) + size;
      std::size_t done = 0;
      while (done < size) {
        const auto count = ::pwrite(
            output, static_cast<const char*>(data) + done, size - done, offset + static_cast<la_int64_t>(done)
        );
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          systemError("Could not write extracted data");
        }
        done += static_cast<std::size_t>(count);
      }
      reader.work.report(size);
    }
    const auto length = expected.value_or(end);
    if ((!sparse && length != end) || length < end || length > static_cast<std::uint64_t>(INT64_MAX))
      throwFileError(FileErrorKind::InvalidArgument, "Archive member size is inconsistent.");
    if (length != end && ::ftruncate(output, static_cast<off_t>(length)) != 0)
      systemError("Could not finish extracted sparse data");
  }

  namespace fs = std::filesystem;

  void makeDirectory(const fs::path& path) {
    if (::mkdir(path.c_str(), 0700) != 0)
      systemError("Could not create staged archive directory");
  }

  void addParents(Entries& entries, const fs::path& root, const std::string& path) {
    const auto parent = parentName(path);
    if (parent.empty())
      return;
    const auto found = entries.find(parent);
    if (found != entries.end()) {
      if (found->second.kind != EntryKind::Directory)
        throwFileError(FileErrorKind::InvalidArgument, "Archive entry traverses a file or link.");
      return;
    }
    addParents(entries, root, parent);
    makeDirectory(root / parent);
    entries.emplace(parent, Entry{.kind = EntryKind::Directory, .mode = 0700});
  }

  // Luciole's space estimate uses only stored extents for sparse members.
  std::uint64_t storageSize(archive_entry* entry, std::optional<std::uint64_t> length, int extents) {
    if (!length || extents <= 0)
      return length.value_or(0);
    std::uint64_t total = 0, end = 0;
    for (int i = 0; i < extents; ++i) {
      la_int64_t offset = 0, size = 0;
      if (archive_entry_sparse_next(entry, &offset, &size) != ARCHIVE_OK
          || offset < 0
          || size < 0
          || static_cast<std::uint64_t>(offset) < end
          || static_cast<std::uint64_t>(offset) > *length
          || static_cast<std::uint64_t>(size) > *length - static_cast<std::uint64_t>(offset))
        throwFileError(FileErrorKind::InvalidArgument, "Archive contains invalid sparse extents.");
      end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(size);
      total += static_cast<std::uint64_t>(size);
    }
    return total;
  }

  void
  stageBody(Reader& reader, const fs::path& path, unsigned mode, std::optional<std::uint64_t> expected, bool sparse) {
    auto output = opened(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (archive_format(reader.value.get()) != ARCHIVE_FORMAT_EMPTY)
      extractData(reader, output.get(), expected, sparse);
    if (::fchmod(output.get(), static_cast<mode_t>(mode)) != 0 || output.close() != 0)
      systemError("Could not finish staged extraction output");
  }

  std::string resolveHardlink(std::string target, const Entries& entries, Work& work) {
    std::set<std::string> seen;
    for (;;) {
      work.check();
      const auto found = entries.find(target);
      if (found == entries.end() || !seen.insert(target).second)
        break;
      if (found->second.kind == EntryKind::File)
        return target;
      if (found->second.kind != EntryKind::Hardlink)
        break;
      target = found->second.link;
    }
    throwFileError(FileErrorKind::InvalidArgument, "Archive hard link does not resolve to a regular archive member.");
  }
} // namespace

namespace filemanager::archive_backend::detail {
  // Catalogue and hard-link selection adapted from Luciole archive_reader.cpp (MIT).
  Entries inspectArchive(const Source& source, Work& work) {
    Entries entries;
    std::map<std::string, std::string> links;
    Reader reader(source, work);
    archive_entry* header = nullptr;
    while (reader.next(header)) {
      work.item();
      const auto type = archive_entry_filetype(header);
      const auto name = reader.detected == Format::Gzip ? safePath(fs::path(source.name).stem().native())
                                                        : safePath(bounded(archive_entry_pathname(header)));
      if (name.empty()) {
        if (type == AE_IFDIR)
          continue;
        throwFileError(FileErrorKind::InvalidArgument, "Archive member has an empty name.");
      }
      for (auto parent = parentName(name); !parent.empty(); parent = parentName(parent)) {
        if (entries.try_emplace(parent, Entry{.kind = EntryKind::Directory, .mode = 0755}).first->second.kind
            != EntryKind::Directory)
          throwFileError(FileErrorKind::InvalidArgument, "Archive entry traverses a file or link.");
      }
      const unsigned mode = archive_entry_perm(header) & 0777U;
      Entry entry{.mode = 0600U | (mode & 0155U)};
      if (const auto* link = archive_entry_hardlink(header)) {
        entry.kind = EntryKind::Hardlink;
        entry.link = safePath(bounded(link));
      } else if (type == AE_IFLNK) {
        entry.kind = EntryKind::Symlink;
        entry.link = bounded(archive_entry_symlink(header));
        checkLinkTarget(name, entry.link);
        links.emplace(name, entry.link);
      } else if (type == AE_IFDIR) {
        entry.kind = EntryKind::Directory;
        entry.mode = 0700U | (mode & 0055U);
      } else if (type != AE_IFREG) {
        throwFileError(FileErrorKind::Unsupported, "Archive contains an unsafe special file.");
      } else if (reader.detected != Format::Gzip && archive_entry_size_is_set(header)) {
        const auto size = archive_entry_size(header);
        if (size < 0)
          throwFileError(FileErrorKind::InvalidArgument, "Archive member has an invalid size.");
        entry.size = size;
      }
      if (archive_entry_mtime_is_set(header))
        entry.modified = archive_entry_mtime(header);
      const auto [existing, inserted] = entries.try_emplace(name, entry);
      if (!inserted && (existing->second.kind != EntryKind::Directory || entry.kind != EntryKind::Directory))
        throwFileError(FileErrorKind::InvalidArgument, "Duplicate or conflicting archive member.");
      existing->second = std::move(entry);
      requireArchive(archive_read_data_skip(reader.value.get()), reader.value.get(), work);
    }
    reader.finish();
    if (reader.detected == Format::Gzip && entries.empty())
      entries.emplace(safePath(fs::path(source.name).stem().native()), Entry{.size = 0});
    validateLinks(links, work);
    for (auto& [name, entry] : entries)
      if (entry.kind == EntryKind::Hardlink)
        entry.size = entries.at(resolveHardlink(entry.link, entries, work)).size;
    return entries;
  }

  void
  readSelected(const Source& source, const Catalogue& catalogue, const Projection& items, Stage& stage, Work& work) {
    std::map<std::string, std::vector<std::string>, std::less<>> bodies;
    std::map<std::string, std::string> links;
    for (const auto& [name, output] : items) {
      const auto& entry = catalogue.entries.at(name);
      work.item();
      switch (entry.kind) {
      case EntryKind::Directory:
        makeDirectory(stage.path / output);
        break;
      case EntryKind::Symlink:
        links.emplace(output, entry.link);
        break;
      case EntryKind::Hardlink:
        bodies[resolveHardlink(entry.link, catalogue.entries, work)].push_back(output);
        break;
      case EntryKind::File:
        bodies[name].push_back(output);
        break;
      }
    }
    validateLinks(links, work);
    auto remaining = availableSpace(stage.directory.get());
    Reader reader(source, work);
    archive_entry* header = nullptr;
    const auto write = [&](const std::string& name, auto found, bool sparse) {
      const auto& entry = catalogue.entries.at(name);
      const auto saved = stage.path / found->second.front();
      const auto estimate = header ? storageSize(header, entry.size, archive_entry_sparse_reset(header)) : 0;
      if (estimate > remaining && estimate > (remaining = availableSpace(stage.directory.get())))
        throwFileError(FileErrorKind::NoSpace, "Not enough space for the staged archive member.");
      const auto before = work.bytes;
      stageBody(reader, saved, entry.mode, entry.size, sparse);
      remaining -= std::min(remaining, work.bytes - before);
      for (const auto& alias : std::span(found->second).subspan(1))
        if (::link(saved.c_str(), (stage.path / alias).c_str()) != 0)
          systemError("Could not stage an archive hard link");
      bodies.erase(found);
    };
    while (reader.next(header)) {
      const auto name = reader.detected == Format::Gzip ? safePath(fs::path(source.name).stem().native())
                                                        : safePath(bounded(archive_entry_pathname(header)));
      const auto found = bodies.find(name);
      if (found != bodies.end())
        write(name, found, archive_entry_sparse_reset(header) > 0);
      else
        requireArchive(archive_read_data_skip(reader.value.get()), reader.value.get(), work);
    }
    reader.finish();
    if (reader.detected == Format::Gzip && !bodies.empty()) {
      header = nullptr;
      write(bodies.begin()->first, bodies.begin(), false);
    }
    if (!bodies.empty())
      throwFileError(FileErrorKind::Changed, "An archive selection no longer exists.");
    for (const auto& [output, target] : links)
      if (::symlink(target.c_str(), (stage.path / output).c_str()) != 0)
        systemError("Could not stage an archive symbolic link");
    for (auto i = items.rbegin(); i != items.rend(); ++i) {
      const auto& entry = catalogue.entries.at(i->first);
      if (entry.kind == EntryKind::Directory && ::chmod((stage.path / i->second).c_str(), entry.mode) != 0)
        systemError("Could not set staged archive directory permissions");
    }
  }

  void readArchive(const std::filesystem::path& path, const Source& source, Stage& stage, Work& work) {
    Entries entries;
    std::map<std::string, std::string> links;
    Reader reader(source, work);
    const auto& root = stage.path;
    auto space = availableSpace(stage.directory.get());
    archive_entry* header = nullptr;
    while (reader.next(header)) {
      work.item();
      const auto type = archive_entry_filetype(header);
      const auto name = reader.detected == Format::Gzip ? safePath(path.stem().native())
                                                        : safePath(bounded(archive_entry_pathname(header)));
      if (name.empty()) {
        if (type == AE_IFDIR)
          continue;
        throwFileError(FileErrorKind::InvalidArgument, "Archive member has an empty name.");
      }
      addParents(entries, root, name);
      const unsigned mode = archive_entry_perm(header) & 0777U;
      Entry entry{.kind = EntryKind::File, .mode = 0600U | (mode & 0155U)};
      if (const auto* link = archive_entry_hardlink(header)) {
        entry.kind = EntryKind::Hardlink;
        entry.link = safePath(bounded(link));
      } else if (type == AE_IFLNK) {
        entry.kind = EntryKind::Symlink;
        entry.link = bounded(archive_entry_symlink(header));
        checkLinkTarget(name, entry.link);
        links.emplace(name, entry.link);
      } else if (type == AE_IFDIR) {
        entry.kind = EntryKind::Directory;
        entry.mode = 0700U | (mode & 0055U);
      } else if (type != AE_IFREG) {
        throwFileError(FileErrorKind::Unsupported, "Archive contains an unsafe special file.");
      }
      auto [position, inserted] = entries.try_emplace(name, entry);
      if (!inserted && (entry.kind != EntryKind::Directory || position->second.kind != EntryKind::Directory))
        throwFileError(FileErrorKind::InvalidArgument, "Duplicate or conflicting archive member.");
      if (entry.kind == EntryKind::Directory && inserted)
        makeDirectory(root / name);
      if (entry.kind == EntryKind::File) {
        std::optional<std::uint64_t> expected;
        if (reader.detected != Format::Gzip && archive_entry_size_is_set(header)) {
          const auto size = archive_entry_size(header);
          if (size < 0)
            throwFileError(FileErrorKind::InvalidArgument, "Archive member has an invalid size.");
          expected = static_cast<std::uint64_t>(size);
        }
        const auto extents = archive_entry_sparse_reset(header);
        const auto estimate = storageSize(header, expected, extents);
        if (estimate > space) {
          space = availableSpace(stage.directory.get());
          if (estimate > space)
            throwFileError(FileErrorKind::NoSpace, "Not enough space for the staged archive member.");
        }
        const auto before = work.bytes;
        stageBody(reader, root / name, entry.mode, expected, extents > 0);
        space -= std::min(space, work.bytes - before);
      }
      position->second = std::move(entry);
    }
    reader.finish();
    if (reader.detected == Format::Gzip && entries.empty()) {
      const auto name = safePath(path.stem().native());
      if (name.empty())
        throwFileError(FileErrorKind::InvalidArgument, "Gzip output has no filename.");
      work.item();
      stageBody(reader, root / name, 0600, 0, false);
    }

    // As in Luciole, validate the complete link graph and create links after all file bodies.
    validateLinks(links, work);
    for (const auto& [name, entry] : entries) {
      work.check();
      if (entry.kind == EntryKind::Directory && ::chmod((root / name).c_str(), entry.mode) != 0)
        systemError("Could not set staged archive directory permissions");
      if (entry.kind == EntryKind::Hardlink) {
        const auto body = resolveHardlink(entry.link, entries, work);
        if (::link((root / body).c_str(), (root / name).c_str()) != 0)
          systemError("Could not stage an archive hard link");
      }
    }
    for (const auto& [name, target] : links) {
      work.check();
      if (::symlink(target.c_str(), (root / name).c_str()) != 0)
        systemError("Could not stage an archive symbolic link");
    }
  }
} // namespace filemanager::archive_backend::detail
