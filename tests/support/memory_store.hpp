#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/store.hpp"
#include "protocols/csipaus/store.hpp"
#include "protocols/openadr2b/store.hpp"

namespace grid_services::testing {

// In-memory stand-in for the database, shared by the stores a test creates.
struct MemoryDatabase {
  struct StoredCommand {
    ConnectionId connectionId = 0;
    Command command;
    std::string status = "pending";
    std::string message;
    nlohmann::json result = nullptr;
    bool expired = false;
  };
  struct StoredEvent {
    ConnectionId connectionId = 0;
    KnownEvent event;
    NormalizedEvent latest;
    // Written by the planner; tests set it with setLifecycle.
    std::string lifecycle = "received";
  };
  struct StoredResponse {
    EventRowId eventId = 0;
    EventAnswer answer = EventAnswer::OptIn;
    int modificationNumber = 0;
    bool ok = false;
    std::string message;
  };
  struct StoredActivity {
    ConnectionId connectionId = 0;
    ActivityEntry entry;
  };
  struct StoredReportRequest {
    ConnectionId connectionId = 0;
    openadr2b::ReportRequest request;
    openadr2b::ReportRequestStatus status =
        openadr2b::ReportRequestStatus::Active;
  };
  struct StoredReportDelivery {
    ConnectionId connectionId = 0;
    ReportDelivery delivery;
  };
  struct StateExtras {
    std::optional<std::string> lastError;
    std::optional<std::string> configError;
    int consecutiveFailures = 0;
    std::optional<int> clockOffsetSeconds;
    std::optional<Timestamp> nextRetryAt;
    int heartbeats = 0;
    int polls = 0;
    int appliedConfigVersion = 0;
    std::string workerId;
  };

  mutable std::mutex mutex;
  std::map<ConnectionId, ConnectionConfig> connections;
  std::map<ConnectionId, openadr2b::Openadr2bSettings> openadr2b;
  std::map<ConnectionId, csipaus::CsipausSettings> csipaus;
  std::map<EventRowId, csipaus::CsipausEventDetail> csipausEvents;
  std::map<ConnectionId, std::vector<csipaus::LimitSegment>> csipausLimits;
  std::map<ConnectionId, csipaus::DerDescription> csipausDer;
  std::map<ConnectionId, std::vector<csipaus::ReadingRow>> csipausReadings;
  std::map<ConnectionId, ClientCredential> clientCredentials;
  std::map<ConnectionId, ConnectionState> states;
  std::map<ConnectionId, StateExtras> extras;
  std::vector<StoredCommand> commands;
  std::vector<StoredEvent> events;
  std::vector<StoredResponse> responses;
  std::vector<StoredActivity> activity;
  std::map<ConnectionId, std::vector<TelemetryPoint>> telemetryPoints;
  std::map<ConnectionId, std::vector<openadr2b::ReportOfferRow>> reportOffers;
  std::map<ConnectionId, DataCredential> dataCredentials;
  std::vector<StoredReportRequest> reportRequests;
  std::vector<StoredReportDelivery> reportDeliveries;
  std::set<ConnectionId> locks;
  std::vector<Notification> notifications;

  // Test helpers. They take the lock themselves.
  ConnectionId addConnection(ConnectionConfig config);
  void updateConnection(ConnectionId id,
                        const std::function<void(ConnectionConfig &)> &change);
  CommandId addCommand(ConnectionId connectionId, CommandKind kind,
                       std::optional<EventRowId> eventId = std::nullopt,
                       std::optional<std::string> requestedBy = std::nullopt);
  EventRowId addEvent(ConnectionId connectionId, const NormalizedEvent &event,
                      Opt opt = Opt::In,
                      OptSource optSource = OptSource::Default);
  void setLifecycle(EventRowId id, const std::string &lifecycle);
  StoredCommand command(CommandId id) const;
  std::optional<KnownEvent> event(EventRowId id) const;
  ConnectionState state(ConnectionId id) const;
  StateExtras stateExtras(ConnectionId id) const;
  std::vector<std::string> activityMessages(ConnectionId id) const;
  // The first entry whose message contains `text`.
  std::optional<ActivityEntry> activityEntry(ConnectionId id,
                                             const std::string &text) const;
  std::vector<StoredResponse> responsesFor(EventRowId id) const;
  std::vector<StoredReportRequest> reportRequestsFor(ConnectionId id) const;
  std::vector<ReportDelivery> reportDeliveriesFor(ConnectionId id) const;
  void notify(Notification notification);

  // Polls `predicate` (called with the lock held) until it holds or the
  // timeout passes.
  bool waitFor(const std::function<bool(const MemoryDatabase &)> &predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;

  // Unlocked helpers for the stores.
  KnownEvent withResponses(const StoredEvent &stored) const;
};

class MemoryRunnerStore : public RunnerStore,
                          public openadr2b::Openadr2bStore,
                          public csipaus::CsipausStore {
public:
  MemoryRunnerStore(std::shared_ptr<MemoryDatabase> db, ConnectionId id);
  ~MemoryRunnerStore() override;

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
  MemoryDatabase::StoredEvent *findStored(EventRowId id);

  std::shared_ptr<MemoryDatabase> db_;
  ConnectionId id_;
  bool locked_ = false;
};

class MemorySupervisorStore : public SupervisorStore {
public:
  explicit MemorySupervisorStore(std::shared_ptr<MemoryDatabase> db);

  std::vector<ConnectionSummary> listConnections() override;
  int failCommandsForDisabledConnections() override;
  int failExpiredCommands() override;
  int pruneReportHistory(std::chrono::seconds retention) override;
  std::vector<Notification>
  waitForNotifications(std::chrono::milliseconds timeout) override;

  int pruneCalls = 0;

private:
  std::shared_ptr<MemoryDatabase> db_;
};

RunnerStoreFactory memoryRunnerStores(std::shared_ptr<MemoryDatabase> db);

} // namespace grid_services::testing
