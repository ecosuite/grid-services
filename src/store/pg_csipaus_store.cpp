// PgRunnerStore's CSIP-AUS tables: csipaus_connections, csipaus_events,
// csipaus_limits, csipaus_der and csipaus_readings.

#include "store/pg_store.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "store/pg_helpers.hpp"

namespace grid_services {

using namespace csipaus;
using pg::fromEpoch;
using pg::nullable;
using pg::toEpoch;

CsipausSettings PgRunnerStore::loadCsipausSettings() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT dcap_url, ca_cert_pem, verify_server_certificate, "
      "connection_point_id, registration_pin "
      "FROM csipaus_connections WHERE connection_id = $1",
      id_);
  tx.commit();
  if (result.empty()) {
    throw std::runtime_error("no CSIP-AUS settings for connection " +
                             std::to_string(id_));
  }
  const auto &row = result[0];
  CsipausSettings settings;
  settings.dcapUrl = row["dcap_url"].as<std::string>();
  settings.caCertPem = nullable<std::string>(row["ca_cert_pem"]);
  settings.verifyServerCertificate =
      row["verify_server_certificate"].as<bool>();
  settings.connectionPointId =
      nullable<std::string>(row["connection_point_id"]);
  if (const auto pin = nullable<std::int64_t>(row["registration_pin"])) {
    settings.registrationPin = static_cast<std::uint32_t>(*pin);
  }
  return settings;
}

void PgRunnerStore::recordEventDetail(EventRowId eventId,
                                      const CsipausEventDetail &detail) {
  pqxx::work tx(*connection_);
  // Only the connection's own events.
  tx.exec_params(
      R"(INSERT INTO csipaus_events (
           event_id, kind, href, mrid, program_mrid, program_description,
           primacy, creation_time, randomize_start_seconds,
           randomize_duration_seconds, reply_to, response_required,
           effective_start_at, effective_end_at)
         SELECT e.id, $3, $4, $5, $6, $7, $8, to_timestamp($9), $10, $11,
                $12, $13, to_timestamp($14), to_timestamp($15)
         FROM events e WHERE e.id = $1 AND e.connection_id = $2
         ON CONFLICT (event_id) DO UPDATE SET
           kind = EXCLUDED.kind,
           href = EXCLUDED.href,
           mrid = EXCLUDED.mrid,
           program_mrid = EXCLUDED.program_mrid,
           program_description = EXCLUDED.program_description,
           primacy = EXCLUDED.primacy,
           creation_time = EXCLUDED.creation_time,
           randomize_start_seconds = EXCLUDED.randomize_start_seconds,
           randomize_duration_seconds = EXCLUDED.randomize_duration_seconds,
           reply_to = EXCLUDED.reply_to,
           response_required = EXCLUDED.response_required,
           effective_start_at = EXCLUDED.effective_start_at,
           effective_end_at = EXCLUDED.effective_end_at)",
      eventId, id_, toString(detail.kind), detail.href, detail.mrid,
      detail.programMrid, detail.programDescription, detail.primacy,
      toEpoch(detail.creationTime), detail.randomizeStartSeconds,
      detail.randomizeDurationSeconds, detail.replyTo,
      detail.responseRequired, toEpoch(detail.effectiveStartAt),
      toEpoch(detail.effectiveEndAt));
  tx.commit();
}

std::vector<ProgressDue> PgRunnerStore::progressDue() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      R"(SELECT e.id, e.program_event_id, e.modification_number,
                e.program_status,
                extract(epoch FROM e.start_at)::bigint AS start_epoch,
                e.duration_seconds, e.lifecycle,
                c.href, c.mrid, c.program_mrid, c.primacy, c.reply_to,
                c.response_required,
                NOT EXISTS (SELECT 1 FROM event_responses r
                            WHERE r.event_id = e.id AND r.status = 'sent'
                              AND r.response = 'started') AS needs_started,
                NOT EXISTS (SELECT 1 FROM event_responses r
                            WHERE r.event_id = e.id AND r.status = 'sent'
                              AND r.response = 'completed') AS needs_completed
         FROM events e JOIN csipaus_events c ON c.event_id = e.id
         WHERE e.connection_id = $1 AND e.source = 'program'
           AND c.kind = 'control' AND c.reply_to IS NOT NULL
           AND (c.response_required & 2) <> 0
           AND ((c.response_required & 1) = 0
                OR EXISTS (SELECT 1 FROM event_responses r
                           WHERE r.event_id = e.id AND r.status = 'sent'
                             AND r.response = 'received'))
           AND e.program_status NOT IN ('cancelled', 'superseded')
           AND e.lifecycle IN ('control', 'completed')
           AND e.received_at > now() - interval '7 days'
         ORDER BY e.start_at, e.id)",
      id_);
  tx.commit();
  std::vector<ProgressDue> due;
  for (const auto &row : result) {
    std::vector<EventAnswer> answers;
    if (row["needs_started"].as<bool>()) {
      answers.push_back(EventAnswer::Started);
    }
    if (row["lifecycle"].as<std::string>() == "completed" &&
        row["needs_completed"].as<bool>()) {
      answers.push_back(EventAnswer::Completed);
    }
    if (answers.empty()) {
      continue;
    }
    ProgressDue item;
    item.event.id = row["id"].as<EventRowId>();
    item.event.programEventId = row["program_event_id"].as<std::string>();
    item.event.modificationNumber = row["modification_number"].as<int>();
    item.event.status =
        parseProgramStatus(row["program_status"].as<std::string>());
    item.event.startAt = fromEpoch(row["start_epoch"].as<long long>());
    item.event.durationSeconds = row["duration_seconds"].as<int>();
    item.event.declinable = false;
    item.detail.kind = CsipausEventKind::Control;
    item.detail.href = row["href"].as<std::string>();
    item.detail.mrid = row["mrid"].as<std::string>();
    item.detail.programMrid = row["program_mrid"].as<std::string>();
    item.detail.primacy = row["primacy"].as<int>();
    item.detail.replyTo = nullable<std::string>(row["reply_to"]);
    item.detail.responseRequired = row["response_required"].as<int>();
    item.answers = std::move(answers);
    due.push_back(std::move(item));
  }
  return due;
}

void PgRunnerStore::replaceLimits(const std::vector<LimitSegment> &limits) {
  pqxx::work tx(*connection_);
  tx.exec_params("DELETE FROM csipaus_limits WHERE connection_id = $1", id_);
  for (const auto &segment : limits) {
    tx.exec_params(
        "INSERT INTO csipaus_limits (connection_id, limit_name, start_at, "
        "end_at, value, source, event_id, ramp_seconds, set_grad_w) "
        "VALUES ($1, $2, to_timestamp($3), to_timestamp($4), $5, $6, "
        "(SELECT e.id FROM events e WHERE e.connection_id = $1 "
        "AND e.source = 'program' AND e.program_event_id = $7), "
        "$8::numeric / 100, $9)",
        id_, segment.limit, toEpoch(segment.start), toEpoch(segment.end),
        segment.value, toString(segment.source), segment.programEventId,
        segment.rampTms, segment.setGradW);
  }
  tx.commit();
}

std::optional<DerDescription> PgRunnerStore::loadDer() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT der_type, rtg_max_w, rtg_max_va, rtg_max_var, rtg_max_var_neg, "
      "rtg_max_charge_rate_w, rtg_max_discharge_rate_w, rtg_max_wh, "
      "rtg_v_nom, set_max_w, set_max_va, set_max_var, set_max_charge_rate_w, "
      "set_max_discharge_rate_w, set_grad_w, "
      "array_to_json(modes_supported)::text AS modes_supported, "
      "array_to_json(modes_enabled)::text AS modes_enabled, "
      "extract(epoch FROM updated_at)::bigint AS updated_epoch, "
      "connected_point_id, operating_point_id "
      "FROM csipaus_der WHERE connection_id = $1",
      id_);
  tx.commit();
  if (result.empty()) {
    return std::nullopt;
  }
  const auto &row = result[0];
  DerDescription der;
  der.type = row["der_type"].as<int>();
  der.rtgMaxW = row["rtg_max_w"].as<double>();
  der.rtgMaxVa = nullable<double>(row["rtg_max_va"]);
  der.rtgMaxVar = nullable<double>(row["rtg_max_var"]);
  der.rtgMaxVarNeg = nullable<double>(row["rtg_max_var_neg"]);
  der.rtgMaxChargeRateW = nullable<double>(row["rtg_max_charge_rate_w"]);
  der.rtgMaxDischargeRateW = nullable<double>(row["rtg_max_discharge_rate_w"]);
  der.rtgMaxWh = nullable<double>(row["rtg_max_wh"]);
  der.rtgVNom = nullable<double>(row["rtg_v_nom"]);
  der.setMaxW = nullable<double>(row["set_max_w"]);
  der.setMaxVa = nullable<double>(row["set_max_va"]);
  der.setMaxVar = nullable<double>(row["set_max_var"]);
  der.setMaxChargeRateW = nullable<double>(row["set_max_charge_rate_w"]);
  der.setMaxDischargeRateW =
      nullable<double>(row["set_max_discharge_rate_w"]);
  der.setGradW = row["set_grad_w"].as<int>();
  der.modesSupported = nlohmann::json::parse(
                           row["modes_supported"].as<std::string>())
                           .get<std::vector<std::string>>();
  der.modesEnabled = nlohmann::json::parse(
                         row["modes_enabled"].as<std::string>())
                         .get<std::vector<std::string>>();
  der.updatedAt = fromEpoch(row["updated_epoch"].as<long long>());
  der.connectedPointId = nullable<std::string>(row["connected_point_id"]);
  der.operatingPointId = nullable<std::string>(row["operating_point_id"]);
  return der;
}

std::vector<ReadingRow> PgRunnerStore::loadReadings() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT mirror, reading, phase, point_id, power_of_ten "
      "FROM csipaus_readings "
      "WHERE connection_id = $1 ORDER BY mirror DESC, reading, phase",
      id_);
  tx.commit();
  std::vector<ReadingRow> rows;
  for (const auto &row : result) {
    rows.push_back({parseMirror(row["mirror"].as<std::string>()),
                    parseReadingKind(row["reading"].as<std::string>()),
                    parsePhase(row["phase"].as<std::string>()),
                    row["point_id"].as<std::string>(),
                    nullable<int>(row["power_of_ten"])});
  }
  return rows;
}

} // namespace grid_services
