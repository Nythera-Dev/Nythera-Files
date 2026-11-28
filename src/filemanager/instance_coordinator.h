#pragma once

#include "app/poll_source.h"

#include <filesystem>
#include <functional>
#include <string>

namespace filemanager {

  class InstanceCoordinator final : public PollSource {
  public:
    using OpenDirectoryCallback = std::function<bool(const std::filesystem::path& path)>;

    InstanceCoordinator() = default;
    ~InstanceCoordinator() override;

    InstanceCoordinator(const InstanceCoordinator&) = delete;
    InstanceCoordinator& operator=(const InstanceCoordinator&) = delete;

    [[nodiscard]] bool start(std::filesystem::path socketPath, OpenDirectoryCallback onOpenDirectory);
    void stop();

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

    [[nodiscard]] const std::filesystem::path& socketPath() const noexcept { return m_socketPath; }
    [[nodiscard]] bool listening() const noexcept { return m_listenFd >= 0; }

    [[nodiscard]] static std::filesystem::path defaultSocketPath();
    [[nodiscard]] static bool
    tryForwardOpenTab(const std::filesystem::path& directory, const std::filesystem::path& socketPath = {});

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    void acceptPending();

    int m_listenFd = -1;
    int m_lockFd = -1;
    std::filesystem::path m_socketPath;
    OpenDirectoryCallback m_onOpenDirectory;
  };

} // namespace filemanager
