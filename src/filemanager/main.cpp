#include "core/files/resource_paths.h"
#include "filemanager/file_manager.h"
#include "filemanager/preview/thumbnail_provider.h"
#include "filemanager/startup_path.h"
#include "net/http_client.h"

#include <exception>
#include <print>
#include <string_view>

int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string_view(argv[1]) == "--internal-preview-image")
      return filemanager::preview::runImageWorker(argv[2], argv[3]);

    FileManager::StartupOptions options;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--new-window") {
        options.forceNewWindow = true;
      } else if (!options.explicitPath) {
        options.explicitPath = std::filesystem::path(filemanager::startupPath(arg));
      } else {
        throw std::runtime_error("Expected at most one local directory path or file URI");
      }
    }

    paths::setInstallPaths(NYTHERA_INSTALL_PREFIX, NYTHERA_INSTALL_DATADIR);
    HttpClient::setUserAgent("nythera/" NYTHERA_VERSION);
    paths::selectFilesAssets();
    if (options.explicitPath && !options.forceNewWindow) {
      const ConfigService shellConfig(ConfigService::SettingsAccess::InheritanceOnly);
      const filemanager::FileManagerSettings settings(shellConfig);
      if (settings.externalOpenMode() == filemanager::ExternalOpenMode::NewTab
          && filemanager::InstanceCoordinator::tryForwardOpenTab(*options.explicitPath))
        return 0;
    }
    FileManager manager(std::move(options));
    return manager.run();
  } catch (const std::exception& error) {
    std::println(stderr, "nythera-file-manager: {}", error.what());
    return 1;
  }
}
