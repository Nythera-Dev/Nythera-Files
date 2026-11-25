#pragma once

#include "config/schema/diagnostics.h"

#include <filesystem>
#include <string>

namespace filemanager {

  struct ConfigurationProblem {
    nythera::config::schema::SourceOrigin origin;
    std::string message;

    bool operator==(const ConfigurationProblem&) const = default;
  };

  void notifyInvalidConfiguration(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory);
  void notifyConfigurationReloaded();

} // namespace filemanager
