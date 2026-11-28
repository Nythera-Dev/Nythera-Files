// Adapted from Luciole archive_writer.cpp; see the project LICENSE.
#include "archive_safety.h"

#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <unistd.h>

namespace {
  using namespace filemanager::archive_backend;
  using namespace filemanager::archive_backend::detail;
  void forEachChild(int fd, Work& work, const std::function<void(const std::string&)>& visit) {
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
      systemError("Could not inspect archive source");
    }
    struct CloseDirectory {
      void operator()(DIR* value) const { ::closedir(value); }
    };
    const std::unique_ptr<DIR, CloseDirectory> stream(::fdopendir(duplicate));
    if (!stream) {
      ::close(duplicate);
      systemError("Could not inspect archive source");
    }
    errno = 0;
    while (const auto* entry = ::readdir(stream.get())) {
      work.check();
      if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..") {
        visit(entry->d_name);
      }
      errno = 0;
    }
    if (errno) {
      systemError("Could not enumerate archive source");
    }
  }
  struct Writer {
    Work& work;
    Fd output;
    struct stat stageIdentity;
    // One buffer per job, rather than a large stack frame for every nested folder.
    std::array<char, 65536> buffer{};
    bool finished = false;
    std::unique_ptr<struct archive, decltype(&archive_write_free)> value{archive_write_new(), &archive_write_free};
    Writer(int directory, Format format, Level level, Work& request, const EncryptionOptions& options)
        : work(request),
          output(opened(::openat(directory, "output", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600))),
          stageIdentity(statFd(directory)) {
      if (!value)
        throwFileError(FileErrorKind::Other, "Could not allocate an archive writer.");
      requireArchive(setFormat(value.get(), format, level, options.method), value.get(), work);
      if (!options.password.empty()) {
        requireArchive(archive_write_set_format_option(value.get(), "zip", "encryption", "aes256"), value.get(), work);
        requireArchive(archive_write_set_passphrase(value.get(), options.password.c_str()), value.get(), work);
      }
      requireArchive(addFilter(value.get(), format, level), value.get(), work);
      requireArchive(archive_write_set_bytes_per_block(value.get(), 0), value.get(), work);
      requireArchive(
          archive_write_open(
              value.get(), this, nullptr,
              [](struct archive* a, void* data, const void* block, std::size_t size) -> la_ssize_t {
                auto& writer = *static_cast<Writer*>(data);
                try {
                  writer.work.check();
                  std::size_t done = 0;
                  while (done < size) {
                    const auto count =
                        ::write(writer.output.get(), static_cast<const char*>(block) + done, size - done);
                    if (count < 0 && errno == EINTR) {
                      continue;
                    }
                    if (count <= 0) {
                      systemError("Could not write compressed archive");
                    }
                    done += static_cast<std::size_t>(count);
                  }
                  return static_cast<la_ssize_t>(size);
                } catch (...) {
                  writer.work.failure = std::current_exception();
                  archive_set_error(a, EIO, "Archive write failed");
                  return -1;
                }
              },
              nullptr
          ),
          value.get(), work
      );
    }
    ~Writer() {
      if (!finished && value)
        archive_write_fail(value.get());
    }
  };
  void compressItem(
      int parent, const std::string& name, const std::string& path, Writer& writer, std::set<std::string>& names
  ) {
    auto& work = writer.work;
    work.item();
    if (path.size() > 4096 || !names.insert(path).second) {
      throwFileError(FileErrorKind::Unsupported, "Archive paths are duplicated or too long.");
    }
    (void)safePath(path);
    const auto before = statAt(parent, name);
    if (sameIdentity(before, writer.stageIdentity))
      throwFileError(FileErrorKind::InvalidArgument, "Archive staging is inside a selected source directory.");
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), path.c_str());
    archive_entry_set_perm(entry.get(), before.st_mode & 0777);
    archive_entry_set_mtime(entry.get(), before.st_mtim.tv_sec, before.st_mtim.tv_nsec);
    if (S_ISDIR(before.st_mode)) {
      archive_entry_set_filetype(entry.get(), AE_IFDIR);
      archive_entry_set_size(entry.get(), 0);
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
      const auto folder = opened(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (!sameRevision(statFd(folder.get()), before)) {
        throwFileError(FileErrorKind::Changed, "An archive source changed.");
      }
      forEachChild(folder.get(), work, [&](const std::string& child) {
        auto childPath = path;
        childPath += '/';
        childPath += child;
        compressItem(folder.get(), child, childPath, writer, names);
      });
      if (!sameRevision(statFd(folder.get()), before)) {
        throwFileError(FileErrorKind::Changed, "An archive source folder changed.");
      }
    } else if (S_ISREG(before.st_mode)) {
      if (before.st_size < 0) {
        throwFileError(FileErrorKind::InvalidArgument, "An archive source has an invalid size.");
      }
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_size(entry.get(), before.st_size);
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
      const auto input = opened(::openat(parent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      if (!sameRevision(statFd(input.get()), before)) {
        throwFileError(FileErrorKind::Changed, "An archive source changed.");
      }
      auto& buffer = writer.buffer;
      auto remaining = static_cast<std::uint64_t>(before.st_size);
      while (remaining) {
        work.check();
        ssize_t count;
        do {
          count = ::read(input.get(), buffer.data(), std::min<std::uint64_t>(remaining, buffer.size()));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
          throwFileError(FileErrorKind::Changed, "An archive source could not be read completely.");
        }
        std::size_t done = 0;
        while (done < static_cast<std::size_t>(count)) {
          const auto written =
              archive_write_data(writer.value.get(), buffer.data() + done, static_cast<std::size_t>(count) - done);
          if (written <= 0) {
            archiveError(writer.value.get(), work);
          }
          done += static_cast<std::size_t>(written);
        }
        remaining -= static_cast<std::uint64_t>(count);
        work.report(static_cast<std::uint64_t>(count));
      }
      if (!sameRevision(statFd(input.get()), before)) {
        throwFileError(FileErrorKind::Changed, "An archive source changed while reading.");
      }
    } else if (S_ISLNK(before.st_mode)) {
      auto& target = writer.buffer;
      constexpr std::size_t maximum = 4097;
      const auto size = ::readlinkat(parent, name.c_str(), target.data(), maximum);
      if (size < 0 || size >= static_cast<ssize_t>(maximum)) {
        throwFileError(FileErrorKind::Other, "Could not read archive source link.");
      }
      const std::string link(target.data(), static_cast<std::size_t>(size));
      // Creation stores the link itself. Only reading/extraction resolves and validates its target.
      archive_entry_set_filetype(entry.get(), AE_IFLNK);
      archive_entry_set_size(entry.get(), 0);
      archive_entry_set_symlink(entry.get(), link.c_str());
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
    } else {
      throwFileError(FileErrorKind::Unsupported, "Special files cannot be compressed.");
    }
    if (!sameRevision(statAt(parent, name), before)) {
      throwFileError(FileErrorKind::Changed, "An archive source was replaced.");
    }
  }
} // namespace

namespace filemanager::archive_backend::detail {
  void writeArchive(
      std::span<const std::filesystem::path> sources, Stage& stage, Format format, Level level, Work& work,
      const EncryptionOptions& options
  ) {
    if (sources.empty() || (format == Format::Gzip && sources.size() != 1))
      throwFileError(FileErrorKind::InvalidArgument, "Single-file gzip requires exactly one regular file.");
    Writer writer(stage.directory.get(), format, level, work, options);
    std::set<std::string> names;
    for (const auto& path : sources) {
      const auto directory = openDirectory(path.parent_path());
      const auto name = path.filename().native();
      if (name.empty() || name == "." || name == ".." || name.contains('\0'))
        throwFileError(FileErrorKind::InvalidArgument, "Invalid compression source.");
      const auto info = statAt(directory.get(), name);
      if (format == Format::Gzip && !S_ISREG(info.st_mode))
        throwFileError(FileErrorKind::Unsupported, "Single-file gzip requires a regular file.");
      compressItem(directory.get(), name, name, writer, names);
    }
    work.check();
    requireArchive(archive_write_close(writer.value.get()), writer.value.get(), work);
    writer.finished = true;
    if (::fsync(writer.output.get()) != 0 || writer.output.close() != 0)
      systemError("Could not finish the staged archive");
    work.check();
  }
} // namespace filemanager::archive_backend::detail
