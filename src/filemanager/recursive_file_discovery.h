#pragma once

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <span>
#include <string>

namespace filemanager {

  // One directory and one batch are processed at a time. Completion and result publication use the
  // thread-default GLib context of start(). The batch consumer itself runs on the worker.
  class RecursiveFileDiscovery {
  public:
    struct Candidate {
      std::string name;
      GFileType type;
    };

    using Publish = std::function<void()>;
    using Batch = std::function<Publish(const std::filesystem::path&, std::span<const Candidate>, GCancellable*)>;
    using Flush = std::function<Publish(GCancellable*)>;
    using Complete = std::function<void(std::string error, std::size_t skippedFolders)>;

    RecursiveFileDiscovery() = default;
    ~RecursiveFileDiscovery();
    RecursiveFileDiscovery(const RecursiveFileDiscovery&) = delete;
    RecursiveFileDiscovery& operator=(const RecursiveFileDiscovery&) = delete;

    void start(const std::filesystem::path& root, bool showHidden, Batch batch, Complete complete, Flush flush = {});
    void cancel();

  private:
    struct Scan;
    std::shared_ptr<Scan> m_scan;
  };

} // namespace filemanager
