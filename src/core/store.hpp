#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/telemetry.hpp"
#include "core/types.hpp"

namespace grid_services {

// Database access for one connection. Each runner owns one, on its own
// database session, and uses it only from its own thread.
//
// This covers what every protocol shares. A protocol's own settings and
// tables are behind an interface the protocol defines, which a store
// implements beside this one; see protocols/openadr2b/store.hpp.
class RunnerStore {
public:
  virtual ~RunnerStore() = default;

  virtual ConnectionId connectionId() const = 0;

  // Takes the connection's lock for this session. Returns false when
  // another session holds it. The lock is released when the session ends.
  virtual bool tryLock() = 0;

  virtual ConnectionConfig loadConfig() = 0;
  // The client certificate the connection authenticates with, if it has
  // one.
  virtual std::optional<ClientCredential> loadClientCredential() = 0;

  // Creates the state row with defaults if it does not exist yet.
  virtual ConnectionState loadState() = 0;
  // `configError` says why the configuration cannot be used, if it cannot.
  virtual void markStarted(int configVersion, const std::string &workerId,
                           const std::optional<std::string> &configError) = 0;
  virtual void setRegistration(Registration registration) = 0;
  virtual void setRegistered(const RegistrationInfo &info) = 0;
  // The program's poll interval, as a poll since registering found it.
  virtual void setProgramPollInterval(int seconds) = 0;
  virtual void clearRegistration() = 0;
  virtual void setParticipation(Participation participation) = 0;
  // Records a successful exchange with the program, and clears the error
  // and the failures.
  virtual void recordPoll(Timestamp at) = 0;
  virtual void recordError(const std::string &message, Timestamp at) = 0;
  // Records how many attempts to reach the program have failed in a row,
  // and when the next one is due.
  virtual void recordFailures(int consecutiveFailures,
                              std::optional<Timestamp> nextRetryAt) = 0;
  // Records how far the program's clock is ahead of ours, as read at `at`.
  virtual void recordClockOffset(int offsetSeconds, Timestamp at) = 0;
  virtual void heartbeat(Timestamp at) = 0;

  // Fails commands left running by a worker that stopped mid-command.
  virtual int failInterruptedCommands(const std::string &message) = 0;
  // Leaves expired commands for the supervisor to fail.
  virtual std::optional<Command> claimNextCommand() = 0;
  virtual void completeCommand(CommandId id,
                               const CommandOutcome &outcome) = 0;

  virtual std::optional<KnownEvent>
  findProgramEvent(const std::string &programEventId) = 0;
  // Only finds program events belonging to this connection.
  virtual std::optional<KnownEvent> findEventById(EventRowId id) = 0;
  virtual std::vector<KnownEvent> openProgramEvents() = 0;
  // Inserts or updates by program event id. The opt columns are only
  // written on insert.
  virtual KnownEvent upsertProgramEvent(const NormalizedEvent &event, Opt opt,
                                        OptSource optSource) = 0;
  virtual void setProgramStatus(EventRowId id, ProgramStatus status) = 0;
  virtual void setOpt(EventRowId id, Opt opt, OptSource optSource) = 0;
  virtual void recordResponse(EventRowId id, EventAnswer answer,
                              int modificationNumber, bool ok,
                              const std::string &message) = 0;

  virtual void appendActivity(const ActivityEntry &entry) = 0;

  // What the site measures. Throws std::invalid_argument for a point the
  // worker cannot read.
  virtual std::vector<TelemetryPoint> loadTelemetryPoints() = 0;
  virtual std::optional<DataCredential> loadDataCredential() = 0;
  // Records a report sent to the program, or attempted. A delivery for a
  // program's request is recorded against the latest request with its id.
  virtual void recordReportDelivery(const ReportDelivery &delivery) = 0;
};

using RunnerStoreFactory =
    std::function<std::unique_ptr<RunnerStore>(ConnectionId)>;

struct Notification {
  enum class Channel { Config, Commands };
  Channel channel = Channel::Config;
  ConnectionId connectionId = 0;
};

// Database access for the supervisor, on its own session.
class SupervisorStore {
public:
  virtual ~SupervisorStore() = default;

  virtual std::vector<ConnectionSummary> listConnections() = 0;

  // Fails pending commands for disabled and archived connections, which no
  // runner will pick up. Returns how many were failed.
  virtual int failCommandsForDisabledConnections() = 0;

  // Fails pending commands that have expired, so that nothing waits on
  // them. Returns how many were failed.
  virtual int failExpiredCommands() = 0;

  // Deletes report deliveries older than `retention`, and ended report
  // requests that have no deliveries left. Returns how many deliveries went.
  virtual int pruneReportHistory(std::chrono::seconds retention) = 0;

  // Blocks for up to `timeout` waiting for change notifications.
  virtual std::vector<Notification>
  waitForNotifications(std::chrono::milliseconds timeout) = 0;
};

} // namespace grid_services
