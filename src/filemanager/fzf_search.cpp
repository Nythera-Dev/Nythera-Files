#include "filemanager/fzf_search.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <memory>
#include <utility>

namespace {
  constexpr std::string_view kOperators = "^$!'|\\";
  bool escaped(std::string_view text, std::size_t pos) {
    bool result = false;
    while (pos > 0 && text[--pos] == '\\')
      result = !result;
    return result;
  }

  template <typename Visit> void visitTerms(std::string_view query, Visit visit) {
    for (std::size_t start = 0; start < query.size();) {
      if (query[start] == ' ') {
        ++start;
        continue;
      }
      std::size_t end = start + 1;
      while (end < query.size() && (query[end] != ' ' || escaped(query, end)))
        ++end;
      visit(start, query.substr(start, end - start));
      start = end;
    }
  }

  void
  preserveRawLiterals(std::string_view query, const std::map<char, std::string>& markers, std::vector<bool>& literal) {
    visitTerms(query, [&](std::size_t start, std::string_view term) {
      if (term == "|")
        return;
      std::size_t begin = 0;
      std::size_t end = term.size();
      if (term.front() == '!')
        ++begin;
      if (end > begin + 1 && term.back() == '$' && !escaped(term, end - 1))
        --end;
      if (begin < end && (term[begin] == '^' || term[begin] == '\'')) {
        if (term[begin] == '\'' && end > begin + 2 && term[end - 1] == '\'' && !escaped(term, end - 1))
          --end;
        ++begin;
      }
      for (std::size_t i = begin; i < end; ++i) {
        if (term[i] == '\\' && i + 1 < end && (kOperators.contains(term[i + 1]) || term[i + 1] == ' ')) {
          ++i;
        } else if (markers.contains(term[i])) {
          literal[start + i] = true;
        }
      }
    });
  }

  std::optional<filemanager::SearchMode> termMode(std::string_view term) {
    using enum filemanager::SearchMode;
    if (term == "|")
      return None;
    if (term.size() < 2)
      return std::nullopt;
    const bool suffix = term.back() == '$' && !escaped(term, term.size() - 1);
    switch (term.front()) {
    case '!':
      return suffix || term[1] == '^' || term[1] == '\'' ? None : Exclude;
    case '\'':
      return suffix || (term.size() > 2 && term.back() == '\'' && !escaped(term, term.size() - 1)) ? None : Contiguous;
    case '^':
      return suffix ? Exact : StartsWith;
    default:
      return suffix ? std::optional(EndsWith) : std::nullopt;
    }
  }

  struct FilterInput {
    std::string pattern;
    std::string records;
  };

  FilterInput
  frameInput(const filemanager::FzfSearch& options, std::string_view query, std::span<const std::string_view> names) {
    using enum filemanager::SearchMode;
    const bool raw = options.manual || options.mode == None;
    const bool unchanged = options.mode == None && !options.manual;
    // fzf only escapes spaces in its extended query grammar. Use unused
    // private-use characters for literal operators before giving fzf the query.
    std::map<char, std::string> literalOperators;
    std::vector<bool> literal(query.size(), false);
    gunichar nextMarker = 0xf0000;
    for (std::size_t i = 0; !unchanged && i < query.size(); ++i) {
      if (query[i] == '\\' && i + 1 < query.size() && (kOperators.contains(query[i + 1]) || query[i + 1] == ' ')) {
        ++i;
        literal[i] = true;
      } else {
        literal[i] = !raw;
      }
      if (!literal[i] || !kOperators.contains(query[i]) || literalOperators.contains(query[i]))
        continue;
      std::string marker;
      while (nextMarker <= 0x10FFFFU) {
        std::array<char, 6> encoded{};
        const int length = g_unichar_to_utf8(nextMarker++, encoded.data());
        if (length <= 0 || static_cast<std::size_t>(length) > encoded.size())
          break;
        marker.assign(encoded.data(), static_cast<std::size_t>(length));
        if (!query.contains(marker) && std::ranges::none_of(names, [&](auto name) { return name.contains(marker); }))
          break;
        marker.clear();
      }
      if (!marker.empty())
        literalOperators.emplace(query[i], std::move(marker));
    }

    if (raw && !literalOperators.empty())
      preserveRawLiterals(query, literalOperators, literal);

    FilterInput input;
    if (!raw) {
      switch (options.mode) {
      case Contiguous:
        input.pattern = "'";
        break;
      case StartsWith:
      case Exact:
        input.pattern = "^";
        break;
      case Exclude:
        input.pattern = "!";
        break;
      default:
        break;
      }
    }
    for (std::size_t i = 0; i < query.size(); ++i) {
      char ch = query[i];
      if (ch == '\\'
          && i + 1 < query.size()
          && literal[i + 1]
          && (kOperators.contains(query[i + 1]) || query[i + 1] == ' ')) {
        ch = query[++i];
      }
      const auto marker = literalOperators.find(ch);
      if (literal[i] && marker != literalOperators.end()) {
        input.pattern += marker->second;
      } else {
        if (literal[i] && ch == ' ')
          input.pattern += '\\';
        input.pattern += ch;
      }
    }
    if (!raw && (options.mode == EndsWith || options.mode == Exact))
      input.pattern += '$';

    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto name = names[i];
      input.records += std::to_string(i);
      input.records += '\t';
      if (literalOperators.empty()) {
        input.records += name;
      } else {
        for (const char ch : name) {
          const auto found = literalOperators.find(ch);
          if (found != literalOperators.end())
            input.records += found->second;
          else
            input.records += ch;
        }
      }
      input.records += '\0';
    }
    return input;
  }

  class ProcessResult {
  public:
    ~ProcessResult() {
      if (output)
        g_bytes_unref(output);
    }
    GBytes* output = nullptr;
    bool done = false;
    bool success = false;
  };
} // namespace

namespace filemanager {

  SearchMode searchModeFromConfig(std::string_view value) {
    constexpr std::array<std::string_view, 7> keys{"contiguous", "fuzzy", "starts_with", "ends_with",
                                                   "exclude",    "exact", "none"};
    const auto found = std::ranges::find(keys, value);
    return found == keys.end() ? SearchMode::Contiguous : static_cast<SearchMode>(found - keys.begin());
  }

  std::string_view searchModeToConfig(SearchMode mode) noexcept {
    constexpr std::array<std::string_view, 7> keys{"contiguous", "fuzzy", "starts_with", "ends_with",
                                                   "exclude",    "exact", "none"};
    const auto index = static_cast<std::size_t>(mode);
    return index < keys.size() ? keys[index] : "contiguous";
  }

  std::optional<SearchMode> manualSearchMode(std::string_view query) {
    std::optional<SearchMode> mode;
    std::size_t terms = 0;
    visitTerms(query, [&](std::size_t, std::string_view term) {
      ++terms;
      if (const auto candidate = termMode(term))
        mode = candidate;
    });
    return mode && terms > 1 ? SearchMode::None : mode;
  }

  std::string findFzf() {
    g_autofree char* path = g_find_program_in_path("fzf");
    return path ? path : "";
  }

  std::optional<std::vector<std::size_t>> filterWithFzf(
      const FzfSearch& options, std::string_view query, std::span<const std::string_view> names, GCancellable* cancel
  ) {
    if (options.executable.empty() || query.contains('\0') || g_cancellable_is_cancelled(cancel))
      return std::nullopt;
    auto input = frameInput(options, query, names);
    const std::string filter = "--filter=" + input.pattern;
    const char* argv[]{options.executable.c_str(), "--read0",   "--print0",     "--no-sort", "--ignore-case",
                       "--delimiter=^[0-9]+\\t",   "--nth=2..", filter.c_str(), nullptr};
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE
        )
    );
    for (const char* variable : {"FZF_DEFAULT_OPTS", "FZF_DEFAULT_OPTS_FILE", "FZF_DEFAULT_COMMAND"})
      g_subprocess_launcher_unsetenv(launcher, variable);
    g_autoptr(GSubprocess) child = g_subprocess_launcher_spawnv(launcher, argv, nullptr);
    if (!child)
      return std::nullopt;

    // Run the cancellable pipe pump on this worker's private GLib context. On
    // cancellation or timeout, kill and reap the direct child before returning.
    const auto killOnCancel = g_cancellable_connect(
        cancel, G_CALLBACK(+[](GCancellable*, gpointer data) { g_subprocess_force_exit(G_SUBPROCESS(data)); }), child,
        nullptr
    );
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(GCancellable) io = g_cancellable_new();
    const auto cancelIo = g_cancellable_connect(
        cancel, G_CALLBACK(+[](GCancellable*, gpointer data) { g_cancellable_cancel(G_CANCELLABLE(data)); }), io,
        nullptr
    );
    g_autoptr(GSource) deadline = g_timeout_source_new(2000);
    g_source_set_callback(
        deadline,
        [](gpointer data) -> gboolean {
          g_cancellable_cancel(G_CANCELLABLE(data));
          return G_SOURCE_REMOVE;
        },
        io, nullptr
    );
    g_source_attach(deadline, context);

    ProcessResult result;
    g_autoptr(GBytes) bytes = g_bytes_new_static(input.records.data(), input.records.size());
    g_main_context_push_thread_default(context);
    g_subprocess_communicate_async(
        child, bytes, io,
        [](GObject* source, GAsyncResult* async, gpointer data) {
          auto& state = *static_cast<ProcessResult*>(data);
          state.success = g_subprocess_communicate_finish(G_SUBPROCESS(source), async, &state.output, nullptr, nullptr);
          state.done = true;
        },
        &result
    );
    while (!result.done)
      g_main_context_iteration(context, true);
    g_main_context_pop_thread_default(context);
    g_source_destroy(deadline);
    g_cancellable_disconnect(cancel, killOnCancel);
    g_cancellable_disconnect(cancel, cancelIo);
    if (!result.success || g_cancellable_is_cancelled(cancel)) {
      g_subprocess_force_exit(child);
      g_subprocess_wait(child, nullptr, nullptr);
      return std::nullopt;
    }
    if (!g_subprocess_get_if_exited(child))
      return std::nullopt;
    const int status = g_subprocess_get_exit_status(child);
    if (status != 0 && status != 1)
      return std::nullopt;

    gsize size = 0;
    const auto* data = static_cast<const char*>(g_bytes_get_data(result.output, &size));
    std::string_view output(data ? data : "", size);
    std::vector<bool> matched(names.size());
    while (!output.empty()) {
      const auto end = output.find('\0');
      const auto field = output.substr(0, end).find('\t');
      if (end == output.npos || field == output.npos || field == 0 || status != 0)
        return std::nullopt;
      std::size_t index = 0;
      const auto [ptr, error] = std::from_chars(output.data(), output.data() + field, index);
      if (error != std::errc{} || ptr != output.data() + field || index >= names.size() || matched[index])
        return std::nullopt;
      matched[index] = true;
      output.remove_prefix(end + 1);
    }

    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < matched.size(); ++i) {
      if (matched[i])
        indices.push_back(i);
    }
    return indices;
  }

} // namespace filemanager
