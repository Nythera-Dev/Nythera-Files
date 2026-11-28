#include "filemanager/path_resolution_cache.h"

#include "filemanager/place_metadata.h"

#include <utility>

namespace filemanager {

  namespace {
    std::filesystem::path normalizeDirectoryPath(const std::filesystem::path& path) {
      auto normalized = path.lexically_normal();
      if (normalized.filename().empty() && normalized.has_parent_path() && normalized != normalized.root_path())
        normalized = normalized.parent_path();
      return normalized;
    }
  } // namespace

  PathResolutionCache::~PathResolutionCache() {
    *m_alive = false;
    cancel();
  }

  void PathResolutionCache::cancel() {
    ++m_generation;
    if (m_cancellable != nullptr) {
      g_cancellable_cancel(m_cancellable);
      g_clear_object(&m_cancellable);
    }
    m_pending.clear();
  }

  std::optional<PlacePathResolution> PathResolutionCache::findCached(const std::filesystem::path& path) const {
    const auto it = m_cache.find(normalizeDirectoryPath(path).string());
    if (it == m_cache.end())
      return std::nullopt;
    return it->second;
  }

  bool PathResolutionCache::samePersonalPlace(
      const std::filesystem::path& first, const std::filesystem::path& second
  ) const {
    const auto normFirst = first.lexically_normal();
    const auto normSecond = second.lexically_normal();
    if (normFirst == normSecond)
      return true;
    const auto firstIt = m_cache.find(normFirst.string());
    const auto secondIt = m_cache.find(normSecond.string());
    const auto& firstTarget =
        firstIt != m_cache.end() && !firstIt->second.canonicalPath.empty() ? firstIt->second.canonicalPath : normFirst;
    const auto& secondTarget = secondIt != m_cache.end() && !secondIt->second.canonicalPath.empty()
        ? secondIt->second.canonicalPath
        : normSecond;
    return firstTarget == secondTarget;
  }

  std::optional<bool> PathResolutionCache::sameDirectoryPathCached(
      const std::filesystem::path& first, const std::filesystem::path& second
  ) const {
    const auto normFirst = normalizeDirectoryPath(first);
    const auto normSecond = normalizeDirectoryPath(second);
    if (normFirst == normSecond)
      return true;
    const auto firstIt = m_cache.find(normFirst.string());
    const auto secondIt = m_cache.find(normSecond.string());
    if (firstIt == m_cache.end() || secondIt == m_cache.end())
      return std::nullopt;
    if (!firstIt->second.exists
        || !secondIt->second.exists
        || firstIt->second.canonicalPath.empty()
        || secondIt->second.canonicalPath.empty())
      return false;
    return firstIt->second.canonicalPath == secondIt->second.canonicalPath;
  }

  void PathResolutionCache::compareDirectoriesAsync(
      const std::filesystem::path& first, const std::filesystem::path& second, std::function<void(bool)> onResolved
  ) {
    if (const auto cached = sameDirectoryPathCached(first, second)) {
      if (onResolved)
        onResolved(*cached);
      return;
    }
    const auto normFirst = normalizeDirectoryPath(first);
    const auto normSecond = normalizeDirectoryPath(second);
    struct CompareState {
      std::size_t remaining = 2;
      std::function<void(bool)> done;
    };
    auto state = std::make_shared<CompareState>(CompareState{.remaining = 2, .done = std::move(onResolved)});
    auto step = [this, alive = std::weak_ptr<bool>(m_alive), normFirst, normSecond, state](const PlacePathResolution&) {
      const auto locked = alive.lock();
      if (!locked || !*locked)
        return;
      if (--state->remaining > 0)
        return;
      if (state->done)
        state->done(sameDirectoryPathCached(normFirst, normSecond).value_or(false));
    };
    resolveAsync(normFirst, step);
    resolveAsync(normSecond, step);
  }

  void PathResolutionCache::cacheResolved(const std::filesystem::path& path, const PlacePathResolution& resolved) {
    m_cache[normalizeDirectoryPath(path).string()] = resolved;
  }

  void PathResolutionCache::resolveAsync(
      const std::filesystem::path& path, std::function<void(const PlacePathResolution&)> onResolved
  ) {
    if (path.empty() || path.string().starts_with("trash:")) {
      if (onResolved)
        onResolved(PlacePathResolution{});
      return;
    }
    const auto normalized = normalizeDirectoryPath(path);
    const std::string key = normalized.string();
    if (const auto cached = m_cache.find(key); cached != m_cache.end()) {
      if (onResolved)
        onResolved(cached->second);
      return;
    }
    if (!onResolved && m_pending.contains(key))
      return;
    if (!onResolved)
      m_pending.insert(key);

    const auto generation = m_generation;
    std::weak_ptr<bool> alive = m_alive;
    auto finishOnMain = [this, alive, generation, key,
                         onResolved = std::move(onResolved)](PlacePathResolution resolved) {
      const auto locked = alive.lock();
      if (!locked || !*locked || m_generation != generation)
        return;
      m_pending.erase(key);
      m_cache[key] = resolved;
      if (onResolved)
        onResolved(resolved);
    };

    if (m_resolveHook) {
      if (m_resolveHook(normalized, std::move(finishOnMain)))
        return;
    }

    if (m_cancellable == nullptr)
      m_cancellable = g_cancellable_new();
    struct ResolveRequest {
      std::filesystem::path path;
      std::function<void(PlacePathResolution)> done;
    };
    GTask* task = g_task_new(
        nullptr, m_cancellable,
        [](GObject*, GAsyncResult* result, gpointer userData) {
          std::unique_ptr<ResolveRequest> req(static_cast<ResolveRequest*>(userData));
          GError* error = nullptr;
          auto* resolved = static_cast<PlacePathResolution*>(g_task_propagate_pointer(G_TASK(result), &error));
          const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
          g_clear_error(&error);
          if (resolved != nullptr) {
            PlacePathResolution value = std::move(*resolved);
            delete resolved;
            if (!cancelled && req->done)
              req->done(std::move(value));
          }
        },
        new ResolveRequest{normalized, std::move(finishOnMain)}
    );
    auto* workerPath = new std::filesystem::path(normalized);
    g_task_set_task_data(task, workerPath, [](gpointer p) { delete static_cast<std::filesystem::path*>(p); });
    g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer taskData, GCancellable* cancellable) {
      if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(worker, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        return;
      }
      const auto& targetPath = *static_cast<const std::filesystem::path*>(taskData);
      PlacePathResolution res;
      std::error_code ec;
      const auto canonical = std::filesystem::canonical(targetPath, ec);
      if (!ec) {
        res.exists = true;
        res.canonicalPath = canonical;
        std::error_code dirEc;
        res.isDirectory = std::filesystem::is_directory(canonical, dirEc) && !dirEc;
      }
      g_task_return_pointer(worker, new PlacePathResolution(std::move(res)), [](gpointer p) {
        delete static_cast<PlacePathResolution*>(p);
      });
    });
    g_object_unref(task);
  }

} // namespace filemanager
