# Nythera Files Agent Instructions

Repository-specific guidance supplementing global agent instructions. Follow global rules alongside this document; do not treat this file as replacing them.

## Common development rules

### Workflow, scope and tool efficiency

- **Orient and inspect:** Check `git status --short` before edits. Identify the owning subsystem with `rg --files`, focused `rg -n` searches, and bounded `sed -n` reads. Exclude `third_party/`, build trees, generated output, caches, and unrelated large assets unless relevant.
- **Implement coherently:** Keep related edits together. Avoid touching unrelated code, speculative abstractions, unnecessary refactoring, or mass reformatting. For research-only requests, inspect and explain without editing.
- **Preserve behaviour and invariants:** Preserve dark sci-fi/fantasy aesthetics, configurable styling, Umbriel compatibility, and approved architectural decisions. Surface conflicts with documented technical invariants rather than silently bypassing them.
- **Tool efficiency:** Prefer repository source over external browsing; prefer direct Git, `rg`/`sed`, and focused patches over heavy scripts (use Python or shell only for structured processing or reproducible diagnostics). Keep compiler/test output in local logs and inspect excerpts rather than dumping full logs. Await completion without repeated polling; check exit codes. Do not use subagents for routine work; delegate only independent bounded work where coordination costs are justified. Reuse existing visual captures for unchanged visual states.

### Build, formatting and validation safeguards

- Use the existing `build-debug/` directory (`debugoptimized` default) and incremental `just build` (which configures if necessary; do not run `just configure` before every build).
- Do not run `just rebuild`, `just clean`, ASan, release builds, full `just lint`, or full `just test` by default.
- Format **only touched C++ files** under `src/` and `tests/` with the project's configured `clang-format` version (22+). Do not run repo-wide `just format-code` for narrow changes.
- Prefer focused `clang-tidy -p build-debug --warnings-as-errors='*' <touched-files>` when lint is warranted; whole-tree `just lint` is an explicit-request/CI operation.
- Before a targeted Meson test, use `meson test -C build-debug --list` once if the exact test name is unknown, then run `meson test -C build-debug <test-name> --print-errorlogs`. Add a focused isolated regression test when protecting a deterministic contract not already covered; do not invent arbitrary tests merely to tick a box.
- **Review before finishing:** Run `git diff --check`, review the changed-file list and focused diff, check affected architectural invariants, and report actual checks run, scope, and unresolved items. Never cite unverified claims or inferred test passes.

- **Runtime compatibility during coordinated changes:** Before changing shared assets, resource layouts, public library interfaces or other files consumed at runtime, account for currently running applications and any subprocesses/helpers they may launch. Preserve compatibility with the active runtime until the coordinated build, installation and authorised restart are complete. If compatibility cannot be maintained, stop before the incompatible change and agree a safe transition and recovery plan with the maintainer. Validate affected runtime paths, including security-critical helpers where relevant, before removing temporary compatibility measures. Never weaken authentication or other safety checks to conceal a deployment mismatch.

- **Shell restart safety (NoNewPrivs):** Before any authorised live Nythera Shell restart, including coordinated downstream restarts, check `NoNewPrivs` in `/proc/<pid>/status` for the actual launching process/environment. Never launch or restart live Shell from an environment with `NoNewPrivs: 1`; this restriction is inherited and cannot be cleared by exec or disown. If the launch environment is restricted or cannot be verified, leave the restart to the maintainer from an unrestricted terminal or use an explicitly authorised, verified unrestricted launch mechanism. After restarting, verify `NoNewPrivs: 0` in `/proc/<nythera-pid>/status` for the actual running Shell before declaring deployment successful or asking the maintainer to test locking. On 26 November 2026, an agent-launched Shell with `NoNewPrivs: 1` failed lock-screen PAM authentication with “Authentication service cannot access authentication info”; restarting from the maintainer's unrestricted terminal restored `NoNewPrivs: 0` and successful lock/unlock. Do not change PAM or authentication permissions to work around this deployment problem.

### Authorisation and Git safety

- Do not modify live user configuration (`~/.config/nythera`, `~/.local/state/nythera`), install host packages, modify global system configuration, restart running applications/services, commit, or push without explicit authorisation.
- Protect uncommitted work: do not reset, overwrite, stash, rebase, force-push, or merge into `main` without explicit approval and an accounted-for state.
- Use `dev` for ordinary development; create a temporary task branch or worktree only for substantial or risky changes.
- When authorised to commit or push "your changes", identify and stage the complete relevant session change set across implementation, tests, assets, and docs (never blindly staging the entire working tree; ask if ownership is uncertain). Verify the remote branch after pushing and report what was committed, what was pushed, and any relevant uncommitted files.

### Baseline test isolation and data safety

- All tests involving filesystem mutations or user/system services must operate exclusively on isolated, disposable fixtures—never touching live user config, home directory contents, or the live session bus.
- Any test constructing `ConfigService` or application services must isolate `HOME`, `NYTHERA_{CONFIG,STATE,DATA}_HOME`, `XDG_{CONFIG,STATE,DATA,CACHE}_HOME`, set `GIO_USE_VFS=local`, and disable or privately isolate `DBUS_SESSION_BUS_ADDRESS` before instantiation; never fall back silently to the live session bus.
- Create fixture roots exclusively (`mkdtemp` or exclusive `create_directory`) and clean up by exact owned path only—never via prefix or wildcard sweeps.
- Drain asynchronous workers and helper sessions cleanly before teardown; avoid `std::exit()` or unguarded paths that skip fixture disposal. Immediately stop and report any unintended user-data modification or isolation gap.

---

## Nythera Files context and invariants

- **Nythera Files** is a standalone C++23 native Wayland file manager (`nythera-file-manager`) with Polkit helpers (`nythera-properties-helper`, `nythera-admin-helper`) and its own application asset bundle (`files-assets/`), built with **Meson/Ninja** and orchestrated by `just`.
- Development primarily targets **Umbriel**. Preserve dark sci-fi/fantasy aesthetics and configurable styling rendered through **NytheraLib**.
- Maintain strict independence from **Nythera Shell**: do not introduce build-time or mandatory runtime dependencies on `nythera-shell` sources, IPC, or Shell-owned assets (`FileManager` reads shared Shell config read-only and falls back to built-in palettes).
- **Asset ownership and GVfs:** `files-assets/` owns Files-specific templates, dialogue artwork, and icons; shared fonts, Tabler glyph metadata, and library translations come from **NytheraLib**. Keep GVfs optional with graceful local-filesystem fallback when GVfs or individual backends are unavailable.
- Preserve Polkit action IDs (`data/polkit/dev.nythera.Files.*.policy.in`), helper IPC contracts, and root-ownership security checks. Do not run privileged installs (`sudo just install`, `makepkg -si`), modify host Polkit files, or mix `/usr/local/libexec/` and `/usr/lib/` helper prefixes without explicit authorisation.
- Launch or restart `nythera-file-manager` only after successful build/applicable tests, when necessary and explicitly authorised.

### Architectural guardrails

- Keep `FileManager` and other coordinators focused on orchestration. Extract cohesive behaviour and its state into encapsulated controllers. Splitting member definitions across translation units leaves ownership with `FileManager`.
- Build specialised dialogs as controllers using `ModalDialogHost::CustomContent`. Centralise menu action IDs and reuse existing shared helpers for common behaviour.
- Keep backend operation families in dedicated implementations and keep shared execution logic with its owning family. Pass narrow operation context values across backend boundaries so backends stay independent of UI objects.
- Keep Files-specific serialization, namespaces and UI semantics in Files. Use NytheraLib for application-agnostic infrastructure and preserve its public ABI where possible.
- Decompose cohesive implementations only when the change demonstrably improves maintainability, correctness, testability or reuse. File size alone does not justify extraction.

## Cross-repository dependencies (NytheraLib)

- `nythera-files` dynamically links `nythera-lib` (`>=0.1.0, <0.2.0`) via `pkg-config`.
- `justfile` automatically discovers `$HOME/.local/opt/nythera-lib/lib/pkgconfig` when `nythera-lib` is not already found by system `pkg-config` or an explicit `PKG_CONFIG_PATH`, without permanently modifying the shell environment.
- Follow the coordinated `0.1.x` rebuild discipline in `../nythera-lib/README.md`: build and install `nythera-lib` before building or testing `nythera-files` against library changes; rebuild `nythera-files` whenever `nythera-lib` public headers, inline/template implementations, or ABI-affecting build options (`debug`/`debugoptimized` vs `release`, sanitizers, toolchain/stdlib) change; restart running `nythera-file-manager` processes (only when authorised) to load reinstalled `.cpp`-only library updates.

## Choose validation by change type

| Change                                                                                      | Default validation                                                                                                                                                              |
| ------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Documentation or comments only                                                              | `git diff --check`. No C++ build/tests.                                                                                                                                         |
| Pure UI styling, spacing, geometry or animations                                            | Incremental `just build` if C++ changed; visual inspection only if an appropriate working session is available and it adds value. Do not run the entire test suite needlessly.   |
| Normal C++ behaviour changes                                                                | Incremental `just build`; run only directly related Meson tests using `meson test -C build-debug <test-name> --print-errorlogs`.                                                 |
| File operations, Wastebin, Polkit protocols, configuration, persistence or state transitions | Incremental `just build` and directly relevant tests; add a focused isolated regression test when protecting a deterministic contract not already covered.                      |
| Internal shared headers, event loop, or `meson.build` changes                               | Incremental `just build` plus a reasoned selection of affected tests; expand to full suite only when impact cannot be bounded.                                                  |

## Files-specific test isolation and data safety

- Never use the user's actual Wastebin, home directory contents, existing files, or mounted data as destructive test targets.
- Wastebin tests must use an injected isolated `WastebinBackend` or fully isolated Trash environment with fail-closed production Wastebin guards enabled; changing `XDG_DATA_HOME` alone must not be assumed to isolate mounted filesystem Wastebins, and automated tests must never inspect or delete entries under a live mount's `.Trash-*` directory.
- Tests involving GIO, GVfs, D-Bus, or other system services must isolate or mock any backend capable of modifying live user data.
- Any test constructing `FileManager` or `FileOperationManager` must enforce full `HOME`, `NYTHERA_*`, `XDG_*`, `GIO_USE_VFS=local`, and `DBUS_SESSION_BUS_ADDRESS` isolation before instantiation.
- Real filesystem and network mount acceptance tests are permitted only using uniquely named disposable fixtures (`mkdtemp` / exclusive `create_directory`); clean up only artefacts created by the test.
- Before running unfamiliar tests that perform destructive operations, verify that their targets and backends are safely isolated; if isolation cannot be guaranteed, do not run the test and report the limitation instead.

## Working with project documentation

- `README.md`: standalone architecture, build/test workflow, optional GVfs behaviour, and NytheraLib relationship.
- `docs/FILE-MANAGER-CONFLICTS.md`, `docs/FILE-MANAGER-PLACES.md`, `docs/FILE-PROPERTIES.md`: subsystem design and behavioural invariants.
- `../nythera-lib/README.md`: user-local NytheraLib installation and coordinated `0.1.x` rebuild discipline.
