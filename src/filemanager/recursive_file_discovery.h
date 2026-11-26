#pragma once

#include "filemanager/search_scope.h"

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <span>
#include <string>

namespace filemanager {

  // Sources traverse independently. Each source processes one batch at a time.
  // Publication and completion run on the thread-default GLib context of start().
  class RecursiveFileDiscovery {
  public:
    struct Candidate {
      std::string name;
      GFileType type;
    };

    struct Source {
      const std::filesystem::path& root;
      int rootFd;
      int folderFd;
      [[nodiscard]] int openFolder(const std::filesystem::path& parent) const;
    };

    using Publish = std::function<void()>;
    using Batch =
        std::function<Publish(const std::filesystem::path&, std::span<const Candidate>, const Source&, GCancellable*)>;
    using Flush = std::function<Publish(const Source&, GCancellable*)>;
    struct Consumer {
      Batch batch;
      Flush flush;
    };
    using MakeConsumer = std::function<Consumer()>;
    using Complete = std::function<void(std::string error, std::size_t skippedFolders)>;

    RecursiveFileDiscovery() = default;
    ~RecursiveFileDiscovery();
    RecursiveFileDiscovery(const RecursiveFileDiscovery&) = delete;
    RecursiveFileDiscovery& operator=(const RecursiveFileDiscovery&) = delete;

    void start(const std::filesystem::path& root, bool showHidden, Batch batch, Complete complete, Flush flush = {});
    void startPlan(std::shared_ptr<const SearchPlan> plan, bool showHidden, MakeConsumer consumer, Complete complete);
    void cancel();

  private:
    struct Run;
    struct Scan;
    std::shared_ptr<Run> m_run;
  };

} // namespace filemanager
