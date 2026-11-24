# Nythera Files

A native Wayland file manager written in C++, built around the Nythera design philosophy.

Nythera Files started because I couldn't quite find a file manager that combined everything I liked about the existing options.

Dolphin has fantastic functionality, COSMIC Files has a lovely interface, and Thunar has plenty of practical touches. But none of them were quite what I wanted.

So naturally, instead of settling for one, I decided to build my own. 🤣

Originally developed as part of [Nythera Shell](https://github.com/Nythera-Dev/Nythera), Files has since grown into a standalone application using the shared [NytheraLib](https://github.com/Nythera-Dev/NytheraLib) rendering and UI infrastructure.

**Nythera Shell is not required to run it.**

## A Note on Development

Like Nythera Shell, **Nythera Files is largely vibe coded.**

I'm learning to code, but C++ isn't something I can write confidently myself. The ideas, design direction and decisions are mine, while much of the implementation is AI-assisted.

I'm not claiming to be an experienced developer, or that the code is perfect. I'm building something I want to use, learning along the way and trying to keep it maintainable.

And yes, it's already grown considerably beyond what I originally planned. 😂

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
- GIO/GVfs-based filesystem integration.

### Tools & Integration

- File, Edit, View, Tools and Help menus.
- Context-sensitive file and folder actions.
- Open With and terminal integration.
- Disk-usage and file-comparison integrations where suitable applications are available.
- MIME-aware icons and file-type handling.

## Appearance & Theming

Nythera Files shares the same general design language as Nythera Shell: dark surfaces, crimson accents, subtle effects and custom branding.

The interface uses native rendering through NytheraLib rather than depending on GTK or Qt for its application UI.

The aim is to make something visually distinctive without sacrificing the functionality expected from a file manager.

**Independent theming is still in development.** Eventually, Files should support its own themes while optionally following Nythera Shell's appearance settings.

My colour scheme shouldn't have to be everyone else's. 😁

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

It uses Meson and C++23, and requires a compatible installed version of NytheraLib.

An Arch `PKGBUILD` is provided in [`packaging/arch/`](packaging/arch/).

For development builds, use the repository's Meson configuration. Other environments have not been extensively tested.

There is no stable packaged release yet.

## Credits

Nythera Files grew out of Nythera Shell, itself a fork of [Noctalia](https://github.com/noctalia-dev/noctalia).

The underlying rendering and UI foundations owe a great deal to the original Noctalia developers and contributors.

Dolphin, Thunar, COSMIC Files and other Linux file managers have also provided plenty of inspiration.

Nythera Files is an independent project and is not affiliated with those applications or their developers.

## License

Nythera Files is licensed under the [MIT License](LICENSE).

---

I started this because I wanted a file manager that looked and behaved the way I imagined.

Somewhere along the way, that turned into building an entire application, a shared library and a small collection of repositories.

Apparently I have a problem with keeping projects small. 🤣
