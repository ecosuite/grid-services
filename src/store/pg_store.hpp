#pragma once

#include <memory>
#include <string>

#include "core/store.hpp"
#include "protocols/csipaus/store.hpp"
#include "protocols/openadr2b/store.hpp"

#include <pqxx/connection>

namespace grid_services {

// A connection's tables, for every protocol the worker speaks. The SQL for a
// protocol's own tables is in its own file, such as pg_openadr2b_store.cpp.
class PgRunnerStore : public RunnerStore,
                      public openadr2b::Openadr2bStore,
                      public csipaus::CsipausStore {
public:
  PgRunnerStore(const std::string &connectionString, ConnectionId id);
  ~PgRunnerStore() override;

  ConnectionId connectionId() const override { return id_; }
  bool tryLock() override;
  ConnectionConfig loadConfig() override;
  std::optional<ClientCredential> loadClientCredential() override;
  ConnectionState loadState() override;
  void markStarted(int configVersion, const std::string &workerId,
                   const std::optional<std::string> &configError) override;
  void setRegistration(Registration registration) override;
  void setRegistered(const RegistrationInfo &info) override;
  void setProgramPollInterval(int seconds) override;
  void clearRegistration() override;
  void setParticipation(Participation participation) override;
  void recordPoll(Timestamp at) override;
  void recordError(const std::string &message, Timestamp at) override;
  void recordFailures(int consecutiveFailures,
                      std::optional<Timestamp> nextRetryAt) override;
  void recordClockOffset(int offsetSeconds, Timestamp at) override;
  void heartbeat(Timestamp at) override;
  int failInterruptedCommands(const std::string &message) override;
  std::optional<Command> claimNextCommand() override;
  void completeCommand(CommandId id, const CommandOutcome &outcome) override;
  std::optional<KnownEvent>
  findProgramEvent(const std::string &programEventId) override;
  std::optional<KnownEvent> findEventById(EventRowId id) override;
  std::vector<KnownEvent> openProgramEvents() override;
  KnownEvent upsertProgramEvent(const NormalizedEvent &event, Opt opt,
                                OptSource optSource) override;
  void setProgramStatus(EventRowId id, ProgramStatus status) override;
  void setOpt(EventRowId id, Opt opt, OptSource optSource) override;
  void recordResponse(EventRowId id, EventAnswer answer, int modificationNumber, bool ok,
                      const std::string &message) override;
  void appendActivity(const ActivityEntry &entry) override;
  std::vector<TelemetryPoint> loadTelemetryPoints() override;
  std::optional<DataCredential> loadDataCredential() override;
  void recordReportDelivery(const ReportDelivery &delivery) override;

  // openadr2b::Openadr2bStore
  openadr2b::Openadr2bSettings loadOpenadr2bSettings() override;
  std::vector<openadr2b::ReportOfferRow> loadReportOffers() override;
  void recordReportRequest(const openadr2b::ReportRequest &request) override;
  void endReportRequest(const std::string &programRequestId,
                        openadr2b::ReportRequestStatus status) override;
  int endAllReportRequests(openadr2b::ReportRequestStatus status) override;

  // csipaus::CsipausStore
  csipaus::CsipausSettings loadCsipausSettings() override;
  void recordEventDetail(EventRowId eventId,
                         const csipaus::CsipausEventDetail &detail) override;
  void replaceLimits(const std::vector<csipaus::LimitSegment> &limits) override;
  std::vector<csipaus::ProgressDue> progressDue() override;
  std::optional<csipaus::DerDescription> loadDer() override;
  std::vector<csipaus::ReadingRow> loadReadings() override;

private:
  ConnectionId id_;
  std::unique_ptr<pqxx::connection> connection_;
};

class PgSupervisorStore : public SupervisorStore {
public:
  explicit PgSupervisorStore(std::string connectionString);
  ~PgSupervisorStore() override;

  std::vector<ConnectionSummary> listConnections() override;
  int failCommandsForDisabledConnections() override;
  int failExpiredCommands() override;
  int pruneReportHistory(std::chrono::seconds retention) override;
  std::vector<Notification>
  waitForNotifications(std::chrono::milliseconds timeout) override;

private:
  class Listener;

  // Connects and subscribes on first use, and again after a lost connection.
  pqxx::connection &connection();
  void reset();

  std::string connectionString_;
  std::unique_ptr<pqxx::connection> connection_;
  std::vector<std::unique_ptr<Listener>> listeners_;
  std::vector<Notification> pending_;
};

// Advisory lock namespace for connection locks: pg_try_advisory_lock(key, id).
constexpr int connectionLockKey = 0x76766e00;

} // namespace grid_services
