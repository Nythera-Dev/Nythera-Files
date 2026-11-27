// Regression scenarios adapted from Luciole archive_service_test.cpp; see the project LICENSE.
#include "archive_rar_fixtures.h"
#include "archive_sevenzip_fixtures.h"
#include "filemanager/archive/archive_backend.h"
#include "filemanager/archive/archive_safety.h"
#include "test_isolation.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <thread>
#include <zlib.h>

void setFileOperationRenameNoReplaceHookForTesting(std::function<int(const char*, const char*)> hook);

namespace {
  enum class WriteFault { None, Short, NoSpace };
  WriteFault writeFault = WriteFault::None;
  std::optional<unsigned long> freeMiB;
  std::optional<std::uint64_t> inputBytes;
  std::size_t writes = 0;
} // namespace

#ifdef __GLIBC__
#define __real_pread __real_pread64
#define __wrap_pread __wrap_pread64
#define __real_pwrite __real_pwrite64
#define __wrap_pwrite __wrap_pwrite64
#define __real_fstatvfs __real_fstatvfs64
#define __wrap_fstatvfs __wrap_fstatvfs64
#endif

extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* bytes, size_t size, off_t offset) {
  const auto result = __real_pread(fd, bytes, size, offset);
  if (inputBytes && result > 0)
    *inputBytes += static_cast<std::uint64_t>(result);
  return result;
}

extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t size, off_t offset) {
  if (writeFault != WriteFault::None) {
    ++writes;
    if (writeFault == WriteFault::NoSpace && writes > 1) {
      errno = ENOSPC;
      return -1;
    }
    size = std::min<std::size_t>(size, 3);
  }
  return __real_pwrite(fd, bytes, size, offset);
}

extern "C" int __real_fstatvfs(int, struct statvfs*);
extern "C" int __wrap_fstatvfs(int fd, struct statvfs* state) {
  const int result = __real_fstatvfs(fd, state);
  if (result == 0 && freeMiB)
    state->f_bavail = (*freeMiB << 20) / state->f_frsize;
  return result;
}

namespace {
  namespace fs = std::filesystem;
  namespace backend = filemanager::archive_backend;
  namespace engine = filemanager::file_operation_engine;
  using backend::Format;
  using backend::Level;

  void write(const fs::path& path, std::string_view data) {
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    TEST_CHECK(file.good());
  }

  std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    TEST_CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  std::string base64(std::string_view input) {
    gsize size = 0;
    const std::unique_ptr<guchar, decltype(&g_free)> decoded(
        g_base64_decode(std::string(input).c_str(), &size), g_free
    );
    return {reinterpret_cast<const char*>(decoded.get()), size};
  }

  void noStage(const fs::path& directory) {
    for (const auto& entry : fs::directory_iterator(directory))
      TEST_CHECK(!entry.path().filename().native().starts_with(".nythera-archive-"));
  }

  void ok(const backend::Result& result) {
    if (!result.ok)
      throw std::runtime_error(result.message);
  }

  struct Member {
    std::string name, bytes;
    unsigned type = AE_IFREG;
    std::string symlink, hardlink;
    unsigned mode = 0644;
    std::optional<std::uint64_t> size;
  };

  template <typename SetOptions>
  void fixture(const fs::path& path, std::span<const Member> entries, SetOptions options) {
    using A = struct archive;
    const std::unique_ptr<A, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    TEST_CHECK(writer);
    options(writer.get());
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    for (const auto& item : entries) {
      const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), archive_entry_free
      );
      archive_entry_set_pathname(entry.get(), item.name.c_str());
      archive_entry_set_filetype(entry.get(), item.type);
      archive_entry_set_perm(entry.get(), item.mode);
      archive_entry_set_size(entry.get(), static_cast<la_int64_t>(item.size.value_or(item.bytes.size())));
      if (!item.symlink.empty())
        archive_entry_set_symlink(entry.get(), item.symlink.c_str());
      if (!item.hardlink.empty())
        archive_entry_set_hardlink(entry.get(), item.hardlink.c_str());
      TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      if (!item.bytes.empty())
        TEST_CHECK(archive_write_data(writer.get(), item.bytes.data(), item.bytes.size()) == std::ssize(item.bytes));
    }
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }

  void tar(const fs::path& path, std::initializer_list<Member> items) {
    fixture(path, {items.begin(), items.size()}, [](auto* writer) {
      TEST_CHECK(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    });
  }

  int renameNoReplace(const char* from, const char* to) {
    return static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE));
  }

  void testRoundTrips(const fs::path& root) {
    TEST_CHECK(backend::capabilities().size() == 9);
    TEST_CHECK(backend::formatFromName("example.TAR.GZ") == Format::TarGzip);
    TEST_CHECK(backend::formatFromName("example.TGZ") == Format::TarGzip);
    TEST_CHECK(backend::formatFromName("example.tar.xz") == Format::TarXz);
    TEST_CHECK(backend::formatFromName("example.LOG.GZ") == Format::Gzip);
    TEST_CHECK(!backend::formatFromName("sample.zip.001"));
    const auto source = root / "source";
    fs::create_directories(source / "nested/empty");
    write(source / "nested/café\nnotes.txt", "Archive payload");
    fs::create_symlink("nested/café\nnotes.txt", source / "link");
    for (const auto& item : backend::capabilities()) {
      TEST_CHECK(item.extract);
      TEST_CHECK(item.create == (item.format != Format::Rar));
      if (item.format == Format::Rar || item.format == Format::Gzip)
        continue;
      TEST_CHECK(item.levels[1] == (item.format != Format::Tar));
      const auto archive = root / ("roundtrip" + std::string(item.suffix));
      const auto result = backend::compress(std::array{source}, archive, item.format, Level::Balanced);
      ok(result);
      TEST_CHECK(result.mutated && result.completedEntries.size() == 1);
      const auto out = root / ("out" + std::string(item.suffix));
      fs::create_directory(out);
      ok(backend::extract(archive, out));
      TEST_CHECK(read(out / "source/nested/café\nnotes.txt") == "Archive payload");
      TEST_CHECK(fs::is_empty(out / "source/nested/empty"));
      TEST_CHECK(fs::read_symlink(out / "source/link") == "nested/café\nnotes.txt");
      noStage(out);
    }
    fs::copy_file(root / "roundtrip.tar.gz", root / "other.TGZ");
    const auto out = root / "tgz-result";
    fs::create_directory(out);
    ok(backend::extract(root / "other.TGZ", out));
    TEST_CHECK(!backend::compress(std::array{source}, root / "unwritable.rar", Format::Rar, Level::Balanced).ok);
    const auto recursive = backend::compress(std::array{source}, source / "self.zip", Format::Zip, Level::Balanced);
    TEST_CHECK(!recursive.ok && !recursive.mutated && recursive.message.contains("staging"));
    noStage(source);
  }

  void testGzip(const fs::path& root) {
    const auto out = root / "gzip-out";
    fs::create_directory(out);
    for (const auto name : {"empty", "example.LOG", "concat"}) {
      const auto source = root / name;
      const auto archive = root / (std::string(name) + ".gz");
      const std::string data = name == std::string_view("empty") ? "" : std::string(65536, 'g');
      write(source, data);
      ok(backend::compress(std::array{source}, archive, Format::Gzip, Level::Balanced));
      if (name == std::string_view("concat")) {
        const auto member = read(archive);
        write(archive, member + member);
      }
      ok(backend::extract(archive, out));
      TEST_CHECK(read(out / name) == (name == std::string_view("concat") ? data + data : data));
      for (const int field : {4, 8}) {
        auto corrupted = read(archive);
        corrupted[corrupted.size() - static_cast<std::size_t>(field)] ^= 0x01;
        write(root / "corrupt.gz", corrupted);
        const auto result = backend::extract(root / "corrupt.gz", out);
        TEST_CHECK(!result.ok && !result.mutated);
      }
      auto truncated = read(archive);
      truncated.pop_back();
      write(root / "truncated.gz", truncated);
      TEST_CHECK(!backend::extract(root / "truncated.gz", out).ok);
    }
    for (const int field : {4, 8}) {
      auto corrupted = read(root / "roundtrip.tar.gz");
      corrupted[corrupted.size() - static_cast<std::size_t>(field)] ^= 0x01;
      write(root / "corrupt.tar.gz", corrupted);
      TEST_CHECK(!backend::extract(root / "corrupt.tar.gz", out).ok);
    }
    write(root / "plain.gz", "not compressed");
    TEST_CHECK(!backend::extract(root / "plain.gz", out).ok);
    write(root / "wrong.gz", read(root / "roundtrip.tar.xz"));
    TEST_CHECK(!backend::extract(root / "wrong.gz", out).ok);
    write(root / "wrong.tar.gz", read(root / "roundtrip.tar"));
    TEST_CHECK(!backend::extract(root / "wrong.tar.gz", out).ok);
    for (const auto suffix : {".xz", ".bz2", ".zst"}) {
      auto corrupted = read(root / ("roundtrip.tar" + std::string(suffix)));
      corrupted.resize(corrupted.size() - 4);
      const auto archive = root / ("truncated.tar" + std::string(suffix));
      write(archive, corrupted);
      TEST_CHECK(!backend::extract(archive, out).ok);
    }
    noStage(out);
  }

  void testProviders(const fs::path& root) {
    for (const auto& reference : rar_fixtures::archives) {
      const auto path = root / (std::string(reference.name) + ".rar");
      write(path, base64(reference.base64));
      const auto out = root / reference.name;
      fs::create_directory(out);
      const auto result = backend::extract(path, out);
      if (!reference.error.empty()) {
        TEST_CHECK(!result.ok && !result.mutated);
      } else {
        ok(result);
        const auto data = read(out / reference.member);
        TEST_CHECK(data.size() == reference.size);
        const std::unique_ptr<char, decltype(&g_free)> hash(
            g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(data.data()), data.size()),
            g_free
        );
        TEST_CHECK(hash.get() == reference.sha256);
      }
      noStage(out);
    }
    unsigned rejectedIndex = 0;
    for (const auto encoded : rar_fixtures::rejected) {
      const auto name = "rar-hostile-" + std::to_string(rejectedIndex++);
      const auto path = root / (name + ".rar");
      write(path, base64(encoded));
      const auto out = root / name;
      fs::create_directory(out);
      const auto result = backend::extract(path, out);
      TEST_CHECK(!result.ok && !result.mutated && fs::is_empty(out));
    }
    for (const auto& reference : sevenzip_fixtures::archives) {
      const auto path = root / (std::string(reference.name) + ".7z");
      const auto out = root / reference.name;
      write(path, base64(reference.base64));
      fs::create_directory(out);
      const auto result = backend::extract(path, out);
      if (reference.encrypted)
        TEST_CHECK(!result.ok && !result.mutated && fs::is_empty(out));
      else
        ok(result);
      noStage(out);
    }
    const auto zip = root / "encrypted.zip";
    const std::array secret{Member{.name = "secret", .bytes = "no password support"}};
    fixture(zip, secret, [](auto* a) {
      TEST_CHECK(archive_write_set_format_zip(a) == ARCHIVE_OK);
      if (archive_write_set_options(a, "zip:encryption=zipcrypt") == ARCHIVE_OK)
        TEST_CHECK(archive_write_set_passphrase(a, "public test password") == ARCHIVE_OK);
      else
        throw std::runtime_error("Host cannot generate the encrypted ZIP test fixture.");
    });
    const auto out = root / "encrypted-zip-out";
    fs::create_directory(out);
    TEST_CHECK(!backend::extract(zip, out).ok && fs::is_empty(out));
  }

  void testHostile(const fs::path& root) {
    const auto archive = root / "hostile.tar";
    const auto out = root / "hostile";
    fs::create_directory(out);
    write(root / "outside", "canary");
    const std::initializer_list<std::initializer_list<Member>> cases{
        {{.name = "../outside", .bytes = "escape"}},
        {{.name = "/absolute", .bytes = "escape"}},
        {{.name = "C:/absolute", .bytes = "escape"}},
        {{.name = "..\\outside", .bytes = "escape"}},
        {{.name = "a", .type = AE_IFLNK, .symlink = "../outside"}},
        {{.name = "a", .type = AE_IFLNK, .symlink = "/outside"}},
        {{.name = "a", .hardlink = "../outside"}},
        {{.name = "a", .type = AE_IFLNK, .symlink = "b"}, {.name = "b", .type = AE_IFLNK, .symlink = "a"}},
        {{.name = "dir", .type = AE_IFLNK, .symlink = "a"}, {.name = "dir/file", .bytes = "blocked"}},
        {{.name = "a", .bytes = "first"}, {.name = "a", .bytes = "second"}},
        {{.name = "pipe", .type = AE_IFIFO}},
    };
    for (const auto entries : cases) {
      tar(archive, entries);
      const auto result = backend::extract(archive, out);
      TEST_CHECK(!result.ok && !result.mutated && fs::is_empty(out));
      TEST_CHECK(read(root / "outside") == "canary");
    }
    tar(archive, {{.name = "mode", .bytes = "permissions", .mode = 07777}});
    ok(backend::extract(archive, out));
    struct stat st{};
    TEST_CHECK(::stat((out / "mode").c_str(), &st) == 0 && (st.st_mode & 07000) == 0);
  }

  void testLinkConflicts(const fs::path& root) {
    const auto archive = root / "links.tar";
    tar(archive,
        {
            {.name = "original", .bytes = "from archive"},
            {.name = "hard", .hardlink = "original"},
            {.name = "symbolic", .type = AE_IFLNK, .symlink = "original"},
            {.name = "chain", .type = AE_IFLNK, .symlink = "symbolic"},
        });
    for (const auto action :
         {engine::ConflictAction::Replace, engine::ConflictAction::Rename, engine::ConflictAction::Skip}) {
      const auto out = root / ("links-" + std::to_string(static_cast<int>(action)));
      fs::create_directory(out);
      write(out / "original", "existing");
      unsigned conflicts = 0;
      backend::Callbacks callbacks{
          .resolveConflict = [&](const auto&, const auto&, bool, const auto&, auto*) {
            ++conflicts;
            return engine::ConflictDecision{.action = action, .alternativeName = "renamed"};
          },
      };
      const auto result = backend::extract(archive, out, nullptr, callbacks);
      ok(result);
      TEST_CHECK(conflicts == 1);
      const auto publishedName = action == engine::ConflictAction::Rename ? "renamed" : "original";
      if (action == engine::ConflictAction::Skip) {
        TEST_CHECK(result.skippedEntries.size() == 1 && read(out / "original") == "existing");
      } else {
        TEST_CHECK(read(out / publishedName) == "from archive" && fs::equivalent(out / publishedName, out / "hard"));
      }
      TEST_CHECK(read(out / "hard") == "from archive");
      TEST_CHECK(fs::read_symlink(out / "symbolic") == "original" && fs::read_symlink(out / "chain") == "symbolic");
      noStage(out);
    }
  }

  void testPublication(const fs::path& root) {
    const auto archive = root / "publication.tar";
    const auto out = root / "publication";
    const auto outside = root / "publication-outside";
    fs::create_directory(out);
    fs::create_directory(outside);

    tar(archive, {{.name = "folder/member", .bytes = "archive bytes"}});
    fs::create_symlink(outside, out / "folder");
    const backend::Callbacks merge{
        .resolveConflict = [](const auto&, const auto&, bool, const auto&, auto*) {
          return engine::ConflictDecision{.action = engine::ConflictAction::Merge};
        },
    };
    TEST_CHECK(!backend::extract(archive, out, nullptr, merge).ok && fs::is_empty(outside));
    fs::remove(out / "folder");
    fs::create_directory(out / "folder");
    ok(backend::extract(archive, out, nullptr, merge));
    TEST_CHECK(read(out / "folder/member") == "archive bytes");
    fs::remove(out / "folder/member");
    fs::remove(out / "folder");

    {
      const auto reset = nythera::test::makeScopeExit([] { setFileOperationRenameNoReplaceHookForTesting({}); });
      setFileOperationRenameNoReplaceHookForTesting([](const char* from, const char* to) {
        TEST_CHECK(::mkdir(to, 0700) == 0);
        return renameNoReplace(from, to);
      });
      TEST_CHECK(!backend::extract(archive, out).ok && fs::is_empty(out / "folder"));
    }
    fs::remove(out / "folder");
    {
      const auto reset = nythera::test::makeScopeExit([] { setFileOperationRenameNoReplaceHookForTesting({}); });
      setFileOperationRenameNoReplaceHookForTesting([](const char*, const char*) {
        errno = EINVAL;
        return -1;
      });
      ok(backend::extract(archive, out));
      TEST_CHECK(read(out / "folder/member") == "archive bytes");
    }
    noStage(out);

    tar(archive, {{.name = "a", .bytes = "first"}, {.name = "b", .bytes = "second"}});
    {
      const auto reset = nythera::test::makeScopeExit([] { setFileOperationRenameNoReplaceHookForTesting({}); });
      setFileOperationRenameNoReplaceHookForTesting([](const char* from, const char* to) {
        if (fs::path(to).filename() == "b") {
          errno = EPERM;
          return -1;
        }
        return renameNoReplace(from, to);
      });
      const auto result = backend::extract(archive, out);
      TEST_CHECK(!result.ok && result.mutated && !result.cancelled && result.completedEntries.size() == 1);
      TEST_CHECK(read(out / "a") == "first" && !fs::exists(out / "b"));
    }
    fs::remove(out / "a");
    const std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancellation(g_cancellable_new(), g_object_unref);
    const backend::Callbacks callbacks{
        .checkpoint = [&] {
          if (fs::exists(out / "a"))
            g_cancellable_cancel(cancellation.get());
        },
    };
    const auto result = backend::extract(archive, out, cancellation.get(), callbacks);
    TEST_CHECK(!result.ok && result.cancelled && result.mutated && result.completedEntries.size() == 1);
    TEST_CHECK(read(out / "a") == "first" && !fs::exists(out / "b"));
    fs::remove(out / "a");

    noStage(out);
  }

  // Reduced-size counterpart of Luciole's sparse member regression.
  void testSparseMember(const fs::path& root) {
    constexpr std::size_t size = 2ULL << 20;
    const auto path = root / "sparse.tar";
    const auto out = root / "sparse-out";
    fs::create_directory(out);
    using A = struct archive;
    const std::unique_ptr<A, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), "sparse");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), size);
    archive_entry_sparse_add_entry(entry.get(), 0, 4);
    archive_entry_sparse_add_entry(entry.get(), size - 4, 4);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    std::string data(size, '\0');
    data.replace(0, 4, "HEAD");
    data.replace(size - 4, 4, "TAIL");
    TEST_CHECK(archive_write_data(writer.get(), data.data(), data.size()) == std::ssize(data));
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK && fs::file_size(path) < 65536);
    {
      const auto reset = nythera::test::makeScopeExit([] { freeMiB.reset(); });
      freeMiB = 0;
      const auto denied = backend::extract(path, out);
      TEST_CHECK(!denied.ok && denied.message.contains("Not enough space") && fs::is_empty(out));
    }
    ok(backend::extract(path, out));
    TEST_CHECK(read(out / "sparse") == data);
    struct stat state{};
    TEST_CHECK(::stat((out / "sparse").c_str(), &state) == 0 && state.st_blocks * 512 < 65536);
    noStage(out);
  }

  void testWritesAndCancellation(const fs::path& root) {
    const auto archive = root / "limits.tar";
    const auto out = root / "limits";
    fs::create_directory(out);
    tar(archive, {{.name = "large", .bytes = "some bytes for short writes", .mode = 0600}});
    {
      const auto reset = nythera::test::makeScopeExit([] { writeFault = WriteFault::None; });
      writeFault = WriteFault::NoSpace;
      writes = 0;
      const auto diskFull = backend::extract(archive, out);
      TEST_CHECK(!diskFull.ok && writes == 2 && fs::is_empty(out));
      writeFault = WriteFault::Short;
      writes = 0;
      ok(backend::extract(archive, out));
      TEST_CHECK(writes > 2 && read(out / "large") == "some bytes for short writes");
    }
    fs::remove(out / "large");
    const auto old = root / "7z-cancel.7z";
    write(old, "existing archive");
    const auto source = root / "7z-cancel-source";
    write(source, std::string(2ULL << 20, 'x'));
    const std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancellation(g_cancellable_new(), g_object_unref);
    const auto cancelled = backend::compress(
        std::array{source}, old, Format::SevenZip, Level::Balanced, cancellation.get(),
        {.progress = [&](auto, auto) { g_cancellable_cancel(cancellation.get()); }}
    );
    TEST_CHECK(!cancelled.ok && cancelled.cancelled && !cancelled.mutated && read(old) == "existing archive");
    noStage(root);
  }

  void testSinglePass(const fs::path& root) {
    for (const auto suffix : {".tar.gz", ".tar.xz", ".tar.zst"}) {
      const auto archive = root / ("roundtrip" + std::string(suffix));
      const auto out = root / ("one-pass" + std::string(suffix));
      fs::create_directory(out);
      inputBytes = 0;
      const auto reset = nythera::test::makeScopeExit([] { inputBytes.reset(); });
      ok(backend::extract(archive, out));
      const auto encodedSize = fs::file_size(archive);
      // The initial format sniff can read up to ten of these bytes twice.
      TEST_CHECK(*inputBytes >= encodedSize && *inputBytes <= encodedSize + 10);
    }
  }

  void testSourceRevision(const fs::path& root) {
    const auto archive = root / "revision.tar";
    tar(archive, {{.name = "member", .bytes = "archive payload"}});
    for (const bool changeMtime : {false, true}) {
      const auto out = root / (changeMtime ? "changed-mtime" : "changed-ctime");
      fs::create_directory(out);
      struct stat before{};
      TEST_CHECK(::stat(archive.c_str(), &before) == 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      bool changed = false;
      const auto result = backend::extract(
          archive, out, nullptr, {.progress = [&](auto, auto) {
            if (changed)
              return;
            if (changeMtime) {
              const std::array times{before.st_atim, timespec{before.st_mtim.tv_sec + 1, before.st_mtim.tv_nsec}};
              TEST_CHECK(::utimensat(AT_FDCWD, archive.c_str(), times.data(), 0) == 0);
            } else {
              TEST_CHECK(::chmod(archive.c_str(), before.st_mode & 07777) == 0);
            }
            changed = true;
          }}
      );
      TEST_CHECK(changed);
      if (changeMtime) {
        TEST_CHECK(!result.ok && result.message.contains("input changed") && fs::is_empty(out));
      } else {
        struct stat after{};
        TEST_CHECK(::stat(archive.c_str(), &after) == 0);
        TEST_CHECK(before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec);
        ok(result);
        TEST_CHECK(read(out / "member") == "archive payload");
      }
    }
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-native-archives", .setXdgDirs = true});
    const auto root = env.root() / "work";
    const auto spool = root / "spool";
    fs::create_directory(spool);
    const nythera::test::ScopedEnvVar temp("TMPDIR", spool.native());
    const backend::detail::ArchiveLocale locale;
    testRoundTrips(root);
    testGzip(root);
    testProviders(root);
    testHostile(root);
    testLinkConflicts(root);
    testPublication(root);
    testSparseMember(root);
    testWritesAndCancellation(root);
    testSinglePass(root);
    testSourceRevision(root);
    TEST_CHECK(fs::is_empty(spool));
  });
}
