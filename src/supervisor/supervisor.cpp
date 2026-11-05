#include "supervisor/supervisor.hpp"

#include <algorithm>
#include <thread>

#include "core/logging.hpp"

namespace grid_services {

Supervisor::Supervisor(SupervisorStore &store, RunnerStoreFactory runnerStores,
                       const AdapterRegistry &adapters,
                       RunnerOptions runnerOptions, SupervisorOptions options)
    : store_(store), runnerStores_(std::move(runnerStores)),
      adapters_(adapters), runnerOptions_(std::move(runnerOptions)),
      options_(options), log_(makeLogger("supervisor")) {}

Supervisor::~Supervisor() { stopAll(); }

void Supervisor::run(std::stop_token stop) {
  log_->info("starting");
  auto nextReconcile = Clock::now();

  while (!stop.stop_requested()) {
    try {
      bool reconcileNow = Clock::now() >= nextReconcile || anyFinished();

      if (!reconcileNow) {
        for (const auto &notification :
             store_.waitForNotifications(options_.notificationWait)) {
          if (notification.channel == Notification::Channel::Config ||
              runners_.count(notification.connectionId) == 0) {
            reconcileNow = true;
          }
          handle(notification);
        }
      }

      if (reconcileNow || anyFinished()) {
        reconcile();
        nextReconcile = Clock::now() + options_.reconcileInterval;
        for (const auto &[id, retryAt] : retryAfter_) {
          nextReconcile = std::min(nextReconcile, retryAt);
        }
      }
    } catch (const std::exception &error) {
      log_->error("supervisor error: {}", error.what());
      std::this_thread::sleep_for(options_.restartDelay);
    }
  }

  log_->info("stopping {} connection(s)", runners_.size());
  stopAll();
}

void Supervisor::handle(const Notification &notification) {
  if (notification.channel != Notification::Channel::Commands) {
    return;
  }
  const auto found = runners_.find(notification.connectionId);
  if (found != runners_.end()) {
    found->second->wake();
  }
}

void Supervisor::reconcile() {
  const auto connections = store_.listConnections();
  const int failed = store_.failCommandsForDisabledConnections();
  if (failed > 0) {
    log_->info("failed {} command(s) for disabled connections", failed);
  }
  const int expired = store_.failExpiredCommands();
  if (expired > 0) {
    log_->info("failed {} expired command(s)", expired);
  }
  if (Clock::now() >= nextPrune_) {
    nextPrune_ = Clock::now() + options_.pruneInterval;
    const int pruned = store_.pruneReportHistory(options_.reportRetention);
    if (pruned > 0) {
      log_->info("pruned {} report deliveries older than {}h", pruned,
                 std::chrono::duration_cast<std::chrono::hours>(
                     options_.reportRetention)
                     .count());
    }
  }

  std::map<ConnectionId, ConnectionSummary> desired;
  for (const auto &connection : connections) {
    // A connection that is no longer to run still has to be unregistered.
    if (isActive(connection) || connection.registered) {
      desired[connection.id] = connection;
    }
  }

  std::vector<ConnectionId> toStop;
  for (const auto &[id, runner] : runners_) {
    const auto wanted = desired.find(id);
    if (wanted == desired.end()) {
      log_->info("connection {} disabled, archived or removed; stopping", id);
      toStop.push_back(id);
    } else if (wanted->second.configVersion != runner->config().configVersion) {
      log_->info("connection {} configuration changed ({} -> {}); restarting",
                 id, runner->config().configVersion,
                 wanted->second.configVersion);
      toStop.push_back(id);
      retryAfter_.erase(id);
    } else if (runner->finished()) {
      const auto exit = runner->exitReason();
      log_->info("connection {} runner exited ({})", id, toString(exit));
      toStop.push_back(id);
      if (exit == RunnerExit::Failed) {
        retryAfter_[id] = Clock::now() + options_.restartDelay;
      } else {
        retryAfter_.erase(id);
      }
    }
  }
  stopRunners(toStop);

  for (const auto &[id, summary] : desired) {
    if (runners_.count(id) > 0) {
      continue;
    }
    const auto retry = retryAfter_.find(id);
    if (retry != retryAfter_.end() && Clock::now() < retry->second) {
      continue;
    }
    startRunner(id);
  }

  for (auto retry = retryAfter_.begin(); retry != retryAfter_.end();) {
    retry = desired.count(retry->first) == 0 ? retryAfter_.erase(retry)
                                             : std::next(retry);
  }
}

void Supervisor::startRunner(ConnectionId id) {
  try {
    auto store = runnerStores_(id);
    if (!store->tryLock()) {
      log_->warn("connection {} is held by another worker", id);
      retryAfter_[id] = Clock::now() + options_.reconcileInterval;
      return;
    }

    auto config = store->loadConfig();
    if (!isActive(config)) {
      log_->info("connection {} is {}; unregistering it if it is registered",
                 id, config.archived ? "archived" : "disabled");
    }

    auto runner = std::make_unique<ConnectionRunner>(
        std::move(config), std::move(store), adapters_, runnerOptions_);
    runner->start();
    runners_[id] = std::move(runner);
    retryAfter_.erase(id);
  } catch (const std::exception &error) {
    log_->error("could not start connection {}: {}", id, error.what());
    retryAfter_[id] = Clock::now() + options_.restartDelay;
  }
}

void Supervisor::stopRunners(const std::vector<ConnectionId> &ids) {
  for (const auto id : ids) {
    runners_.at(id)->requestStop();
  }
  for (const auto id : ids) {
    runners_.at(id)->join();
    runners_.erase(id);
  }
}

void Supervisor::stopAll() {
  std::vector<ConnectionId> ids;
  for (const auto &[id, runner] : runners_) {
    ids.push_back(id);
  }
  stopRunners(ids);
}

bool Supervisor::anyFinished() const {
  for (const auto &[id, runner] : runners_) {
    if (runner->finished()) {
      return true;
    }
  }
  return false;
}

std::vector<ConnectionId> Supervisor::running() const {
  std::vector<ConnectionId> ids;
  for (const auto &[id, runner] : runners_) {
    ids.push_back(id);
  }
  return ids;
}

} // namespace grid_services
