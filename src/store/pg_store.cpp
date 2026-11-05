#include "store/pg_store.hpp"

#include <climits>
#include <stdexcept>

#include <pqxx/pqxx>

#include "store/pg_helpers.hpp"

namespace grid_services {

using pg::fromEpoch;
using pg::jsonText;
using pg::nullable;
using pg::toEpoch;

namespace {

const char *eventColumns = R"(
    e.id, e.program_event_id, e.modification_number, e.program_status,
    extract(epoch FROM e.start_at)::bigint AS start_epoch, e.duration_seconds,
    e.response_required, e.declinable, e.signals::text AS signals, e.opt,
    e.opt_source, e.program_request_id,
    (SELECT max(r.modification_number) FROM event_responses r
      WHERE r.event_id = e.id AND r.status = 'sent') AS responded_modification
)";

KnownEvent readEvent(const pqxx::row &row) {
  KnownEvent event;
  event.id = row["id"].as<EventRowId>();
  event.programEventId = row["program_event_id"].as<std::string>();
  event.modificationNumber = row["modification_number"].as<int>();
  event.status = parseProgramStatus(row["program_status"].as<std::string>());
  event.startAt = fromEpoch(row["start_epoch"].as<long long>());
  event.durationSeconds = row["duration_seconds"].as<int>();
  event.responseRequired = row["response_required"].as<bool>();
  event.declinable = row["declinable"].as<bool>();
  event.signals =
      signalsFromJson(nlohmann::json::parse(row["signals"].as<std::string>()));
  event.opt = parseOpt(row["opt"].as<std::string>());
  event.optSource = parseOptSource(row["opt_source"].as<std::string>());
  event.programRequestId = nullable<std::string>(row["program_request_id"]);
  event.respondedModification = nullable<int>(row["responded_modification"]);
  return event;
}

template <typename... Args>
std::vector<KnownEvent> selectEvents(pqxx::connection &connection,
                                     ConnectionId id,
                                     const std::string &condition,
                                     const Args &...args) {
  pqxx::work tx(connection);
  const auto result = tx.exec_params(
      std::string("SELECT ") + eventColumns +
          " FROM events e WHERE e.source = 'program' AND e.connection_id = $1 "
          "AND " +
          condition + " ORDER BY e.start_at, e.id",
      id, args...);
  tx.commit();
  std::vector<KnownEvent> events;
  for (const auto &row : result) {
    events.push_back(readEvent(row));
  }
  return events;
}

} // namespace

// ---------------------------------------------------------------------------
// PgRunnerStore

PgRunnerStore::PgRunnerStore(const std::string &connectionString,
                             ConnectionId id)
    : id_(id),
      connection_(std::make_unique<pqxx::connection>(connectionString)) {}

PgRunnerStore::~PgRunnerStore() = default;

bool PgRunnerStore::tryLock() {
  if (id_ > INT_MAX) {
    throw std::out_of_range("connection id too large for an advisory lock");
  }
  pqxx::nontransaction tx(*connection_);
  const auto result = tx.exec_params("SELECT pg_try_advisory_lock($1, $2)",
                                     connectionLockKey, static_cast<int>(id_));
  return result[0][0].as<bool>();
}

ConnectionConfig PgRunnerStore::loadConfig() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT id, site_code, protocol, enabled, default_opt, "
      "poll_interval_seconds, poll_jitter, config_version, "
      "archived_at IS NOT NULL AS archived "
      "FROM connections WHERE id = $1",
      id_);
  tx.commit();
  if (result.empty()) {
    throw std::runtime_error("connection " + std::to_string(id_) +
                             " does not exist");
  }
  const auto &row = result[0];
  ConnectionConfig config;
  config.id = row["id"].as<ConnectionId>();
  config.siteCode = row["site_code"].as<std::string>();
  config.protocol = row["protocol"].as<std::string>();
  config.enabled = row["enabled"].as<bool>();
  config.defaultOpt = parseOpt(row["default_opt"].as<std::string>());
  config.pollIntervalSeconds = nullable<int>(row["poll_interval_seconds"]);
  config.pollJitter = row["poll_jitter"].as<double>();
  config.configVersion = row["config_version"].as<int>();
  config.archived = row["archived"].as<bool>();
  return config;
}

std::optional<ClientCredential> PgRunnerStore::loadClientCredential() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT k.cert_pem, k.key_sealed FROM connections c "
      "JOIN client_credentials k ON k.id = c.client_credential_id "
      "WHERE c.id = $1",
      id_);
  tx.commit();
  if (result.empty()) {
    return std::nullopt;
  }
  return ClientCredential{result[0]["cert_pem"].as<std::string>(),
                          result[0]["key_sealed"].as<std::string>()};
}

ConnectionState PgRunnerStore::loadState() {
  pqxx::work tx(*connection_);
  tx.exec_params("INSERT INTO connection_state (connection_id) VALUES ($1) "
                 "ON CONFLICT (connection_id) DO NOTHING",
                 id_);
  const auto result = tx.exec_params(
      "SELECT registration, participation, client_id, registration_id, "
      "program_poll_interval_seconds FROM connection_state "
      "WHERE connection_id = $1",
      id_);
  tx.commit();
  const auto &row = result[0];
  ConnectionState state;
  state.registration = parseRegistration(row["registration"].as<std::string>());
  state.participation =
      parseParticipation(row["participation"].as<std::string>());
  state.clientId = nullable<std::string>(row["client_id"]);
  state.registrationId = nullable<std::string>(row["registration_id"]);
  state.programPollIntervalSeconds =
      nullable<int>(row["program_poll_interval_seconds"]);
  return state;
}

void PgRunnerStore::markStarted(int configVersion, const std::string &workerId,
                                const std::optional<std::string> &configError) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET applied_config_version = $2, "
                 "worker_id = $3, heartbeat_at = now(), config_error = $4, "
                 "consecutive_failures = 0, next_retry_at = NULL "
                 "WHERE connection_id = $1",
                 id_, configVersion, workerId, configError);
  tx.commit();
}

void PgRunnerStore::setRegistration(Registration registration) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET registration = $2 "
                 "WHERE connection_id = $1",
                 id_, toString(registration));
  tx.commit();
}

void PgRunnerStore::setRegistered(const RegistrationInfo &info) {
  pqxx::work tx(*connection_);
  tx.exec_params(
      "UPDATE connection_state SET registration = 'registered', "
      "client_id = $2, registration_id = $3, "
      "program_poll_interval_seconds = $4 WHERE connection_id = $1",
      id_, info.clientId, info.registrationId, info.pollIntervalSeconds);
  tx.commit();
}

void PgRunnerStore::setProgramPollInterval(int seconds) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET program_poll_interval_seconds = "
                 "$2 WHERE connection_id = $1",
                 id_, seconds);
  tx.commit();
}

void PgRunnerStore::clearRegistration() {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET registration = 'unregistered', "
                 "registration_id = NULL, program_poll_interval_seconds = NULL "
                 "WHERE connection_id = $1",
                 id_);
  tx.commit();
}

void PgRunnerStore::setParticipation(Participation participation) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET participation = $2 "
                 "WHERE connection_id = $1",
                 id_, toString(participation));
  tx.commit();
}

void PgRunnerStore::recordPoll(Timestamp at) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET last_poll_at = to_timestamp($2), "
                 "last_contact_at = to_timestamp($2), last_error = NULL, "
                 "last_error_at = NULL, consecutive_failures = 0, "
                 "next_retry_at = NULL WHERE connection_id = $1",
                 id_, toEpoch(at));
  tx.commit();
}

void PgRunnerStore::recordError(const std::string &message, Timestamp at) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET last_error = $2, "
                 "last_error_at = to_timestamp($3) WHERE connection_id = $1",
                 id_, message, toEpoch(at));
  tx.commit();
}

void PgRunnerStore::recordFailures(int consecutiveFailures,
                                   std::optional<Timestamp> nextRetryAt) {
  const auto retryEpoch =
      nextRetryAt ? std::optional<long long>(toEpoch(*nextRetryAt))
                  : std::nullopt;
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET consecutive_failures = $2, "
                 "next_retry_at = to_timestamp($3) WHERE connection_id = $1",
                 id_, consecutiveFailures, retryEpoch);
  tx.commit();
}

void PgRunnerStore::recordClockOffset(int offsetSeconds, Timestamp at) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET clock_offset_seconds = $2, "
                 "clock_checked_at = to_timestamp($3) WHERE connection_id = $1",
                 id_, offsetSeconds, toEpoch(at));
  tx.commit();
}

void PgRunnerStore::heartbeat(Timestamp at) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_state SET heartbeat_at = to_timestamp($2) "
                 "WHERE connection_id = $1",
                 id_, toEpoch(at));
  tx.commit();
}

int PgRunnerStore::failInterruptedCommands(const std::string &message) {
  pqxx::work tx(*connection_);
  const auto result =
      tx.exec_params("UPDATE connection_commands SET status = 'failed', "
                     "message = $2, completed_at = now() "
                     "WHERE connection_id = $1 AND status = 'running'",
                     id_, message);
  tx.commit();
  return static_cast<int>(result.affected_rows());
}

std::optional<Command> PgRunnerStore::claimNextCommand() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "UPDATE connection_commands SET status = 'running', claimed_at = now() "
      "WHERE id = (SELECT id FROM connection_commands "
      "  WHERE connection_id = $1 AND status = 'pending' "
      "  AND expires_at > now() "
      "  ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED) "
      "RETURNING id, kind, event_id, requested_by",
      id_);
  tx.commit();
  if (result.empty()) {
    return std::nullopt;
  }
  const auto &row = result[0];
  Command command;
  command.id = row["id"].as<CommandId>();
  command.kind = parseCommandKind(row["kind"].as<std::string>());
  command.eventId = nullable<EventRowId>(row["event_id"]);
  command.requestedBy = nullable<std::string>(row["requested_by"]);
  return command;
}

void PgRunnerStore::completeCommand(CommandId id,
                                    const CommandOutcome &outcome) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE connection_commands SET status = $2, message = $3, "
                 "result = $4::jsonb, completed_at = now() WHERE id = $1",
                 id, outcome.ok ? "succeeded" : "failed", outcome.message,
                 jsonText(outcome.result));
  tx.commit();
}

std::optional<KnownEvent>
PgRunnerStore::findProgramEvent(const std::string &programEventId) {
  auto events = selectEvents(*connection_, id_, "e.program_event_id = $2",
                             programEventId);
  if (events.empty()) {
    return std::nullopt;
  }
  return events.front();
}

std::optional<KnownEvent> PgRunnerStore::findEventById(EventRowId id) {
  auto events = selectEvents(*connection_, id_, "e.id = $2", id);
  if (events.empty()) {
    return std::nullopt;
  }
  return events.front();
}

std::vector<KnownEvent> PgRunnerStore::openProgramEvents() {
  return selectEvents(*connection_, id_,
                      "e.program_status IN ('pending', 'active')");
}

KnownEvent PgRunnerStore::upsertProgramEvent(const NormalizedEvent &event,
                                             Opt opt, OptSource optSource) {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      R"(INSERT INTO events (
           site_code, site_id, source, connection_id, program_event_id,
           modification_number, program_status, start_at, duration_seconds,
           start_after_seconds, signals, market_context, test_event,
           response_required, program_comment, program_request_id, opt,
           opt_source, declinable)
         SELECT c.site_code, c.site_id, 'program', c.id, $2, $3, $4,
                to_timestamp($5), $6, $15, $7::jsonb, $8, $9, $10, $11, $12,
                $13, $14, $16
         FROM connections c WHERE c.id = $1
         ON CONFLICT (connection_id, program_event_id) WHERE source = 'program'
         DO UPDATE SET
           modification_number = EXCLUDED.modification_number,
           program_status = EXCLUDED.program_status,
           start_at = EXCLUDED.start_at,
           duration_seconds = EXCLUDED.duration_seconds,
           start_after_seconds = EXCLUDED.start_after_seconds,
           signals = EXCLUDED.signals,
           market_context = EXCLUDED.market_context,
           test_event = EXCLUDED.test_event,
           response_required = EXCLUDED.response_required,
           program_comment = EXCLUDED.program_comment,
           program_request_id = EXCLUDED.program_request_id,
           declinable = EXCLUDED.declinable,
           updated_at = now()
         WHERE events.modification_number < EXCLUDED.modification_number
         RETURNING id)",
      id_, event.programEventId, event.modificationNumber,
      toString(event.status), toEpoch(event.startAt), event.durationSeconds,
      signalsToJson(event.signals).dump(), event.marketContext, event.testEvent,
      event.responseRequired, event.comment, event.programRequestId,
      toString(opt), toString(optSource), event.startAfterSeconds,
      event.declinable);
  tx.commit();

  const auto stored = findProgramEvent(event.programEventId);
  if (!stored) {
    throw std::runtime_error("event " + event.programEventId +
                             " was not stored");
  }
  return *stored;
}

void PgRunnerStore::setProgramStatus(EventRowId id, ProgramStatus status) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE events SET program_status = $3, updated_at = now() "
                 "WHERE id = $1 AND connection_id = $2",
                 id, id_, toString(status));
  tx.commit();
}

void PgRunnerStore::setOpt(EventRowId id, Opt opt, OptSource optSource) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE events SET opt = $3, opt_source = $4, "
                 "updated_at = now() WHERE id = $1 AND connection_id = $2",
                 id, id_, toString(opt), toString(optSource));
  tx.commit();
}

void PgRunnerStore::recordResponse(EventRowId id, EventAnswer answer,
                                   int modificationNumber, bool ok,
                                   const std::string &message) {
  pqxx::work tx(*connection_);
  tx.exec_params("INSERT INTO event_responses (event_id, response, "
                 "modification_number, status, message) "
                 "VALUES ($1, $2, $3, $4, $5)",
                 id, toString(answer), modificationNumber,
                 ok ? "sent" : "failed", message);
  tx.commit();
}

void PgRunnerStore::appendActivity(const ActivityEntry &entry) {
  pqxx::work tx(*connection_);
  tx.exec_params(
      "INSERT INTO activity (site_code, site_id, connection_id, event_id, "
      "category, actor, message, detail) "
      "SELECT c.site_code, c.site_id, c.id, $2, $3, COALESCE($4, 'system'), "
      "$5, $6::jsonb FROM connections c WHERE c.id = $1",
      id_, entry.eventId, toString(entry.category), entry.actor, entry.message,
      jsonText(entry.detail));
  tx.commit();
}

std::vector<TelemetryPoint> PgRunnerStore::loadTelemetryPoints() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT id, point_id, kind, units, scale, description, source::text "
      "FROM telemetry_points WHERE connection_id = $1 ORDER BY id",
      id_);
  tx.commit();
  std::vector<TelemetryPoint> points;
  for (const auto &row : result) {
    TelemetryPoint point;
    point.id = row["id"].as<TelemetryPointId>();
    point.pointId = row["point_id"].as<std::string>();
    point.item.kind = row["kind"].as<std::string>();
    point.item.units = row["units"].as<std::string>();
    point.item.scale = row["scale"].as<std::string>();
    point.item.description =
        nullable<std::string>(row["description"]).value_or("");
    const std::string where = "telemetry point " + point.pointId;
    checkItem(point.item, where);
    try {
      point.source = parseDatumSource(
          nlohmann::json::parse(row["source"].as<std::string>()), where);
    } catch (const nlohmann::json::exception &error) {
      throw std::invalid_argument(where + ": " + error.what());
    }
    points.push_back(std::move(point));
  }
  return points;
}

std::optional<DataCredential> PgRunnerStore::loadDataCredential() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT host, token, secret_sealed FROM connection_data_credentials "
      "WHERE connection_id = $1",
      id_);
  tx.commit();
  if (result.empty()) {
    return std::nullopt;
  }
  const auto &row = result[0];
  return DataCredential{row["host"].as<std::string>(),
                        row["token"].as<std::string>(),
                        row["secret_sealed"].as<std::string>()};
}

void PgRunnerStore::recordReportDelivery(const ReportDelivery &delivery) {
  pqxx::work tx(*connection_);
  // The request is OpenADR's; other protocols' deliveries have none.
  tx.exec_params(
      "INSERT INTO report_deliveries (request_id, connection_id, "
      "interval_start, interval_end, payload, status, response_code, "
      "message, reads, report, exchange) "
      "SELECT (SELECT r.id FROM openadr2b_report_requests r "
      "        WHERE r.connection_id = $1 AND r.report_request_id = $2 "
      "        ORDER BY r.id DESC LIMIT 1), "
      "$1, to_timestamp($3), to_timestamp($4), $5::jsonb, $6, $7, $8, "
      "$9::jsonb, $10, $11::jsonb "
      "WHERE $2::text IS NULL OR EXISTS (SELECT 1 "
      "  FROM openadr2b_report_requests r "
      "  WHERE r.connection_id = $1 AND r.report_request_id = $2)",
      id_, delivery.programRequestId, toEpoch(delivery.intervalStart),
      toEpoch(delivery.intervalEnd), delivery.payload.dump(),
      delivery.ok ? "sent" : "failed", delivery.responseCode,
      delivery.message.empty() ? std::optional<std::string>()
                               : std::optional<std::string>(delivery.message),
      delivery.reads.dump(), delivery.report,
      delivery.exchange.is_null()
          ? std::optional<std::string>()
          : std::optional<std::string>(delivery.exchange.dump()));
  tx.commit();
}

// ---------------------------------------------------------------------------
// PgSupervisorStore

class PgSupervisorStore::Listener : public pqxx::notification_receiver {
public:
  Listener(pqxx::connection_base &connection, const std::string &channel,
           Notification::Channel kind, std::vector<Notification> &pending)
      : pqxx::notification_receiver(connection, channel), kind_(kind),
        pending_(pending) {}

  void operator()(const std::string &payload, int) override {
    try {
      pending_.push_back({kind_, std::stoll(payload)});
    } catch (const std::exception &) {
      // Ignore payloads that are not connection ids.
    }
  }

private:
  Notification::Channel kind_;
  std::vector<Notification> &pending_;
};

PgSupervisorStore::PgSupervisorStore(std::string connectionString)
    : connectionString_(std::move(connectionString)) {}

PgSupervisorStore::~PgSupervisorStore() { reset(); }

void PgSupervisorStore::reset() {
  listeners_.clear();
  connection_.reset();
}

pqxx::connection &PgSupervisorStore::connection() {
  if (connection_ && connection_->is_open()) {
    return *connection_;
  }
  reset();
  connection_ = std::make_unique<pqxx::connection>(connectionString_);
  listeners_.push_back(std::make_unique<Listener>(
      *connection_, "grid_services_config", Notification::Channel::Config,
      pending_));
  listeners_.push_back(std::make_unique<Listener>(
      *connection_, "grid_services_commands", Notification::Channel::Commands,
      pending_));
  return *connection_;
}

std::vector<ConnectionSummary> PgSupervisorStore::listConnections() {
  try {
    pqxx::work tx(connection());
    const auto result = tx.exec(
        "SELECT c.id, c.protocol, c.enabled, c.config_version, "
        "c.archived_at IS NOT NULL AS archived, "
        "COALESCE(s.registration = 'registered', false) AS registered "
        "FROM connections c "
        "LEFT JOIN connection_state s ON s.connection_id = c.id "
        "ORDER BY c.id");
    tx.commit();
    std::vector<ConnectionSummary> connections;
    for (const auto &row : result) {
      connections.push_back({row["id"].as<ConnectionId>(),
                             row["protocol"].as<std::string>(),
                             row["enabled"].as<bool>(),
                             row["config_version"].as<int>(),
                             row["archived"].as<bool>(),
                             row["registered"].as<bool>()});
    }
    return connections;
  } catch (const pqxx::broken_connection &) {
    reset();
    throw;
  }
}

int PgSupervisorStore::failCommandsForDisabledConnections() {
  try {
    pqxx::work tx(connection());
    const auto result = tx.exec(
        "UPDATE connection_commands m SET status = 'failed', "
        "message = CASE WHEN c.archived_at IS NOT NULL "
        "  THEN 'The connection is archived.' "
        "  ELSE 'The connection is disabled.' END, "
        "completed_at = now() FROM connections c "
        "WHERE m.status = 'pending' AND c.id = m.connection_id "
        "AND (NOT c.enabled OR c.archived_at IS NOT NULL)");
    tx.commit();
    return static_cast<int>(result.affected_rows());
  } catch (const pqxx::broken_connection &) {
    reset();
    throw;
  }
}

int PgSupervisorStore::pruneReportHistory(std::chrono::seconds retention) {
  try {
    pqxx::work tx(connection());
    const auto deleted = tx.exec_params(
        "DELETE FROM report_deliveries "
        "WHERE sent_at < now() - make_interval(secs => $1)",
        static_cast<long long>(retention.count()));
    tx.exec_params(
        "DELETE FROM openadr2b_report_requests r WHERE r.status <> 'active' "
        "AND r.ended_at < now() - make_interval(secs => $1) "
        "AND NOT EXISTS (SELECT 1 FROM report_deliveries d "
        "WHERE d.request_id = r.id)",
        static_cast<long long>(retention.count()));
    tx.commit();
    return static_cast<int>(deleted.affected_rows());
  } catch (const pqxx::broken_connection &) {
    reset();
    throw;
  }
}

int PgSupervisorStore::failExpiredCommands() {
  try {
    pqxx::work tx(connection());
    const auto result = tx.exec(
        "UPDATE connection_commands SET status = 'failed', "
        "message = 'The command expired before a worker picked it up.', "
        "completed_at = now() "
        "WHERE status = 'pending' AND expires_at <= now()");
    tx.commit();
    return static_cast<int>(result.affected_rows());
  } catch (const pqxx::broken_connection &) {
    reset();
    throw;
  }
}

std::vector<Notification>
PgSupervisorStore::waitForNotifications(std::chrono::milliseconds timeout) {
  try {
    auto &conn = connection();
    if (pending_.empty()) {
      const auto seconds = timeout.count() / 1000;
      const auto micros = (timeout.count() % 1000) * 1000;
      conn.await_notification(seconds, micros);
    }
    conn.get_notifs();
  } catch (const pqxx::broken_connection &) {
    reset();
    throw;
  }
  std::vector<Notification> notifications;
  notifications.swap(pending_);
  return notifications;
}

} // namespace grid_services
