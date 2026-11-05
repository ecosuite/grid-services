#include "support/memory_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace grid_services::testing {

// ---------------------------------------------------------------------------
// MemoryDatabase

ConnectionId MemoryDatabase::addConnection(ConnectionConfig config) {
  std::lock_guard lock(mutex);
  const ConnectionId id =
      connections.empty() ? 1 : connections.rbegin()->first + 1;
  config.id = id;
  if (config.configVersion == 0) {
    config.configVersion = 1;
  }
  if (config.protocol.empty()) {
    config.protocol = "fake";
  }
  if (config.siteCode.empty()) {
    config.siteCode = "SITE-" + std::to_string(id);
  }
  connections[id] = config;
  return id;
}

void MemoryDatabase::updateConnection(
    ConnectionId id, const std::function<void(ConnectionConfig &)> &change) {
  std::lock_guard lock(mutex);
  auto &config = connections.at(id);
  change(config);
  ++config.configVersion;
  notifications.push_back({Notification::Channel::Config, id});
}

CommandId MemoryDatabase::addCommand(ConnectionId connectionId,
                                     CommandKind kind,
                                     std::optional<EventRowId> eventId,
                                     std::optional<std::string> requestedBy) {
  std::lock_guard lock(mutex);
  const CommandId id = static_cast<CommandId>(commands.size()) + 1;
  commands.push_back(
      {connectionId, Command{id, kind, eventId, std::move(requestedBy)}});
  notifications.push_back({Notification::Channel::Commands, connectionId});
  return id;
}

EventRowId MemoryDatabase::addEvent(ConnectionId connectionId,
                                    const NormalizedEvent &event, Opt opt,
                                    OptSource optSource) {
  std::lock_guard lock(mutex);
  StoredEvent stored;
  stored.connectionId = connectionId;
  stored.latest = event;
  stored.event.id = static_cast<EventRowId>(events.size()) + 1;
  stored.event.programEventId = event.programEventId;
  stored.event.modificationNumber = event.modificationNumber;
  stored.event.status = event.status;
  stored.event.startAt = event.startAt;
  stored.event.durationSeconds = event.durationSeconds;
  stored.event.responseRequired = event.responseRequired;
  stored.event.declinable = event.declinable;
  stored.event.signals = event.signals;
  stored.event.programRequestId = event.programRequestId;
  stored.event.opt = opt;
  stored.event.optSource = optSource;
  events.push_back(stored);
  return stored.event.id;
}

void MemoryDatabase::setLifecycle(EventRowId id, const std::string &lifecycle) {
  std::lock_guard lock(mutex);
  for (auto &stored : events) {
    if (stored.event.id == id) {
      stored.lifecycle = lifecycle;
    }
  }
}

MemoryDatabase::StoredCommand MemoryDatabase::command(CommandId id) const {
  std::lock_guard lock(mutex);
  return commands.at(static_cast<size_t>(id) - 1);
}

std::optional<KnownEvent> MemoryDatabase::event(EventRowId id) const {
  std::lock_guard lock(mutex);
  for (const auto &stored : events) {
    if (stored.event.id == id) {
      return withResponses(stored);
    }
  }
  return std::nullopt;
}

ConnectionState MemoryDatabase::state(ConnectionId id) const {
  std::lock_guard lock(mutex);
  const auto found = states.find(id);
  return found == states.end() ? ConnectionState{} : found->second;
}

MemoryDatabase::StateExtras MemoryDatabase::stateExtras(ConnectionId id) const {
  std::lock_guard lock(mutex);
  const auto found = extras.find(id);
  return found == extras.end() ? StateExtras{} : found->second;
}

std::vector<std::string>
MemoryDatabase::activityMessages(ConnectionId id) const {
  std::lock_guard lock(mutex);
  std::vector<std::string> messages;
  for (const auto &entry : activity) {
    if (entry.connectionId == id) {
      messages.push_back(entry.entry.message);
    }
  }
  return messages;
}

std::optional<ActivityEntry>
MemoryDatabase::activityEntry(ConnectionId id, const std::string &text) const {
  std::lock_guard lock(mutex);
  for (const auto &entry : activity) {
    if (entry.connectionId == id &&
        entry.entry.message.find(text) != std::string::npos) {
      return entry.entry;
    }
  }
  return std::nullopt;
}

std::vector<MemoryDatabase::StoredResponse>
MemoryDatabase::responsesFor(EventRowId id) const {
  std::lock_guard lock(mutex);
  std::vector<StoredResponse> found;
  for (const auto &response : responses) {
    if (response.eventId == id) {
      found.push_back(response);
    }
  }
  return found;
}

std::vector<MemoryDatabase::StoredReportRequest>
MemoryDatabase::reportRequestsFor(ConnectionId id) const {
  std::lock_guard lock(mutex);
  std::vector<StoredReportRequest> found;
  for (const auto &stored : reportRequests) {
    if (stored.connectionId == id) {
      found.push_back(stored);
    }
  }
  return found;
}

std::vector<ReportDelivery>
MemoryDatabase::reportDeliveriesFor(ConnectionId id) const {
  std::lock_guard lock(mutex);
  std::vector<ReportDelivery> found;
  for (const auto &stored : reportDeliveries) {
    if (stored.connectionId == id) {
      found.push_back(stored.delivery);
    }
  }
  return found;
}

void MemoryDatabase::notify(Notification notification) {
  std::lock_guard lock(mutex);
  notifications.push_back(notification);
}

bool MemoryDatabase::waitFor(
    const std::function<bool(const MemoryDatabase &)> &predicate,
    std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    {
      std::lock_guard lock(mutex);
      if (predicate(*this)) {
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

KnownEvent MemoryDatabase::withResponses(const StoredEvent &stored) const {
  KnownEvent event = stored.event;
  event.respondedModification.reset();
  for (const auto &response : responses) {
    if (response.eventId == event.id && response.ok &&
        (!event.respondedModification ||
         response.modificationNumber > *event.respondedModification)) {
      event.respondedModification = response.modificationNumber;
    }
  }
  return event;
}

// ---------------------------------------------------------------------------
// MemoryRunnerStore

MemoryRunnerStore::MemoryRunnerStore(std::shared_ptr<MemoryDatabase> db,
                                     ConnectionId id)
    : db_(std::move(db)), id_(id) {}

MemoryRunnerStore::~MemoryRunnerStore() {
  if (locked_) {
    std::lock_guard lock(db_->mutex);
    db_->locks.erase(id_);
  }
}

bool MemoryRunnerStore::tryLock() {
  std::lock_guard lock(db_->mutex);
  if (locked_) {
    return true;
  }
  if (!db_->locks.insert(id_).second) {
    return false;
  }
  locked_ = true;
  return true;
}

ConnectionConfig MemoryRunnerStore::loadConfig() {
  std::lock_guard lock(db_->mutex);
  const auto found = db_->connections.find(id_);
  if (found == db_->connections.end()) {
    throw std::runtime_error("connection does not exist");
  }
  return found->second;
}

std::optional<ClientCredential> MemoryRunnerStore::loadClientCredential() {
  std::lock_guard lock(db_->mutex);
  const auto found = db_->clientCredentials.find(id_);
  if (found == db_->clientCredentials.end()) {
    return std::nullopt;
  }
  return found->second;
}

openadr2b::Openadr2bSettings MemoryRunnerStore::loadOpenadr2bSettings() {
  std::lock_guard lock(db_->mutex);
  return db_->openadr2b.at(id_);
}

csipaus::CsipausSettings MemoryRunnerStore::loadCsipausSettings() {
  std::lock_guard lock(db_->mutex);
  return db_->csipaus.at(id_);
}

void MemoryRunnerStore::recordEventDetail(
    EventRowId eventId, const csipaus::CsipausEventDetail &detail) {
  std::lock_guard lock(db_->mutex);
  db_->csipausEvents[eventId] = detail;
}

std::vector<csipaus::ProgressDue> MemoryRunnerStore::progressDue() {
  std::lock_guard lock(db_->mutex);
  const auto told = [&](EventRowId id, EventAnswer answer) {
    for (const auto &response : db_->responses) {
      if (response.eventId == id && response.answer == answer && response.ok) {
        return true;
      }
    }
    return false;
  };
  std::vector<csipaus::ProgressDue> due;
  for (const auto &stored : db_->events) {
    const auto detail = db_->csipausEvents.find(stored.event.id);
    if (stored.connectionId != id_ || detail == db_->csipausEvents.end() ||
        detail->second.kind != csipaus::CsipausEventKind::Control ||
        !detail->second.replyTo ||
        (detail->second.responseRequired & 0x02) == 0 ||
        ((detail->second.responseRequired & 0x01) != 0 &&
         !told(stored.event.id, EventAnswer::Received)) ||
        stored.event.status == ProgramStatus::Cancelled ||
        stored.event.status == ProgramStatus::Superseded ||
        (stored.lifecycle != "control" && stored.lifecycle != "completed")) {
      continue;
    }
    std::vector<EventAnswer> answers;
    if (!told(stored.event.id, EventAnswer::Started)) {
      answers.push_back(EventAnswer::Started);
    }
    if (stored.lifecycle == "completed" &&
        !told(stored.event.id, EventAnswer::Completed)) {
      answers.push_back(EventAnswer::Completed);
    }
    if (!answers.empty()) {
      due.push_back({stored.event, detail->second, std::move(answers)});
    }
  }
  return due;
}

void MemoryRunnerStore::replaceLimits(
    const std::vector<csipaus::LimitSegment> &limits) {
  std::lock_guard lock(db_->mutex);
  db_->csipausLimits[id_] = limits;
}

std::optional<csipaus::DerDescription> MemoryRunnerStore::loadDer() {
  std::lock_guard lock(db_->mutex);
  const auto found = db_->csipausDer.find(id_);
  if (found == db_->csipausDer.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<csipaus::ReadingRow> MemoryRunnerStore::loadReadings() {
  std::lock_guard lock(db_->mutex);
  const auto found = db_->csipausReadings.find(id_);
  return found == db_->csipausReadings.end()
             ? std::vector<csipaus::ReadingRow>{}
             : found->second;
}

ConnectionState MemoryRunnerStore::loadState() {
  std::lock_guard lock(db_->mutex);
  return db_->states[id_];
}

void MemoryRunnerStore::markStarted(
    int configVersion, const std::string &workerId,
    const std::optional<std::string> &configError) {
  std::lock_guard lock(db_->mutex);
  auto &extras = db_->extras[id_];
  extras.appliedConfigVersion = configVersion;
  extras.workerId = workerId;
  extras.configError = configError;
  extras.consecutiveFailures = 0;
  extras.nextRetryAt.reset();
}

void MemoryRunnerStore::setRegistration(Registration registration) {
  std::lock_guard lock(db_->mutex);
  db_->states[id_].registration = registration;
}

void MemoryRunnerStore::setRegistered(const RegistrationInfo &info) {
  std::lock_guard lock(db_->mutex);
  auto &state = db_->states[id_];
  state.registration = Registration::Registered;
  state.clientId = info.clientId;
  state.registrationId = info.registrationId;
  state.programPollIntervalSeconds = info.pollIntervalSeconds;
}

void MemoryRunnerStore::setProgramPollInterval(int seconds) {
  std::lock_guard lock(db_->mutex);
  db_->states[id_].programPollIntervalSeconds = seconds;
}

void MemoryRunnerStore::clearRegistration() {
  std::lock_guard lock(db_->mutex);
  auto &state = db_->states[id_];
  state.registration = Registration::Unregistered;
  state.registrationId.reset();
  state.programPollIntervalSeconds.reset();
}

void MemoryRunnerStore::setParticipation(Participation participation) {
  std::lock_guard lock(db_->mutex);
  db_->states[id_].participation = participation;
}

void MemoryRunnerStore::recordPoll(Timestamp) {
  std::lock_guard lock(db_->mutex);
  auto &extras = db_->extras[id_];
  ++extras.polls;
  extras.lastError.reset();
  extras.consecutiveFailures = 0;
  extras.nextRetryAt.reset();
}

void MemoryRunnerStore::recordError(const std::string &message, Timestamp) {
  std::lock_guard lock(db_->mutex);
  db_->extras[id_].lastError = message;
}

void MemoryRunnerStore::recordFailures(int consecutiveFailures,
                                       std::optional<Timestamp> nextRetryAt) {
  std::lock_guard lock(db_->mutex);
  db_->extras[id_].consecutiveFailures = consecutiveFailures;
  db_->extras[id_].nextRetryAt = nextRetryAt;
}

void MemoryRunnerStore::recordClockOffset(int offsetSeconds, Timestamp) {
  std::lock_guard lock(db_->mutex);
  db_->extras[id_].clockOffsetSeconds = offsetSeconds;
}

void MemoryRunnerStore::heartbeat(Timestamp) {
  std::lock_guard lock(db_->mutex);
  ++db_->extras[id_].heartbeats;
}

int MemoryRunnerStore::failInterruptedCommands(const std::string &message) {
  std::lock_guard lock(db_->mutex);
  int failed = 0;
  for (auto &stored : db_->commands) {
    if (stored.connectionId == id_ && stored.status == "running") {
      stored.status = "failed";
      stored.message = message;
      ++failed;
    }
  }
  return failed;
}

std::optional<Command> MemoryRunnerStore::claimNextCommand() {
  std::lock_guard lock(db_->mutex);
  for (auto &stored : db_->commands) {
    if (stored.connectionId == id_ && stored.status == "pending" &&
        !stored.expired) {
      stored.status = "running";
      return stored.command;
    }
  }
  return std::nullopt;
}

void MemoryRunnerStore::completeCommand(CommandId id,
                                        const CommandOutcome &outcome) {
  std::lock_guard lock(db_->mutex);
  auto &stored = db_->commands.at(static_cast<size_t>(id) - 1);
  stored.status = outcome.ok ? "succeeded" : "failed";
  stored.message = outcome.message;
  stored.result = outcome.result;
}

MemoryDatabase::StoredEvent *MemoryRunnerStore::findStored(EventRowId id) {
  for (auto &stored : db_->events) {
    if (stored.event.id == id && stored.connectionId == id_) {
      return &stored;
    }
  }
  return nullptr;
}

std::optional<KnownEvent>
MemoryRunnerStore::findProgramEvent(const std::string &programEventId) {
  std::lock_guard lock(db_->mutex);
  for (const auto &stored : db_->events) {
    if (stored.connectionId == id_ &&
        stored.event.programEventId == programEventId) {
      return db_->withResponses(stored);
    }
  }
  return std::nullopt;
}

std::optional<KnownEvent> MemoryRunnerStore::findEventById(EventRowId id) {
  std::lock_guard lock(db_->mutex);
  const auto *stored = findStored(id);
  if (stored == nullptr) {
    return std::nullopt;
  }
  return db_->withResponses(*stored);
}

std::vector<KnownEvent> MemoryRunnerStore::openProgramEvents() {
  std::lock_guard lock(db_->mutex);
  std::vector<KnownEvent> open;
  for (const auto &stored : db_->events) {
    if (stored.connectionId == id_ && !isTerminal(stored.event.status)) {
      open.push_back(db_->withResponses(stored));
    }
  }
  return open;
}

KnownEvent MemoryRunnerStore::upsertProgramEvent(const NormalizedEvent &event,
                                                 Opt opt,
                                                 OptSource optSource) {
  {
    std::lock_guard lock(db_->mutex);
    for (auto &stored : db_->events) {
      if (stored.connectionId != id_ ||
          stored.event.programEventId != event.programEventId) {
        continue;
      }
      if (stored.event.modificationNumber < event.modificationNumber) {
        stored.latest = event;
        stored.event.modificationNumber = event.modificationNumber;
        stored.event.status = event.status;
        stored.event.startAt = event.startAt;
        stored.event.durationSeconds = event.durationSeconds;
        stored.event.responseRequired = event.responseRequired;
        stored.event.declinable = event.declinable;
        stored.event.signals = event.signals;
        stored.event.programRequestId = event.programRequestId;
      }
      return db_->withResponses(stored);
    }
  }
  const auto id = db_->addEvent(id_, event, opt, optSource);
  return *db_->event(id);
}

void MemoryRunnerStore::setProgramStatus(EventRowId id, ProgramStatus status) {
  std::lock_guard lock(db_->mutex);
  if (auto *stored = findStored(id)) {
    stored->event.status = status;
  }
}

void MemoryRunnerStore::setOpt(EventRowId id, Opt opt, OptSource optSource) {
  std::lock_guard lock(db_->mutex);
  if (auto *stored = findStored(id)) {
    stored->event.opt = opt;
    stored->event.optSource = optSource;
  }
}

void MemoryRunnerStore::recordResponse(EventRowId id, EventAnswer answer,
                                       int modificationNumber, bool ok,
                                       const std::string &message) {
  std::lock_guard lock(db_->mutex);
  db_->responses.push_back({id, answer, modificationNumber, ok, message});
}

void MemoryRunnerStore::appendActivity(const ActivityEntry &entry) {
  std::lock_guard lock(db_->mutex);
  db_->activity.push_back({id_, entry});
}

std::vector<TelemetryPoint> MemoryRunnerStore::loadTelemetryPoints() {
  std::lock_guard lock(db_->mutex);
  const auto found = db_->telemetryPoints.find(id_);
  return found == db_->telemetryPoints.end() ? std::vector<TelemetryPoint>{}
                                             : found->second;
}

std::vector<openadr2b::ReportOfferRow> MemoryRunnerStore::loadReportOffers() {
  std::lock_guard lock(db_->mutex);
  const auto found = db_->reportOffers.find(id_);
  return found == db_->reportOffers.end()
             ? std::vector<openadr2b::ReportOfferRow>{}
             : found->second;
}

std::optional<DataCredential> MemoryRunnerStore::loadDataCredential() {
  std::lock_guard lock(db_->mutex);
  const auto found = db_->dataCredentials.find(id_);
  if (found == db_->dataCredentials.end()) {
    return std::nullopt;
  }
  return found->second;
}

void MemoryRunnerStore::recordReportRequest(
    const openadr2b::ReportRequest &request) {
  std::lock_guard lock(db_->mutex);
  db_->reportRequests.push_back(
      {id_, request, openadr2b::ReportRequestStatus::Active});
}

void MemoryRunnerStore::endReportRequest(
    const std::string &programRequestId,
    openadr2b::ReportRequestStatus status) {
  std::lock_guard lock(db_->mutex);
  for (auto &stored : db_->reportRequests) {
    if (stored.connectionId == id_ &&
        stored.request.programRequestId == programRequestId &&
        stored.status == openadr2b::ReportRequestStatus::Active) {
      stored.status = status;
    }
  }
}

int MemoryRunnerStore::endAllReportRequests(
    openadr2b::ReportRequestStatus status) {
  std::lock_guard lock(db_->mutex);
  int ended = 0;
  for (auto &stored : db_->reportRequests) {
    if (stored.connectionId == id_ &&
        stored.status == openadr2b::ReportRequestStatus::Active) {
      stored.status = status;
      ++ended;
    }
  }
  return ended;
}

void MemoryRunnerStore::recordReportDelivery(const ReportDelivery &delivery) {
  std::lock_guard lock(db_->mutex);
  db_->reportDeliveries.push_back({id_, delivery});
}

// ---------------------------------------------------------------------------
// MemorySupervisorStore

MemorySupervisorStore::MemorySupervisorStore(std::shared_ptr<MemoryDatabase> db)
    : db_(std::move(db)) {}

std::vector<ConnectionSummary> MemorySupervisorStore::listConnections() {
  std::lock_guard lock(db_->mutex);
  std::vector<ConnectionSummary> summaries;
  for (const auto &[id, config] : db_->connections) {
    const auto state = db_->states.find(id);
    summaries.push_back(
        {id, config.protocol, config.enabled, config.configVersion,
         config.archived,
         state != db_->states.end() &&
             state->second.registration == Registration::Registered});
  }
  return summaries;
}

int MemorySupervisorStore::failCommandsForDisabledConnections() {
  std::lock_guard lock(db_->mutex);
  int failed = 0;
  for (auto &stored : db_->commands) {
    const auto connection = db_->connections.find(stored.connectionId);
    if (stored.status == "pending" && connection != db_->connections.end() &&
        !isActive(connection->second)) {
      stored.status = "failed";
      stored.message = connection->second.archived
                           ? "The connection is archived."
                           : "The connection is disabled.";
      ++failed;
    }
  }
  return failed;
}

int MemorySupervisorStore::pruneReportHistory(std::chrono::seconds) {
  ++pruneCalls;
  return 0;
}

int MemorySupervisorStore::failExpiredCommands() {
  std::lock_guard lock(db_->mutex);
  int failed = 0;
  for (auto &stored : db_->commands) {
    if (stored.status == "pending" && stored.expired) {
      stored.status = "failed";
      stored.message = "The command expired before a worker picked it up.";
      ++failed;
    }
  }
  return failed;
}

std::vector<Notification>
MemorySupervisorStore::waitForNotifications(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    {
      std::lock_guard lock(db_->mutex);
      if (!db_->notifications.empty()) {
        std::vector<Notification> taken;
        taken.swap(db_->notifications);
        return taken;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return {};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

RunnerStoreFactory memoryRunnerStores(std::shared_ptr<MemoryDatabase> db) {
  return [db](ConnectionId id) {
    return std::make_unique<MemoryRunnerStore>(db, id);
  };
}

} // namespace grid_services::testing
