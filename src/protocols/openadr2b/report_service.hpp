#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <oadr/manager/reportmanager/IReportService.h>
#include <oadrsd/2b/oadrReportPayloadType.hxx>

#include "core/adapter.hpp"
#include "core/types.hpp"
#include "datum/datum.hpp"
#include "protocols/openadr2b/report_requests.hpp"
#include "protocols/openadr2b/reports.hpp"

namespace grid_services::openadr2b {

// What a report carried, kept for the adapter to record once it knows
// whether the program took it.
struct GeneratedReport {
  // [{"rid", "point_id"?, "dtstart", "duration_seconds", "value" |
  //   "online" + "manual_override", "quality", "source"?}]
  nlohmann::json payload = nlohmann::json::array();
  // The datum reads behind the values, one per stream:
  // [{"node_id", "source_id", "kind", "path", "query", "datum_count",
  //   "error"}]
  nlohmann::json reads = nlohmann::json::array();
  std::optional<Timestamp> intervalStart;
  std::optional<Timestamp> intervalEnd;
};

// The reports the VEN offers and how it fills them.
//
//   - TELEMETRY_STATUS is always offered, with one resource status item:
//     online while the connection runs, manual override while participation
//     is paused.
//   - Every enabled offer from the database is a report whose data points
//     are read from SolarNetwork datums when the program asks.
//
// liboadr's ReportManager drives the callbacks; the adapter tells the
// service which sink to report to before each operation, and takes the
// generated payloads to record deliveries. The program's requests are
// recorded in `requests`.
class ReportService : public IReportService {
public:
  static constexpr const char *statusSpecifierId = "TELEMETRY_STATUS";
  static constexpr const char *statusRid = "status";

  ReportService(std::string resourceId, std::optional<std::string> marketContext,
                std::vector<ReportOffer> offers,
                std::shared_ptr<datum::DatumReader> reader,
                ReportRequestLog &requests, std::function<std::time_t()> clock);

  void setParticipation(Participation participation);
  void setSink(ProgramEventSink *sink) { sink_ = sink; }
  // Whether the program is cancelling requests; decides how a request that
  // completes is recorded.
  void setCancelling(bool cancelling) { cancelling_ = cancelling; }

  const std::vector<ReportOffer> &offers() const { return offers_; }
  // The enabled offer behind a specifier, or null. TELEMETRY_STATUS is not
  // an offer.
  const ReportOffer *offer(const std::string &specifierId) const;
  // Whether the specifier names a report the VEN offers.
  bool offersSpecifier(const std::string &specifierId) const;
  // Why a request cannot be served, or nothing when it can: an OpenADR
  // response code and description.
  std::optional<std::pair<std::string, std::string>>
  refusal(const oadrReportRequestType &request) const;

  std::optional<GeneratedReport> takeGenerated(const std::string &requestId);

  // IReportService
  void OnPeriodicReportStart(const oadrReportRequestType &) override {}
  void OnPeriodicReportComplete(const oadrReportRequestType &request) override;
  void OnGenerateOneshotReport(
      const oadrReportRequestType &request,
      oadrUpdateReportType::oadrReport_sequence &sequence) override;
  void OnGeneratePeriodicReport(
      const oadrReportRequestType &request,
      oadrUpdateReportType::oadrReport_sequence &sequence, time_t dtstart,
      unsigned int durationInSeconds) override;
  void OnGenerateHistoricalReport(
      const oadrReportRequestType &request,
      oadrUpdateReportType::oadrReport_sequence &sequence, time_t dtstart,
      unsigned int durationInSeconds) override;
  void OnGenerateRegisterReport(
      oadrRegisterReportType::oadrReport_sequence &sequence) override;
  void OnCreatedReport(
      oadrCreateReportType::oadrReportRequest_sequence &sequence) override;

private:
  struct Stream {
    datum::NodeId nodeId;
    std::string sourceId;
    bool operator<(const Stream &other) const {
      return std::tie(nodeId, sourceId) < std::tie(other.nodeId, other.sourceId);
    }
  };
  // Datums per stream for one report, read once and shared by its data
  // points. A stream that could not be read is absent, and `failure` says
  // why.
  struct Datums {
    std::map<Stream, std::vector<datum::Datum>> byStream;
    std::optional<std::string> failure;
    nlohmann::json reads = nlohmann::json::array();
  };

  // The requested rIDs that the report offers, in request order.
  std::vector<std::string>
  requestedRids(const oadrReportRequestType &request) const;
  const ReportDataPoint *dataPoint(const ReportOffer &offer,
                                   const std::string &rid) const;
  Datums readDatums(const ReportOffer &offer,
                    const std::vector<std::string> &rids, Timestamp start,
                    Timestamp end);
  // Appends one report with intervals of `granularity` over [start, end).
  void appendIntervals(const oadrReportRequestType &request,
                       oadrUpdateReportType::oadrReport_sequence &sequence,
                       Timestamp start, Timestamp end, int granularitySeconds);
  void appendStatusReport(const oadrReportRequestType &request,
                          oadrUpdateReportType::oadrReport_sequence &sequence,
                          Timestamp start, int durationSeconds);
  oadr2b::oadr::oadrReportPayloadType payloadFor(const std::string &rid,
                                   const datum::Sample &sample) const;
  void remember(const std::string &requestId, GeneratedReport report);

  std::string resourceId_;
  std::optional<std::string> marketContext_;
  std::vector<ReportOffer> offers_;
  std::shared_ptr<datum::DatumReader> reader_;
  ReportRequestLog &requests_;
  std::function<std::time_t()> clock_;
  ProgramEventSink *sink_ = nullptr;
  Participation participation_ = Participation::Active;
  bool cancelling_ = false;
  std::map<std::string, GeneratedReport> generated_;
};

// Seconds in an ISO 8601 duration such as PT5M; zero for an empty one.
int durationSeconds(const std::string &iso8601);

} // namespace grid_services::openadr2b
