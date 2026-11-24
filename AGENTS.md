# Nythera Files Agent Instructions

Repository-specific guidance supplementing the global Codex `AGENTS.md`. Follow global instructions for general research, initiative, safety, efficient tool use and reporting; do not treat this file as replacing them.

## Project context and invariants

- **Nythera Files** is a standalone C++23 native Wayland file manager (`nythera-file-manager`) with Polkit helpers (`nythera-properties-helper`, `nythera-admin-helper`) and its own asset bundle (`files-assets/`), built with **Meson/Ninja** and orchestrated by `just`.
- Development primarily targets **Umbriel**. Preserve dark sci-fi/fantasy aesthetics and configurable styling rendered through **NytheraLib**.
- Maintain strict independence from **Nythera Shell**: do not introduce build-time or mandatory runtime dependencies on `nythera-shell` sources, IPC, or shell-owned assets (`FileManager` reads shared Shell config read-only and falls back to built-in palettes).
- Preserve Polkit action IDs (`data/polkit/dev.nythera.Files.*.policy.in`), helper IPC contracts, and root-ownership security checks. Do not run privileged installs (`sudo just install`, `makepkg -si`) or mix `/usr/local/libexec/` and `/usr/lib/` helper prefixes without explicit authorisation.
- Follow relevant project documentation listed below. Surface conflicts with documented technical invariants rather than silently bypassing them.

## Cross-repository dependencies (NytheraLib)

- `nythera-files` dynamically links `nythera-lib` (`>=0.1.0, <0.2.0`) via `pkg-config`.
- `justfile` automatically discovers `$HOME/.local/opt/nythera-lib/lib/pkgconfig` when `nythera-lib` is not already found by system `pkg-config` or an explicit `PKG_CONFIG_PATH`, without permanently modifying the shell environment.
- Follow the coordinated `0.1.x` rebuild discipline in `../nythera-lib/README.md`:
  - Build and install `nythera-lib` before building or testing `nythera-files` against library changes.
  - Rebuild `nythera-files` whenever `nythera-lib` public headers, inline/template implementations, or ABI-affecting build options (`debug`/`debugoptimized` vs `release`, sanitizers, toolchain/stdlib) change.
  - `.cpp`-only `nythera-lib` updates require reinstalling `nythera-lib` and restarting running `nythera-file-manager` processes (only when authorised), without needing to relink `nythera-files`.

## Repository workflow

1. **Orient and inspect.** Check `git status --short` before edits. Identify the owning subsystem using `rg --files`, focused `rg -n` searches and bounded `sed -n` reads. Exclude `third_party/`, build trees, generated output and caches unless relevant.
2. **Implement coherently.** Keep related edits together and avoid touching unrelated code or mass reformatting. For research-only requests, inspect and explain without editing.
3. **Validate proportionately.** Use the change-type matrix below, generally one incremental build and focused tests rather than whole-tree checks.
4. **Verify live UI when appropriate.** Launch or restart `nythera-file-manager` only after successful build/applicable tests, when necessary and explicitly authorised. Never perform privileged helper installs or modify host Polkit files without explicit authorisation.
5. **Review before finishing.** Use `git diff --check`, review the changed-file list and focused diff, and check affected architectural invariants. Recheck after subsequent edits when necessary.

## Nythera Files tool practices

- Prefer repository source to external browsing; consult external docs for unresolved API, compatibility or upstream-change questions.
- Prefer direct Git, `rg`/`sed` and focused patches for ordinary operations. Use Python or shell scripts for structured processing or reproducible diagnostics—not to wrap a single trivial command.
- Keep compiler/test output in local logs and inspect error excerpts rather than dumping full logs. Await process completion without repeated polling; check exit codes.
- Use existing captures for unchanged visual states. Capture new or cropped views when rendered behaviour changes or evidence is insufficient.
- Do not use subagents for routine work; delegate only independent bounded work where coordination costs are justified.
- Do not modify live user configuration (`~/.config/nythera`, `~/.local/state/nythera`), install packages, clean builds, or modify global system configuration without explicit authorisation.

## Choose validation by change type

| Change                                                                                      | Default validation                                                                                                                                                              |
| ------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Documentation or comments only                                                              | `git diff --check`. No C++ build/tests.                                                                                                                                         |
| Pure UI styling, spacing, geometry or animations                                            | Incremental `just build` if C++ changed; visual inspection only if an appropriate working session is available and it adds value. Do not run the entire test suite needlessly.   |
| Normal C++ behaviour changes                                                                | Incremental `just build`; run only directly related Meson tests using `meson test -C build-debug <test-name> --print-errorlogs`.                                                 |
| File operations, Wastebin, Polkit protocols, configuration, persistence or state transitions | Incremental `just build` and directly relevant tests; add a focused isolated regression test when protecting a deterministic contract not already covered.                      |
| Internal shared headers, event loop, or `meson.build` changes                               | Incremental `just build` plus a reasoned selection of affected tests; expand to full suite only when impact cannot be bounded.                                                  |

- Use the existing `build-debug/` directory (`debugoptimized` default) and incremental builds when available and correctly configured. `just build` configures if necessary; do not run `just configure` before every build.
- Do not run `just rebuild`, `just clean`, ASan, release builds, full `just lint`, or full `just test` by default.
- Prefer focused clang-tidy for touched `.cpp` files if lint is warranted: `clang-tidy -p build-debug --warnings-as-errors='*' <touched-files>`. Whole-tree `just lint` is an explicit-request/CI operation.
- Prefer formatting **only touched C++ files** under `src/` and `tests/` with the project's configured clang-format version (22+). Do not run repo-wide `just format-code` for narrow changes.
- Before a targeted Meson test, use `meson test -C build-debug --list` once if the exact test name is unknown. If no matching test exists, don't invent an arbitrary test merely to tick a box.

## Working with project documentation

- `README.md`: standalone architecture, build/test workflow, and NytheraLib relationship.
- `docs/FILE-MANAGER-CONFLICTS.md`, `docs/FILE-MANAGER-PLACES.md`, `docs/FILE-PROPERTIES.md`: subsystem design and behavioural invariants.
- `../nythera-lib/README.md`: user-local NytheraLib installation and coordinated `0.1.x` rebuild discipline.

## Git safety

- Protect uncommitted work: do not reset, overwrite, stash, rebase, force-push or merge into `main` without explicit approval and an accounted-for state.
- Use `dev` for ordinary development. Create a temporary task branch (or worktree) only for substantial or risky changes.
- Do not commit or push until explicitly authorised.

### Committing and pushing session work

- When asked to commit or push "your changes", identify the complete relevant change set from the current task or session across implementation, tests, assets, documentation, additions and deletions.
- Review the files to be staged and commit the full relevant change set; do not blindly stage the entire working tree. If ownership or inclusion is genuinely uncertain, ask before including those files.
- After pushing, verify the remote branch contains the intended commit and report what was committed, what was pushed and any relevant changes left uncommitted.

## Test Isolation and Data Safety

- All tests involving filesystem mutations must operate exclusively on isolated, disposable test fixtures.
- Never use the user's actual Wastebin, home directory contents, existing files or mounted data as destructive test targets.
- Wastebin tests must use an injected test backend or fully isolated Trash environment. Changing `XDG_DATA_HOME` alone must not be assumed to isolate mounted filesystem Wastebins.
- Tests involving GIO, GVfs, D-Bus or other system services must isolate or mock any backend capable of modifying live user data.
- Real filesystem and network mount acceptance tests are permitted using uniquely named disposable fixtures. Clean up only artefacts created by the test.
- Before running unfamiliar tests that perform destructive operations, verify that their targets and backends are safely isolated.
- If isolation cannot be guaranteed, do not run the affected test. Report the limitation instead.
- Immediately stop and report any unintended modification or deletion of user data.
- Any test constructing `FileManager`, `ConfigService`, or `FileOperationManager` must isolate `HOME`, `NYTHERA_{CONFIG,STATE,DATA}_HOME`, `XDG_{CONFIG,STATE,DATA,CACHE}_HOME`, set `GIO_USE_VFS=local`, and disable or privately isolate `DBUS_SESSION_BUS_ADDRESS` before instantiation; never fall back silently to the live session bus.
- Destructive Wastebin tests must inject an isolated `WastebinBackend` and run with fail-closed production Wastebin guards enabled; automated tests must never inspect or delete entries under a live mount's `.Trash-*` directory.
- Fixture roots must be created exclusively (e.g. `mkdtemp` or exclusive `create_directory`) and cleaned up by exact owned path only—never via prefix or wildcard sweeps over shared or mount parent directories.
- Ensure asynchronous workers and helper sessions drain cleanly before teardown, and avoid `std::exit()` or unguarded cleanup paths that skip fixture disposal or leave workers racing shutdown.
