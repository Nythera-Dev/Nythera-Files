#include "filemanager/search_scope.h"

#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <linux/openat2.h>
#include <ranges>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace filemanager {
  namespace {
    namespace fs = std::filesystem;
    using Kind = SearchMount::Kind;

    fs::path normalPath(const fs::path& path) {
      auto normal = path.lexically_normal();
      if (normal == normal.root_path())
        return normal.root_path();
      if (!normal.has_filename())
        normal = normal.parent_path();
      return normal;
    }

    bool under(const fs::path& parent, const fs::path& child) {
      const auto& prefix = parent.native();
      const auto& path = child.native();
      return path == prefix || (path.starts_with(prefix) && (prefix == "/" || path[prefix.size()] == '/'));
    }

    std::string unescapeMountField(std::string_view field) {
      std::string value;
      for (std::size_t i = 0; i < field.size(); ++i) {
        const auto octal = [](char c) { return c >= '0' && c <= '7'; };
        if (field[i] == '\\'
            && i + 3 < field.size()
            && octal(field[i + 1])
            && octal(field[i + 2])
            && octal(field[i + 3])) {
          value += static_cast<char>((field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + field[i + 3] - '0');
          i += 3;
        } else {
          value += field[i];
        }
      }
      return value;
    }

    Kind mountKind(std::string_view type, std::string_view source) {
      if (type == "autofs")
        return Kind::Automount;
      constexpr std::string_view blocked[]{"proc",      "sysfs",      "devtmpfs", "devpts",      "cgroup",
                                           "cgroup2",   "securityfs", "debugfs",  "tracefs",     "configfs",
                                           "pstore",    "efivarfs",   "bpf",      "mqueue",      "hugetlbfs",
                                           "fusectl",   "rpc_pipefs", "nfsd",     "binfmt_misc", "nsfs",
                                           "selinuxfs", "smackfs",    "resctrl",  "functionfs",  "ramfs",
                                           "bpffs"};
      if (std::ranges::contains(blocked, type))
        return Kind::Blocked;
      constexpr std::string_view network[]{"nfs",      "nfs4",       "cifs",         "smb3",        "smbfs",
                                           "9p",       "afs",        "ceph",         "glusterfs",   "fuse.glusterfs",
                                           "davfs",    "fuse.davfs", "fuse.sshfs",   "fuse.rclone", "fuse.gvfsd-fuse",
                                           "virtiofs", "lustre",     "fuse.smbnetfs"};
      if (std::ranges::contains(network, type))
        return Kind::Network;
      constexpr std::string_view local[]{"btrfs",   "bcachefs", "zfs",     "ext2",  "ext3",    "ext4",
                                         "xfs",     "f2fs",     "exfat",   "vfat",  "ntfs",    "ntfs3",
                                         "fuseblk", "udf",      "iso9660", "erofs", "squashfs"};
      if (source.starts_with("/dev/") || std::ranges::contains(local, type))
        return Kind::Local;
      return Kind::Other;
    }
  } // namespace

  std::string trimSearchText(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == text.npos)
      return {};
    return std::string(text.substr(first, text.find_last_not_of(whitespace) - first + 1));
  }

  SearchLocations searchLocationLines(std::string_view text) {
    SearchLocations locations;
    for (auto line : text | std::views::split('\n')) {
      if (auto path = trimSearchText(std::string_view(line.begin(), line.end())); !path.empty())
        locations.emplace_back(std::move(path));
    }
    return locations;
  }

  bool validSearchLocation(const fs::path& path) {
    return path.is_absolute() && path.native().find('\0') == std::string::npos;
  }

  bool validCustomSearchName(std::string_view name) {
    return !name.empty()
        && trimSearchText(name) == name
        && std::ranges::none_of(name, [](unsigned char c) { return c < 32 || c == 127; });
  }

  bool explicitlyIncludesRoot(const SearchLocations& paths) {
    return std::ranges::any_of(paths, [](const auto& path) { return path.native() == "/"; });
  }

  std::vector<SearchMount> readSearchMounts(std::istream& input) {
    std::vector<SearchMount> mounts;
    for (std::string line; std::getline(input, line);) {
      std::istringstream fields(line);
      std::string id, parent, device, root, path, field, type, source;
      if (!(fields >> id >> parent >> device >> root >> path))
        continue;
      while (fields >> field && field != "-") {
      }
      if (!(fields >> type >> source))
        continue;
      const auto target = fs::path(unescapeMountField(path));
      if (validSearchLocation(target))
        mounts.push_back({target, unescapeMountField(root), std::move(device), mountKind(type, source)});
    }
    // Keep the innermost mount at a stacked mount point.
    std::ranges::reverse(mounts);
    std::unordered_set<std::string> seen;
    std::erase_if(mounts, [&](const auto& mount) { return !seen.insert(mount.path.native()).second; });
    return mounts;
  }

  std::vector<SearchMount> discoverSearchMounts() {
    std::ifstream input("/proc/self/mountinfo");
    return readSearchMounts(input);
  }

  bool searchPathExcluded(const SearchPlan& plan, const fs::path& path) {
    if (plan.exclusions.empty())
      return false;
    // Query component prefixes without allocating a path for each ancestor.
    std::string_view prefix = path.native();
    if (plan.exclusions.contains("/"))
      return true;
    while (prefix.size() > 1) {
      if (plan.exclusions.contains(prefix))
        return true;
      prefix = prefix.substr(0, prefix.find_last_of('/'));
    }
    return false;
  }

  int openSearchDescendant(int root, const char* relative, unsigned long flags) {
    const open_how how{.flags = flags, .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS};
    return static_cast<int>(syscall(SYS_openat2, root, relative, &how, sizeof(how)));
  }

  OpenSearchPath::OpenSearchPath(const SearchPlan& plan, const fs::path& path, const SearchPathBoundary* boundary) {
    if (!validSearchLocation(path))
      return;
    auto pending = boundary ? path : normalPath(path);
    for (int links = 0; links <= 40; ++links) {
      const fs::path base = boundary ? boundary->root : fs::path("/");
      if ((boundary && !under(base, pending)) || searchPathExcluded(plan, base))
        return;
      int parent = boundary ? fcntl(boundary->fd, F_DUPFD_CLOEXEC, 0) : open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
      if (parent < 0)
        return;
      auto prefix = base;
      auto part = std::next(pending.begin(), std::distance(base.begin(), base.end()));
      bool restart = false;
      for (; part != pending.end(); ++part) {
        if (boundary && (*part == "." || part->empty()))
          continue;
        if (boundary && *part == "..") {
          close(parent);
          if (prefix == base)
            return;
          // Keep the kernel's symlink/.. semantics without resolving a parent outside the pinned root.
          auto target = prefix.parent_path();
          for (++part; part != pending.end(); ++part)
            target /= *part;
          pending = std::move(target);
          restart = true;
          break;
        }
        prefix /= *part;
        constexpr auto flags = O_PATH | O_NOFOLLOW | O_CLOEXEC;
        const bool allowed =
            !plan.exclusions.contains(prefix.native()) && (!boundary || boundary->allowChild(prefix, parent));
        int fd = -1;
        if (allowed)
          fd = boundary ? openSearchDescendant(parent, part->c_str(), flags) : openat(parent, part->c_str(), flags);
        close(parent);
        parent = fd;
        struct statx info{};
        if (fd < 0)
          return;
        if (statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_TYPE, &info) != 0
            || (info.stx_attributes & STATX_ATTR_AUTOMOUNT) != 0) {
          close(fd);
          return;
        }
        if (!S_ISLNK(info.stx_mode)) {
          if (boundary && std::next(part) != pending.end() && !S_ISDIR(info.stx_mode)) {
            close(fd);
            return;
          }
          continue;
        }
        // An empty pathname reads the already opened symlink, even if its name changed.
        std::array<char, 4096> buffer;
        const auto length = readlinkat(fd, "", buffer.data(), buffer.size());
        close(fd);
        if (length < 0 || static_cast<std::size_t>(length) == buffer.size())
          return;
        const fs::path link(std::string_view(buffer.data(), static_cast<std::size_t>(length)));
        auto target = link.is_absolute() ? link : prefix.parent_path() / link;
        for (++part; part != pending.end(); ++part)
          target /= *part;
        pending = boundary ? std::move(target) : normalPath(target);
        restart = true;
        break;
      }
      if (!restart) {
        m_fd = parent;
        m_resolved = std::move(prefix);
        return;
      }
    }
  }

  OpenSearchPath::~OpenSearchPath() {
    if (m_fd >= 0)
      close(m_fd);
  }

  int OpenSearchPath::release() { return std::exchange(m_fd, -1); }

  fs::path OpenSearchPath::fdPath(int fd) { return "/proc/self/fd/" + std::to_string(fd); }

  SearchPlan resolveSearchScope(
      const SearchSelection& selection, const fs::path& current, const fs::path& home, const CustomSearch* profile,
      const SearchLocations& globalExclusions, const std::vector<SearchMount>& mounts, bool followSymlinks
  ) {
    using enum SearchScope;
    SearchPlan plan;
    plan.recursive = selection.scope != Folder;
    plan.followSymlinks = selection.scope == Custom && profile ? profile->options.followSymlinks : followSymlinks;
    plan.reportRootError = selection.scope == Recursive || selection.scope == Folder || selection.scope == Home;
    for (const auto& path : {"/proc", "/sys", "/dev"})
      plan.exclusions.emplace(path);
    const auto exclude = [&](const SearchLocations& locations) {
      for (const auto& path : locations) {
        if (validSearchLocation(path))
          plan.exclusions.insert(normalPath(path).native());
      }
    };
    exclude(globalExclusions);
    if (selection.scope == Custom && profile)
      exclude(profile->excluded);

    for (const auto& mount : mounts) {
      if (mount.path != "/")
        plan.boundaries.insert(mount.path.native());
      if (mount.kind == Kind::Blocked)
        plan.exclusions.insert(mount.path.native());
      if (mount.kind == Kind::Automount)
        plan.suppressedMounts.insert(mount.path.native());
    }

    const auto ownerOf = [&](const fs::path& path) {
      const SearchMount* owner = nullptr;
      for (const auto& mount : mounts) {
        if (under(mount.path, path) && (!owner || mount.path.native().size() > owner->path.native().size()))
          owner = &mount;
      }
      return owner;
    };
    const auto explicitExclusions = plan.exclusions;
    for (const auto& path : explicitExclusions) {
      const auto* owner = ownerOf(path);
      if (!owner || owner->kind == Kind::Blocked || owner->device.empty())
        continue;
      const auto coordinate = normalPath(owner->filesystemPath / fs::path(path).lexically_relative(owner->path));
      for (const auto& alias : mounts) {
        if (&alias == owner || alias.device != owner->device || !under(alias.filesystemPath, coordinate))
          continue;
        const auto alternate = normalPath(alias.path / coordinate.lexically_relative(alias.filesystemPath));
        if (ownerOf(alternate) == &alias)
          plan.exclusions.insert(alternate.native());
      }
    }

    std::unordered_set<std::string> selected;
    const auto add = [&](const fs::path& path, const fs::path& visibleFrom = {}) {
      if (!validSearchLocation(path))
        return;
      auto normal = normalPath(path);
      if (searchPathExcluded(plan, normal)
          || plan.suppressedMounts.contains(normal.native())
          || !selected.insert(normal.native()).second)
        return;
      const auto* owner = ownerOf(normal);
      plan.roots.push_back(
          {std::move(normal), owner && owner->kind == Kind::Network, owner ? owner->device : "", visibleFrom}
      );
    };

    switch (selection.scope) {
    case Recursive:
    case Folder:
      add(current);
      break;
    case Home:
      add(home);
      break;
    case Local:
    case All:
      add(home);
      if (selection.includeSystem)
        add("/");
      for (const auto& mount : mounts) {
        if (mount.path != "/" && (mount.kind == Kind::Local || (selection.scope == All && mount.kind == Kind::Network)))
          add(mount.path);
      }
      break;
    case Custom:
      if (profile) {
        for (const auto& path : profile->included)
          add(path);
      }
      break;
    }

    if (plan.recursive
        && selection.scope != Local
        && selection.scope != All
        && (selection.scope != Custom || profile)) {
      // Separate mounted subtrees into their own sources. An explicit / is
      // single-filesystem unless the profile opts into storage mounts.
      const auto explicitRoots = plan.roots;
      for (const auto& mount : mounts) {
        if (mount.kind != Kind::Blocked && mount.kind != Kind::Automount && mount.path != "/")
          plan.nestedMounts.push_back(mount);
        const SearchRoot* parent = nullptr;
        for (const auto& root : explicitRoots) {
          if (mount.path != root.path
              && under(root.path, mount.path)
              && (!parent || root.path.native().size() > parent->path.native().size()))
            parent = &root;
        }
        if (!parent)
          continue;
        if (parent->path == "/"
            && selection.scope == Custom
            && (!profile->includeMounts
                || !explicitlyIncludesRoot(profile->included)
                || (mount.kind != Kind::Local && mount.kind != Kind::Network)))
          continue;
        add(mount.path, parent->path == "/" && selection.scope == Custom ? fs::path{} : parent->path);
      }
    }

    // Broad scopes search mounted directories as independent roots. The
    // parent scan must not hydrate their mountpoint entries on its own lane.
    if (selection.scope == Local || selection.scope == All || (selection.scope == Custom && selected.contains("/"))) {
      for (const auto& mount : mounts) {
        if (mount.path != "/")
          plan.suppressedMounts.insert(mount.path.native());
      }
    }
    // A nested root is visited independently, even if hidden from an ancestor.
    for (const auto& root : plan.roots) {
      plan.boundaries.insert(root.path.native());
      if (!root.visibleFrom.empty())
        plan.suppressedMounts.insert(root.path.native());
    }
    plan.reportRootError = plan.reportRootError && plan.roots.size() == 1;
    return plan;
  }

} // namespace filemanager
