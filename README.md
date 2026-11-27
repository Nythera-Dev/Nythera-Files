# Nythera Files

A native Wayland file manager written in C++, built around the Nythera design philosophy.

Nythera Files started because I couldn't quite find a file manager that combined everything I liked about the existing options.

Dolphin has fantastic functionality, COSMIC Files has a lovely interface, and Thunar has plenty of practical touches. But none of them were quite what I wanted.

So naturally, instead of settling for one, I decided to build my own 🤣

Originally developed as part of [Nythera Shell](https://github.com/Nythera-Dev/Nythera), Files has since grown into a standalone application using the shared [NytheraLib](https://github.com/Nythera-Dev/NytheraLib) rendering and UI infrastructure.

**Nythera Shell is not required to run it.**

## A Note on Development

Like Nythera Shell, **Nythera Files is largely vibe coded.**

I'm learning to code, but C++ isn't something I can write confidently myself. The ideas, design direction and decisions are mine, while much of the implementation is AI-assisted.

I'm not claiming to be an experienced developer, or that the code is perfect. I'm building something I want to use, learning along the way and trying to keep it maintainable.

And yes, it's already grown considerably beyond what I originally planned 😂

## Screenshots

### List View

<img width="1513" height="853" alt="image" src="https://github.com/user-attachments/assets/b37a000b-7735-404b-b8f7-17e0714f8be4" />

### Grid View

<img width="1516" height="855" alt="image" src="https://github.com/user-attachments/assets/e5c72c5f-fc90-4136-8858-a29d879289ce" />

## What Can It Do?

Nythera Files is already a usable file manager, rather than just a visual experiment.

### Browsing & Navigation

- List and grid views with adjustable scaling.
- Multiple tabs, including tab reordering and reopening closed tabs.
- Breadcrumb navigation and directory history.
- **Search (Ctrl+F)** finds files and folders by part of their name in the current folder and its subfolders. Results appear as the search runs. Use **Open Containing Folder** on a result or **Esc** to return to browsing. Search follows the tab's hidden-file setting and skips inaccessible folders and symlink traversal. Search runs with your regular user permissions and is available in regular folder tabs. Eligible visible results on local filesystems generate thumbnails asynchronously. Other results use fast file-type icons. Opening a file or choosing an application can use its full file type.
- Sorting, file selection and drag-and-drop.
- Configurable details-view columns.
- File and folder properties.

### File Operations

- Copy, move, rename, duplicate and delete.
- File and folder creation, including templates.
- Symlink creation and management.
- Conflict resolution and directory merging.
- Undo and redo for supported operations.
- Background transfers with progress reporting.
- Wastebin browsing and file restoration.
- Administrator Mode for supported privileged operations.

### Places & Devices

- Personal Places and bookmarked directories.
- Local devices, mounted volumes and network locations.
- Mount management and filesystem capacity information.
- Optional GIO/GVfs integration: `gvfs` provides `trash://` Wastebin browsing, GVfs volume/remote mounting, and FUSE mount paths, while protocol backends (such as `gvfs-smb`, `gvfs-mtp`, and `gvfs-nfs`) extend remote location support. When GVfs or individual backends are unavailable, local browsing, file operations, and UDisks2/systemd mount tracking continue working gracefully.

### Tools & Integration

- File, Edit, View, Tools and Help menus.
- Context-sensitive file and folder actions.
- Open With and terminal integration.
- Disk-usage and file-comparison integrations where suitable applications are available.
- MIME-aware icons and file-type handling.

### Search scopes

Search starts in **Current folder & subfolders**. The scope menu also offers **Current folder only**, **Home**, **Local drives**, **All drives**, and named **Custom searches**. Local drives searches home and mounted local storage. All drives also searches mounted network storage such as SMB and NFS. For either drive scope, **Include system directories** adds the root filesystem. Files discovers storage that is already mounted and skips untriggered automounts.

The results view opens when you enter a filename query or activate a valid filter. Clearing the query and all active filters returns to browsing with the search bar still open. The scope menu saves **Show hidden items** and **Follow symbolic links** for standard scopes and separately for each custom search. Both options start off. Following links searches directory targets within their eligible source and lists descendants at their resolved locations. Exclusions and mount boundaries still apply.

Use **Define custom search…** to save included folders and optional additional exclusions, one absolute folder path per line. **Manage custom searches…** lets you edit, rename or delete these profiles. Entering `/` reveals **Include mounted filesystems**. Leave it unchecked to search the root filesystem only, or check it to add mounted local and network storage. Explicitly listed mount paths are searched independently.

Set global exclusions in `$XDG_CONFIG_HOME/nythera/files/search/config.toml` (or `$NYTHERA_CONFIG_HOME/files/search/config.toml` when using the Nythera override). This file also stores profiles saved through the editor.

```toml
exclusions = ["/home/me/private", "/mnt/storage/staging"]
```

Global and profile exclusions are applied before traversal. Virtual and kernel-managed filesystems such as `/proc`, `/sys`, and `/dev` are always skipped.

### Optional fzf search

Files uses an installed `fzf` from `PATH` when it is available. The search bar offers Contains, Fuzzy, Starts with, Ends with, Exclude, Exact match and None. Contains is the default. The mode adds matching operators internally, while the search field keeps the text you entered. Explicit fzf operators override the selected mode. Compound expressions use None, which also accepts raw fzf syntax. A multi-word query without operators is treated as one phrase by the named modes.

Set these preferences in the Files `config.toml` to disable fzf or change the default mode.

```toml
[files]
fzf_search = false
search_default_mode = "contiguous"
```

The default-mode values are `contiguous`, `fuzzy`, `starts_with`, `ends_with`, `exclude`, `exact` and `none`. If fzf cannot start or fails during search, Files runs the query again using conventional substring matching. Search results have their own saved list or grid view, column layout, and sort order. The default list shows Name, Location, Size, Modified and Type. The Location column and grid subtitles show the absolute containing-directory path. Hover over a result to see the full path. Files counts immediate items in visible matching folders when the Size column is shown.

In either list view, drag a column's right divider to resize it, or double-click the divider to fit its displayed contents. Drag a heading to reorder columns. Right-click the header for visibility and sizing options. When columns extend beyond the viewport, scroll horizontally with the scrollbar, a touchpad, or Shift+wheel. Browsing and search remember their own column widths and order.

### Search filters

Open the sliders button in the search toolbar to filter by file size, modification date or contents. The button displays the number of active filters. Collapsing the panel keeps them active. **Clear Filters** beside the sliders removes them together. Empty metadata values leave that filter inactive, and Between requires both values. A filename query and any active filters must all match. Searches continue when you switch tabs. Press **Esc** during an active search to stop it and keep its current results. Press **Esc** again to close the search.

- **File size** supports bytes and decimal or binary units. Between includes both endpoints. Fractional units must resolve to a whole number of bytes.
- **Date modified** uses your local calendar. Before and After exclude the selected day. Between includes both days. Relative months and years use calendar arithmetic.
- **File contents** applies the query and its options when you press **Search** or **Enter**. You can edit a nonempty draft while the last submitted content query remains active. Clearing the content field removes its filter. Plain text uses ripgrep (`rg`). Extracted text uses ripgrep-all (`rga` and `rga-preproc`) and its default extension-based adapters with available dependencies. For example, PDF needs `pdftotext`, several document formats need Pandoc, and media metadata needs FFmpeg. Mail extraction is disabled. If only ripgrep is available, **All supported files** searches plain text. Filename and metadata search work independently of these tools.

Plain-text search runs with time and memory limits. Files larger than 128 MiB use a separate search lane. Extracted text has its own queue, a 128 MiB input limit, and a three-level archive limit. The search status reports access failures, extraction limits and failures, and timed-out batches. Files uses a separate ripgrep-all cache at `$XDG_CACHE_HOME/nythera-files/search-content/ripgrep-all-v2` (under `~/.cache` when the XDG cache variable is unset).

## Appearance & Theming

Nythera Files shares the same general design language as Nythera Shell: dark surfaces, crimson accents, subtle effects and custom branding.

The interface uses native rendering through NytheraLib rather than depending on GTK or Qt for its application UI.

The aim is to make something visually distinctive without sacrificing the functionality expected from a file manager.

**Independent theming is still in development.** Eventually, Files should support its own themes while optionally following Nythera Shell's appearance settings.

My colour scheme shouldn't have to be everyone else's 😁

## Files preferences and state

Files keeps sparse preferences in `$XDG_CONFIG_HOME/nythera/files/config.toml` and its transparency override in `$XDG_CONFIG_HOME/nythera/files/theme.toml`. Window and pane sizes and sidebar expansion are runtime state in `$XDG_STATE_HOME/nythera/files/state.toml`. The existing `NYTHERA_CONFIG_HOME` and `NYTHERA_STATE_HOME` overrides take precedence over the XDG variables.

In `config.toml`:

```toml
[files]
show_hidden = true
view_scale = 1.2

[files.saved_places."work"]
path = "/home/me/Work"
label = "Work"
```

Set `files.readline_shortcuts` explicitly to override Shell's keyboard preference. In `theme.toml`, set `theme.transparency_mode` to `solid`, `soft`, or `glass` to override Shell's transparency. With these overrides omitted, Files uses the Shell configuration when available and falls back to built-in settings for standalone use.

Files loads TOML files in its config directory in filename order. The `files` array in an `[include]` table names files or directories that load before the including file. Setting `autoload = false` limits discovery to the opting-out roots and their includes. Keep the primary files within that load set if GUI edits should be able to create settings there.

GUI changes edit the winning definition or use the owning table's file for a new setting. New tables use the corresponding primary file. To restore a default manually, remove every definition of the setting. Invalid edits leave the last valid configuration active. Saved Places can use an optional nonnegative `order` value to retain their insertion order when there is no explicit sidebar ordering.

Existing installations need their legacy Files preferences and runtime geometry transferred from the shared state file before switching to this layout. The new runtime leaves the old state file in place.

## How It Fits Together

Nythera is split into three related projects:

- [Nythera Shell](https://github.com/Nythera-Dev/Nythera) — The desktop shell.
- [NytheraLib](https://github.com/Nythera-Dev/NytheraLib) — Shared rendering, Wayland and UI infrastructure.
- **Nythera Files** — The standalone file manager.

Files links dynamically against the independently installed NytheraLib library.

The applications are developed together, but Files does not need the Shell to be running.

## What's Next?

There's still quite a bit on the to-do list.

Some areas I'm working towards include improved search, independent theming, further sidebar refinements, additional filesystem integrations and continued UI polish.

The priority is making Files a reliable everyday application, not adding features simply for the sake of it.

You can follow development and report issues through the [GitHub issue tracker](https://github.com/Nythera-Dev/Nythera-Files/issues).

## Building & Installation

Nythera Files is currently experimental and primarily developed on **Arch Linux with Umbriel**.

It uses Meson, `just`, and C++23, and requires a compatible installed version of NytheraLib (`>=0.1.0, <0.2.0`).

For local development after building and installing NytheraLib:

```sh
just configure
just build
just test
```

The default `debug` mode uses `build-debug/` configured with Meson's `debugoptimized` build type and `--prefix /usr/local`. NytheraLib is expected in system `pkg-config` or at the user-local development prefix `$HOME/.local/opt/nythera-lib` (`libdir=lib`); when `nythera-lib` is not already resolvable via system `pkg-config` or an explicit `PKG_CONFIG_PATH`, `just` automatically falls back to `$HOME/.local/opt/nythera-lib/lib/pkgconfig` while keeping explicit paths and system installs prioritised. Shared Nythera fonts, Tabler glyph metadata, and library component translations are supplied by NytheraLib (`share/nythera-lib/assets/`), while `files-assets/` provides Files-specific templates, dialogue artwork, and icons.

During coordinated `0.1.x` development, always build and install `nythera-lib` before building `nythera-files`. Rebuild `nythera-files` whenever `nythera-lib` public headers, inline/template implementations, or ABI-affecting build options change, and restart running `nythera-file-manager` processes to load reinstalled `.cpp`-only library updates (see NytheraLib's `README.md`).

### Installation conventions (development, packaged, and unprivileged)

- **Development privileged helper installation (`/usr/local`):** The default `just` workflow configures `--prefix /usr/local` (`libexecdir=libexec`). Running `sudo just install debug` (or installing only the helpers and policies with `sudo meson install --no-rebuild -C build-debug --tags admin-helper,properties-helper`) installs `nythera-admin-helper` and `nythera-properties-helper` under `/usr/local/libexec/` and writes `dev.nythera.Files.*.policy` into `/usr/share/polkit-1/actions/` (via `polkit-gobject-1`'s `policydir`).
- **Package-managed installation (`packaging/arch/PKGBUILD`):** `arch-meson` configures `--prefix=/usr --libexecdir=lib`, installing helpers under `/usr/lib/` while also writing `dev.nythera.Files.*.policy` into `/usr/share/polkit-1/actions/`. Because both mechanisms share `/usr/share/polkit-1/actions/` and embed their configured helper paths into the binary and policy XML, never mix `/usr/local/libexec/` and `/usr/lib/` helper installations on one host; remove obsolete helpers and policy files from the unused prefix before switching.
- **Unprivileged user-prefix and staged installs:** Because `policydir` always resolves to `/usr/share/polkit-1/actions/`, an unprivileged `meson install` fails unless Polkit targets are excluded or `DESTDIR` is used. To install the unprivileged application and `files-assets/` into a user prefix without root or Polkit files:

```sh
just configure debug "$HOME/.local"
just build
meson install --no-rebuild -C build-debug --tags files,files-assets
```

To inspect a complete installation tree (including helpers and policy files) without modifying host paths, use `DESTDIR`:

```sh
DESTDIR=/path/to/stage meson install --no-rebuild -C build-debug
```

Polkit execution checks require root ownership and non-writable ancestors down to `/`, so elevated Properties and Administrator Mode actions remain disabled in unprivileged or staged installs. Other environments outside Arch Linux have not been extensively tested.

The optional [native archive backend](docs/FILE-MANAGER-ARCHIVES.md) is built with the standalone file manager. Archive actions will be connected in a later stage.

There is no stable packaged release yet.

## Credits

Nythera Files grew out of Nythera Shell, itself a fork of [Noctalia](https://github.com/noctalia-dev/noctalia).

The underlying rendering and UI foundations owe a great deal to the original Noctalia developers and contributors. Recursive filename search adapts the GIO traversal and Unicode matching from [Luciole](https://github.com/noctalia-dev/luciole/tree/cb6375a5e046ae9ce3eaba7d12ba53a85e2c769a/src/fs) under the MIT licence.

Dolphin, Thunar, COSMIC Files and other Linux file managers have also provided plenty of inspiration.

Nythera Files is an independent project and is not affiliated with those applications or their developers.

## License

Nythera Files is licensed under the [MIT License](LICENSE).

---

I started this because I wanted a file manager that looked and behaved the way I imagined.

Somewhere along the way, that turned into building an entire application, a shared library and a small collection of repositories.

Apparently I have a problem with keeping projects small. 🤣
