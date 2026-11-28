// Password transport and private extraction adapted from Luciole archive_sevenzip.cpp
// and archive_provider_process.cpp (26649c8264145ad527b3a13cf067f08cd4663f17), MIT.
#include "archive_safety.h"

#include <algorithm>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace filemanager::archive_backend {
  std::optional<std::string> sevenZipProgram() {
    for (const char* name : {"7zz", "7z"}) {
      g_autofree gchar* path = g_find_program_in_path(name);
      if (path && std::filesystem::path(path).is_absolute())
        return path;
    }
    return std::nullopt;
  }
} // namespace filemanager::archive_backend

namespace filemanager::archive_backend::detail {
  namespace {
    namespace fs = std::filesystem;

    struct ProviderResult {
      int status = -1;
      std::string output;
      bool rejected() const {
        return status == 2 && (output.contains("Wrong password") || output.contains("wrong password"));
      }
    };

    void suspendProvider(GSubprocess* child, bool pause, GCancellable* cancellable) {
      const char* id = g_subprocess_get_identifier(child);
      if (!id)
        return;
      const auto status = std::filesystem::path("/proc") / id / "status";
      g_subprocess_send_signal(child, pause ? SIGSTOP : SIGCONT);
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (!g_cancellable_is_cancelled(cancellable)) {
        std::ifstream state(status);
        if (!state)
          return; // The helper exited before reaching the pause boundary.
        for (std::string line; std::getline(state, line);) {
          if (!line.starts_with("State:"))
            continue;
          const auto code = line.find_first_not_of(" \t", 6);
          if (code != std::string::npos) {
            const char value = line[code];
            if (value == 'Z' || value == 'X' || (value == 'T') == pause)
              return;
          }
          break;
        }
        if (std::chrono::steady_clock::now() >= until)
          throwFileError(FileErrorKind::Other, "Could not change the state of the 7-Zip process.");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    ProviderResult runProvider(
        const Stage& stage, const Source* source, std::string_view password, const std::vector<std::string>& args,
        bool writing, Work& work
    ) {
      const auto program = sevenZipProgram();
      if (!program)
        throwFileError(FileErrorKind::Unsupported, "Encrypted 7z archives require the optional 7-Zip executable.");
      if ((writing && password.empty())
          || password.find_first_of("\r\n") != std::string_view::npos
          || password.contains('\0'))
        throwFileError(FileErrorKind::InvalidArgument, "Enter a nonempty 7z password without line breaks.");
      work.check();
      std::vector<const char*> argv{program->c_str()};
      for (const auto& arg : args)
        argv.push_back(arg.c_str());
      argv.push_back(nullptr);
      g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
          static_cast<GSubprocessFlags>(
              G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE
          )
      );
      g_subprocess_launcher_set_cwd(launcher, stage.path.c_str());
      g_subprocess_launcher_setenv(launcher, "LC_ALL", "C.UTF-8", true);
      g_subprocess_launcher_setenv(launcher, "TMPDIR", stage.path.c_str(), true);
      if (source) {
        const int input = ::fcntl(source->file.get(), F_DUPFD_CLOEXEC, 4);
        if (input < 0)
          systemError("Could not pass the held 7z input");
        g_subprocess_launcher_take_fd(launcher, input, 3);
      }
      g_autoptr(GSubprocess) child = g_subprocess_launcher_spawnv(launcher, argv.data(), nullptr);
      if (!child)
        throwFileError(FileErrorKind::Unsupported, "Could not start the optional 7-Zip executable.");

      struct Communication {
        bool done = false;
        GBytes* output = nullptr;
        bool ok = false;
        ~Communication() {
          if (output)
            g_bytes_unref(output);
        }
      } state;
      g_autoptr(GMainContext) context = g_main_context_new();
      std::string secret(password);
      secret += '\n';
      if (writing) {
        secret += password;
        secret += '\n';
      }
      g_autoptr(GBytes) input = g_bytes_new_static(secret.data(), secret.size());
      g_main_context_push_thread_default(context);
      g_subprocess_communicate_async(
          child, input, nullptr,
          [](GObject* object, GAsyncResult* result, gpointer data) {
            auto& communication = *static_cast<Communication*>(data);
            communication.ok =
                g_subprocess_communicate_finish(G_SUBPROCESS(object), result, &communication.output, nullptr, nullptr);
            communication.done = true;
          },
          &state
      );
      g_main_context_pop_thread_default(context);
      std::exception_ptr failure;
      while (!state.done) {
        while (g_main_context_iteration(context, false)) {
        }
        if (state.done)
          break;
        try {
          if (!failure && work.callbacks.helperCheckpoint) {
            work.callbacks.helperCheckpoint([&](bool pause) { suspendProvider(child, pause, work.cancellable); });
          }
          if (!failure && g_cancellable_is_cancelled(work.cancellable))
            throwFileError(FileErrorKind::Cancelled, "Archive operation cancelled.");
        } catch (...) {
          failure = std::current_exception();
          g_subprocess_force_exit(child);
        }
        if (!state.done)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      explicit_bzero(secret.data(), secret.size());
      if (failure)
        std::rethrow_exception(failure);
      work.check();
      if (!state.ok) {
        g_subprocess_force_exit(child);
        g_subprocess_wait(child, nullptr, nullptr);
      }
      if (!state.ok || !g_subprocess_get_if_exited(child))
        throwFileError(FileErrorKind::Other, "7-Zip stopped before completing.");
      if (source
          && (!sameRevision(statFd(source->file.get()), source->revision)
              || !sameRevision(statAt(source->parent.get(), source->name), source->revision)))
        throwFileError(FileErrorKind::Changed, "Archive input changed while reading.");
      gsize length = 0;
      const auto* data = static_cast<const char*>(g_bytes_get_data(state.output, &length));
      return {g_subprocess_get_exit_status(child), length ? std::string(data, length) : std::string{}};
    }

    struct Member {
      bool directory = false;
      std::uint64_t bytes = 0;
    };

    using Manifest = std::map<std::string, Member, std::less<>>;

    [[noreturn]] void invalidListing() {
      throwFileError(FileErrorKind::InvalidArgument, "7-Zip returned an invalid or ambiguous member listing.");
    }

    Manifest catalogue(const std::string& listing, Work& work) {
      Manifest entries;
      std::map<std::string, std::string, std::less<>> fields;
      const auto finish = [&] {
        if (fields.empty())
          return;
        const auto field = [&](std::string_view key) -> std::string_view {
          const auto it = fields.find(key);
          return it == fields.end() ? std::string_view{} : it->second;
        };
        if (!field("Symbolic Link").empty() || !field("Hard Link").empty())
          throwFileError(FileErrorKind::Unsupported, "Links in encrypted 7z archives are unsupported.");
        const auto attributes = field("Attributes");
        const auto space = attributes.find(' ');
        const bool dir = attributes.substr(0, space).contains('D') || field("Folder") == "+";
        if (space != std::string_view::npos) {
          const auto unixMode = attributes.substr(space + 1);
          if (unixMode.size() != 10 || unixMode[0] != (dir ? 'd' : '-'))
            throwFileError(FileErrorKind::Unsupported, "Unsupported entry type in encrypted 7z archive.");
        }
        const std::string raw(field("Path"));
        const auto path = safePath(raw);
        if (path.empty() || path != raw)
          invalidListing();
        std::uint64_t bytes = 0;
        const auto size = field("Size");
        if (!size.empty()) {
          const auto [end, error] = std::from_chars(size.data(), size.data() + size.size(), bytes);
          if (error != std::errc{} || end != size.data() + size.size())
            invalidListing();
        } else if (!dir) {
          invalidListing();
        }
        if (!entries.emplace(path, Member{dir, bytes}).second)
          invalidListing();
        fields.clear();
      };
      std::istringstream stream(listing);
      bool started = false;
      for (std::string line; std::getline(stream, line);) {
        work.check();
        if (!started && !(started = line.starts_with("Path = ")))
          continue;
        if (line.empty()) {
          finish();
          continue;
        }
        const auto sep = line.find(" = ");
        if (sep == std::string::npos
            || (fields.empty() && !line.starts_with("Path = "))
            || !fields.emplace(line.substr(0, sep), line.substr(sep + 3)).second)
          invalidListing();
      }
      finish();
      return entries;
    }

    void checkOutput(const Stage& stage, Manifest expected, Work& work) {
      // Include implicit parent directories in the expected tree.
      for (const auto& [name, member] : expected) {
        for (auto parent = parentName(name); !parent.empty(); parent = parentName(parent)) {
          if (!expected.try_emplace(parent, Member{true, 0}).first->second.directory)
            invalidListing();
        }
      }
      for (const auto& item : fs::recursive_directory_iterator(stage.path)) {
        work.check();
        const auto path = item.path().lexically_relative(stage.path).native();
        const auto match = expected.find(path);
        const auto type = item.symlink_status().type();
        if (match == expected.end()
            || (match->second.directory ? type != fs::file_type::directory
                                        : type != fs::file_type::regular || item.file_size() != match->second.bytes))
          invalidListing();
        struct stat info{};
        if (::lstat(item.path().c_str(), &info) != 0 || (type == fs::file_type::regular && info.st_nlink != 1))
          invalidListing();
        if ((info.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) != 0
            && ::chmod(item.path().c_str(), info.st_mode & 0777) != 0)
          systemError("Could not set staged archive entry permissions");
        work.item();
        work.report(match->second.bytes);
        expected.erase(match);
      }
      if (!expected.empty())
        invalidListing();
    }

    const char* helperMethod(Method method) {
      switch (method) {
      case Method::Default:
      case Method::Lzma2:
        return "LZMA2";
      case Method::Lzma:
        return "LZMA";
      case Method::Deflate:
        return "Deflate";
      case Method::Copy:
        return "Copy";
      default:
        throwFileError(FileErrorKind::Unsupported, "Unsupported encrypted 7z compression method.");
      }
    }
  } // namespace

  Entries inspectEncryptedSevenZip(const Source& source, Stage& stage, Work& work) {
    bool requested = false;
    std::string password;
    for (;;) {
      const auto listing = runProvider(
          stage, &source, password, {"l", "-t7z", "-slt", "-ba", "-bsp0", "-bse1", "/proc/self/fd/3"}, false, work
      );
      explicit_bzero(password.data(), password.size());
      if (listing.rejected()) {
        if (!work.callbacks.password)
          throwFileError(FileErrorKind::Unsupported, "Encrypted 7z browsing requires a password.");
        auto answer = work.callbacks.password(requested);
        if (!answer || answer->empty())
          throwFileError(FileErrorKind::Cancelled, "Archive password entry cancelled.");
        password = std::move(*answer);
        requested = true;
        continue;
      }
      if (listing.status != 0)
        throwFileError(FileErrorKind::InvalidArgument, "7-Zip could not list this encrypted archive.");
      Entries entries;
      for (const auto& [name, member] : catalogue(listing.output, work)) {
        if (!g_utf8_validate(name.data(), name.size(), nullptr)
            || std::ranges::any_of(name, [](char c) { return g_ascii_iscntrl(c); }))
          invalidListing();
        for (auto parent = parentName(name); !parent.empty(); parent = parentName(parent))
          if (entries.try_emplace(parent, Entry{.kind = EntryKind::Directory, .mode = 0755}).first->second.kind
              != EntryKind::Directory)
            invalidListing();
        auto entry = member.directory ? Entry{.kind = EntryKind::Directory, .mode = 0755} : Entry{.size = member.bytes};
        const auto [existing, inserted] = entries.try_emplace(name, entry);
        if (!inserted && (existing->second.kind != EntryKind::Directory || !member.directory))
          invalidListing();
      }
      return entries;
    }
  }

  void readSelectedSevenZip(
      const Source& source, const Catalogue& catalog, const Projection& items, Stage& stage, Work& work
  ) {
    if (!work.callbacks.password)
      throwFileError(FileErrorKind::Unsupported, "Encrypted 7z extraction requires a password.");
    Stage request(stage.path);
    const auto selector = request.path / "members";
    std::ofstream names(selector, std::ios::binary);
    Manifest expected;
    auto remaining = availableSpace(stage.directory.get());
    for (const auto& [name, output] : items) {
      work.check();
      const auto& entry = catalog.entries.at(name);
      const auto size = entry.size.value_or(0);
      if (size > remaining)
        throwFileError(FileErrorKind::NoSpace, "There is insufficient space for the encrypted archive selection.");
      remaining -= size;
      names << name << '\n';
      expected.emplace(name, Member{entry.kind == EntryKind::Directory, size});
    }
    names.close();
    if (!names)
      throwFileError(FileErrorKind::Other, "Could not write the 7-Zip member list.");
    for (bool retry = false;; retry = true) {
      auto answer = work.callbacks.password(retry);
      if (!answer || answer->empty())
        throwFileError(FileErrorKind::Cancelled, "Archive password entry cancelled.");
      Stage attempt(request.path);
      const auto extracted = runProvider(
          request, &source, *answer,
          {"x", "-y", "-t7z", "-bso0", "-bsp0", "-bse1", "-spd", "-ssc", "-scsUTF-8", "-i@" + selector.native(),
           "-o" + attempt.path.native(), "/proc/self/fd/3"},
          false, work
      );
      explicit_bzero(answer->data(), answer->size());
      if (extracted.rejected())
        continue;
      if (extracted.status != 0)
        throwFileError(FileErrorKind::InvalidArgument, "7-Zip could not extract the selected members.");
      checkOutput(attempt, expected, work);
      for (const auto& [name, output] : items) {
        work.check();
        if (output.find('/') == std::string::npos)
          fs::rename(attempt.path / name, stage.path / output);
      }
      return;
    }
  }

  void readEncryptedSevenZip(const Source& source, Stage& stage, Work& work) {
    if (!sevenZipProgram())
      throwFileError(FileErrorKind::Unsupported, "Encrypted 7z archives require the optional 7-Zip executable.");
    if (!work.callbacks.password)
      throwFileError(FileErrorKind::Unsupported, "Encrypted 7z archives require a password.");
    bool retry = false;
    while (true) {
      work.check();
      auto answer = work.callbacks.password(retry);
      if (!answer)
        throwFileError(FileErrorKind::Cancelled, "Archive password entry cancelled.");
      const auto listing = runProvider(
          stage, &source, *answer, {"l", "-t7z", "-slt", "-ba", "-bsp0", "-bse1", "/proc/self/fd/3"}, false, work
      );
      if (listing.rejected()) {
        retry = true;
        continue;
      }
      if (listing.status != 0)
        throwFileError(FileErrorKind::InvalidArgument, "7-Zip could not list this encrypted archive.");
      auto expected = catalogue(listing.output, work);
      auto remaining = availableSpace(stage.directory.get());
      for (const auto& [name, member] : expected) {
        if (member.bytes > remaining)
          throwFileError(FileErrorKind::NoSpace, "There is insufficient space for the encrypted archive.");
        remaining -= member.bytes;
      }
      Stage attempt(stage.path.parent_path());
      const auto extracted = runProvider(
          stage, &source, *answer,
          {"x", "-y", "-t7z", "-bso0", "-bsp0", "-bse1", "-o" + attempt.path.native(), "/proc/self/fd/3"}, false, work
      );
      if (extracted.rejected()) {
        retry = true;
        continue;
      }
      if (extracted.status != 0)
        throwFileError(FileErrorKind::InvalidArgument, "7-Zip could not extract this encrypted archive.");
      checkOutput(attempt, std::move(expected), work);
      for (const auto& item : fs::directory_iterator(attempt.path)) {
        work.check();
        fs::rename(item.path(), stage.path / item.path().filename());
      }
      return;
    }
  }

  void writeEncryptedSevenZip(
      std::span<const fs::path> sources, Stage& stage, Level level, const EncryptionOptions& options, Work& work
  ) {
    const auto output = stage.path / "output.7z";
    std::vector<std::string> args{"a",    "-t7z",  "-p",    "-mhe=on",
                                  "-snl", "-bso0", "-bsp0", std::string("-m0=") + helperMethod(options.method)};
    if (options.method != Method::Copy)
      args.push_back(level == Level::Fast ? "-mx=1" : level == Level::Maximum ? "-mx=9" : "-mx=6");
    std::map<std::string, bool, std::less<>> names;
    std::vector<std::pair<fs::path, struct stat>> revisions;
    const auto snapshot = [&](const fs::path& path) {
      const auto name = path.filename().native();
      if (!g_utf8_validate(name.data(), static_cast<gssize>(name.size()), nullptr)
          || std::ranges::any_of(name, [](char c) { return g_ascii_iscntrl(c); }))
        throwFileError(FileErrorKind::Unsupported, "7-Zip cannot preserve a source name unambiguously.");
      struct stat revision{};
      if (::lstat(path.c_str(), &revision) != 0)
        systemError("Could not inspect an encrypted 7z source");
      if (!S_ISDIR(revision.st_mode) && !S_ISREG(revision.st_mode))
        throwFileError(FileErrorKind::Unsupported, "Links and special files are unsupported in encrypted 7z.");
      revisions.emplace_back(path, revision);
      work.item();
      return S_ISDIR(revision.st_mode);
    };
    for (const auto& source : sources) {
      work.check();
      const auto root = source.lexically_normal();
      const auto name = safePath(root.filename().native());
      if (name.empty() || !names.emplace(name, true).second)
        throwFileError(FileErrorKind::InvalidArgument, "Archive sources have conflicting names.");
      if (snapshot(root)) {
        const auto relative = fs::canonical(stage.path).lexically_relative(fs::canonical(root));
        const bool containsStage = !relative.empty() && relative != "." && *relative.begin() != "..";
        if (containsStage)
          args.push_back("-x!" + (fs::path(name) / relative).native());
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
          work.check();
          if (containsStage && it->path().lexically_relative(root) == relative) {
            it.disable_recursion_pending();
            continue;
          }
          snapshot(it->path());
        }
      }
    }
    args.push_back(output.native());
    args.emplace_back("--");
    for (const auto& source : sources)
      args.push_back(source.lexically_normal().native());
    const auto result = runProvider(stage, nullptr, options.password, args, true, work);
    if (result.status != 0)
      throwFileError(FileErrorKind::Other, "7-Zip could not create the encrypted archive.");
    for (const auto& [path, before] : revisions) {
      work.check();
      struct stat after{};
      if (::lstat(path.c_str(), &after) != 0 || !sameRevision(before, after))
        throwFileError(FileErrorKind::Changed, "An encrypted 7z source changed.");
    }
    auto archive = opened(::open(output.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (::fchmod(archive.get(), 0600) != 0 || ::fsync(archive.get()) != 0 || archive.close() != 0)
      systemError("Could not finish the staged encrypted archive");
    fs::rename(output, stage.path / "output");
  }
} // namespace filemanager::archive_backend::detail
