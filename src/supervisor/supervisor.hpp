#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <stop_token>
#include <vector>

#include <spdlog/logger.h>

#include "core/adapter.hpp"
#include "core/runner.hpp"
#include "core/store.hpp"

namespace grid_services {

struct SupervisorOptions {
  // Full reconcile interval, on top of change notifications.
  std::chrono::milliseconds reconcileInterval{30'000};
  // Delay before restarting a connection whose runner failed, or retrying
  // one that could not be started.
  std::chrono::milliseconds restartDelay{5'000};
  std::chrono::milliseconds notificationWait{1000};
  // How long delivered reports are kept, and how often old ones are pruned.
  std::chrono::seconds reportRetention{std::chrono::hours{24 * 30}};
  std::chrono::milliseconds pruneInterval{std::chrono::hours{1}};
};

// Keeps one runner per enabled connection. Starts runners for new or
// re-enabled connections, stops them for disabled, archived or deleted
// ones, restarts them when their configuration version changes or they
// fail, and wakes a runner when a command arrives for it.
//
// A disabled or archived connection that is still registered with the
// program gets a runner too, which unregisters it and stops.
class Supervisor {
public:
  Supervisor(SupervisorStore &store, RunnerStoreFactory runnerStores,
             const AdapterRegistry &adapters, RunnerOptions runnerOptions,
             SupervisorOptions options = {});
  ~Supervisor();

  Supervisor(const Supervisor &) = delete;
  Supervisor &operator=(const Supervisor &) = delete;

  // Runs until stop is requested, then stops every runner.
  void run(std::stop_token stop);

  // One reconcile pass.
  void reconcile();
  void handle(const Notification &notification);
  void stopAll();

  std::vector<ConnectionId> running() const;

private:
  void startRunner(ConnectionId id);
  void stopRunners(const std::vector<ConnectionId> &ids);
  bool anyFinished() const;

  SupervisorStore &store_;
  RunnerStoreFactory runnerStores_;
  const AdapterRegistry &adapters_;
  RunnerOptions runnerOptions_;
  SupervisorOptions options_;
  std::shared_ptr<spdlog::logger> log_;

  std::map<ConnectionId, std::unique_ptr<ConnectionRunner>> runners_;
  std::map<ConnectionId, Clock::time_point> retryAfter_;
  Clock::time_point nextPrune_{};
};

} // namespace grid_services
