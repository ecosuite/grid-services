// PgRunnerStore's OpenADR 2.0b tables: openadr2b_connections,
// openadr2b_report_offers and openadr2b_report_requests.

#include "store/pg_store.hpp"

#include <pqxx/pqxx>

#include "store/pg_helpers.hpp"

namespace grid_services {

using namespace openadr2b;
using pg::nullable;
using pg::toEpoch;

Openadr2bSettings PgRunnerStore::loadOpenadr2bSettings() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT vtn_url, ven_name, ven_id, market_context, ca_cert_pem, "
      "verify_server_certificate, "
      "to_json(accepted_signals)::text AS accepted_signals "
      "FROM openadr2b_connections WHERE connection_id = $1",
      id_);
  tx.commit();
  if (result.empty()) {
    throw std::runtime_error("no OpenADR 2.0b settings for connection " +
                             std::to_string(id_));
  }
  const auto &row = result[0];
  Openadr2bSettings settings;
  settings.vtnUrl = row["vtn_url"].as<std::string>();
  settings.venName = row["ven_name"].as<std::string>();
  settings.venId = nullable<std::string>(row["ven_id"]);
  settings.marketContext = nullable<std::string>(row["market_context"]);
  settings.caCertPem = nullable<std::string>(row["ca_cert_pem"]);
  settings.verifyServerCertificate =
      row["verify_server_certificate"].as<bool>();
  settings.acceptedSignals =
      nlohmann::json::parse(row["accepted_signals"].as<std::string>())
          .get<std::vector<std::string>>();
  return settings;
}

std::vector<ReportOfferRow> PgRunnerStore::loadReportOffers() {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "SELECT id, report_name, specifier_id, enabled, min_sampling_seconds, "
      "max_sampling_seconds, history_seconds, data_points::text "
      "FROM openadr2b_report_offers WHERE connection_id = $1 ORDER BY id",
      id_);
  tx.commit();
  std::vector<ReportOfferRow> offers;
  for (const auto &row : result) {
    ReportOfferRow offer;
    offer.id = row["id"].as<ReportOfferId>();
    offer.reportName = row["report_name"].as<std::string>();
    offer.specifierId = row["specifier_id"].as<std::string>();
    offer.enabled = row["enabled"].as<bool>();
    offer.minSamplingSeconds = row["min_sampling_seconds"].as<int>();
    offer.maxSamplingSeconds = row["max_sampling_seconds"].as<int>();
    offer.historySeconds = row["history_seconds"].as<int>();
    offer.dataPoints = nlohmann::json::parse(row["data_points"].as<std::string>());
    offers.push_back(std::move(offer));
  }
  return offers;
}

void PgRunnerStore::recordReportRequest(const ReportRequest &request) {
  pqxx::work tx(*connection_);
  // The program may reuse a request id; the earlier request is over.
  tx.exec_params("UPDATE openadr2b_report_requests SET status = 'completed', "
                 "ended_at = now() WHERE connection_id = $1 "
                 "AND report_request_id = $2 AND status = 'active'",
                 id_, request.programRequestId);
  tx.exec_params(
      "INSERT INTO openadr2b_report_requests (connection_id, offer_id, "
      "report_request_id, specifier_id, report_name, kind, "
      "granularity_seconds, report_back_seconds, start_at, duration_seconds, "
      "rids) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, "
      "to_timestamp($9), $10, "
      "ARRAY(SELECT jsonb_array_elements_text($11::jsonb)))",
      id_, request.offerId, request.programRequestId, request.specifierId,
      request.reportName, toString(request.kind), request.granularitySeconds,
      request.reportBackSeconds, toEpoch(request.startAt),
      request.durationSeconds, nlohmann::json(request.rids).dump());
  tx.commit();
}

void PgRunnerStore::endReportRequest(const std::string &programRequestId,
                                     ReportRequestStatus status) {
  pqxx::work tx(*connection_);
  tx.exec_params("UPDATE openadr2b_report_requests SET status = $3, "
                 "ended_at = now() WHERE connection_id = $1 "
                 "AND report_request_id = $2 AND status = 'active'",
                 id_, programRequestId, toString(status));
  tx.commit();
}

int PgRunnerStore::endAllReportRequests(ReportRequestStatus status) {
  pqxx::work tx(*connection_);
  const auto result = tx.exec_params(
      "UPDATE openadr2b_report_requests SET status = $2, ended_at = now() "
      "WHERE connection_id = $1 AND status = 'active'",
      id_, toString(status));
  tx.commit();
  return static_cast<int>(result.affected_rows());
}

} // namespace grid_services
