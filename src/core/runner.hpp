#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <thread>

#include <spdlog/logger.h>

#include "core/adapter.hpp"
#include "core/errors.hpp"
#include "core/event_sink.hpp"
#include "core/store.hpp"

namespace grid_services {

struct RunnerOptions {
  std::string workerId = "grid-services";
  // Used when neither the connection nor the program sets one.
  std::chrono::milliseconds defaultPollInterval{30'000};
  std::chrono::milliseconds heartbeatInterval{15'000};
  // Retries back off from minBackoff, doubling up to maxBackoff, unless the
  // adapter has its own retryDelay.
  std::chrono::milliseconds minBackoff{5'000};
  std::chrono::milliseconds maxBackoff{300'000};
};

// `interval` lengthened by a random part of up to `jitter` times itself. It is
// never shortened: the VTN's requested interval is a minimum (OpenADR 2.0b
// rule 500).
std::chrono::milliseconds withJitter(std::chrono::milliseconds interval,
                                     double jitter, std::mt19937 &random);

enum class RunnerExit { Running, Stopped, RestartRequested, Failed };

std::string toString(RunnerExit exit);

// Runs one connection on its own thread: claims and executes commands,
// polls the program, and keeps the connection's state and heartbeat
// current. Nothing else touches the connection's adapter or store.
//
// A connection that is disabled or archived is retired instead: the runner
// unregisters it from the program, retrying for as long as the program
// cannot be reached, and stops.
class ConnectionRunner {
public:
  ConnectionRunner(ConnectionConfig config, std::unique_ptr<RunnerStore> store,
                   const AdapterRegistry &adapters, RunnerOptions options);
  // Stops and joins the thread.
  ~ConnectionRunner();

  ConnectionRunner(const ConnectionRunner &) = delete;
  ConnectionRunner &operator=(const ConnectionRunner &) = delete;

  void start();
  void requestStop();
  void join();
  // Wakes the loop early, e.g. when a command arrives.
  void wake();

  bool finished() const { return exit_.load() != RunnerExit::Running; }
  RunnerExit exitReason() const { return exit_.load(); }
  const ConnectionConfig &config() const { return config_; }

private:
  using Actor = std::optional<std::string>;

  void run(std::stop_token stop);
  void setUp();
  void loop(std::stop_token stop);
  void retire(std::stop_token stop);
  void processCommands(std::stop_token stop);
  CommandOutcome execute(const Command &command);
  // `actor` is who asked, or nothing when the program or the worker did.
  CommandOutcome registerNow(const Actor &actor = std::nullopt);
  CommandOutcome unregisterNow(const Actor &actor = std::nullopt);
  CommandOutcome testNow(const Actor &actor);
  CommandOutcome setParticipationNow(Participation participation,
                                     const Command &command);
  CommandOutcome changeOpt(const Command &command, Opt opt);
  void pollNow();
  // Confirms a registration saved before a restart. Returns false, having
  // cleared the registration, when the program no longer has it.
  bool confirmRegistration();
  void forgetRegistration();
  void runDueWork();
  void handleFailure(const AdapterError &error, const char *operation);
  // Counts a failed attempt to reach the program and returns how long to
  // wait before the next.
  std::chrono::milliseconds failedAttempt(const std::string &message);
  void markPermanentError(const std::string &message,
                          const Actor &actor = std::nullopt);
  void waitUntil(std::stop_token stop, Clock::time_point deadline);
  bool polling() const;
  std::chrono::milliseconds pollInterval() const;
  std::chrono::milliseconds backoff();
  void activity(ActivityCategory category, std::string message,
                std::optional<EventRowId> eventId = std::nullopt,
                std::optional<std::string> actor = std::nullopt,
                nlohmann::json detail = nullptr);
  // Connection activity carrying the adapter's latest exchange.
  void exchangeActivity(std::string message,
                        std::optional<std::string> actor = std::nullopt);

  ConnectionConfig config_;
  std::unique_ptr<RunnerStore> store_;
  const AdapterRegistry &adapters_;
  RunnerOptions options_;
  std::shared_ptr<spdlog::logger> log_;

  std::jthread thread_;
  std::mutex wakeMutex_;
  std::condition_variable_any wakeCondition_;
  bool wakeRequested_ = false;
  std::atomic<bool> abort_{false};
  std::atomic<RunnerExit> exit_{RunnerExit::Running};

  // Loop state; only touched on the runner thread.
  std::unique_ptr<Adapter> adapter_;
  std::string adapterError_;
  std::unique_ptr<StoreEventSink> sink_;
  ConnectionState state_;
  bool needsResume_ = false;
  bool restartRequested_ = false;
  int consecutiveFailures_ = 0;
  Clock::time_point nextPoll_{};
  std::mt19937 random_{std::random_device{}()};
};

} // namespace grid_services
