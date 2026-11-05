#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

namespace grid_services {

// A named logger that writes through the default logger's sinks.
inline std::shared_ptr<spdlog::logger> makeLogger(const std::string &name) {
  const auto base = spdlog::default_logger();
  auto logger = std::make_shared<spdlog::logger>(name, base->sinks().begin(),
                                                 base->sinks().end());
  logger->set_level(base->level());
  return logger;
}

} // namespace grid_services
