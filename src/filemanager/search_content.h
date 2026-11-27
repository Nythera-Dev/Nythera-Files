#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/search_filters.h"

#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace filemanager {

  struct ContentBackends {
    std::string rg;
    std::string rga;
    [[nodiscard]] bool supports(ContentMode mode) const;
  };

  [[nodiscard]] ContentBackends findContentBackends();
  [[nodiscard]] std::filesystem::path searchExtractionCache();

  enum class ContentLane { Plain, LargePlain, Extraction };

  struct ContentIssues {
    std::size_t unsupported = 0;
    std::size_t sizeRestricted = 0;
    std::size_t inaccessible = 0;
    std::size_t extractionFailed = 0;
    std::size_t archiveTruncated = 0;
    std::size_t extractionTimeouts = 0;
    std::size_t plainTimeouts = 0;
    bool backendError = false;

    void merge(const ContentIssues& other);
    [[nodiscard]] bool partial() const;
  };

  // An O_PATH descriptor pins a scope-checked regular file. Backends never receive
  // paths originally supplied by discovery, which could be replaced while scanning.
  struct ContentCandidate {
    FileEntry entry;
    int fd = -1;

    ContentCandidate(FileEntry entry, int fd);
    ~ContentCandidate();
    ContentCandidate(ContentCandidate&& other) noexcept;
    ContentCandidate& operator=(ContentCandidate&&) = delete;
  };

  struct ContentResult {
    std::vector<FileEntry> entries;
    ContentIssues issues;
  };

  class ContentSearch {
  public:
    explicit ContentSearch(ContentFilter filter, ContentBackends backends = findContentBackends());
    [[nodiscard]] ContentLane lane(const FileEntry& entry, GCancellable* cancel);
    [[nodiscard]] ContentResult match(std::span<ContentCandidate> candidates, GCancellable* cancel);
    [[nodiscard]] static std::future<ContentResult>
    queueExtraction(const std::shared_ptr<ContentSearch>& search, ContentCandidate candidate, GCancellable* cancel);
    [[nodiscard]] std::string notice() const;

  private:
    struct Adapter {
      std::string name;
      bool enabled = false;
      bool available = false;
    };

    void prepare(GCancellable* cancel);
    [[nodiscard]] const Adapter* adapterFor(const FileEntry& entry) const;

    ContentFilter m_filter;
    ContentBackends m_backends;
    std::filesystem::path m_cache;
    std::once_flag m_prepare;
    std::unordered_map<std::string, Adapter> m_adapters;
    std::vector<std::pair<std::string, std::string>> m_missing;
    std::string m_invalid;
    std::string m_preprocessor;
    bool m_rgaReady = false;
  };

} // namespace filemanager
