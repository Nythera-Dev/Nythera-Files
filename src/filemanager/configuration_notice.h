#pragma once

#include "config/config_monitor.h"

#include <filesystem>

namespace filemanager {

  using ConfigurationProblem = nythera::config::ConfigurationProblem;

  void notifyInvalidConfiguration(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory);
  void notifyConfigurationReloaded();

} // namespace filemanager
