#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/telemetry.hpp"
#include "core/types.hpp"
#include "datum/datum.hpp"

namespace grid_services::openadr2b {

using ReportOfferId = std::int64_t;

// One rID in an offered report: a telemetry point, described in OpenADR
// terms.
struct ReportDataPoint {
  std::string rid;
  // The telemetry point the value comes from.
  std::string pointId;
  std::string resourceId;
  // OpenADR ReportEnumeratedType: usage, demand, baseline, storedEnergy, ...
  std::string reportType = "usage";
  // OpenADR ReadingTypeEnumeratedType: Direct Read, Mean, Summed, ...
  std::string readingType = "Direct Read";
  // The point's, copied here when the offer is read.
  TelemetryItem item;
  datum::DatumSource source;
};

// A report the connection offers the program.
struct ReportOffer {
  ReportOfferId id = 0;
  std::string reportName;
  std::string specifierId;
  bool enabled = true;
  int minSamplingSeconds = 60;
  int maxSamplingSeconds = 3600;
  // How far back the report's data is served. Zero offers no history.
  int historySeconds = 0;
  std::vector<ReportDataPoint> dataPoints;
};

// An offer as stored, before its data points are bound to the connection's
// telemetry points.
struct ReportOfferRow {
  ReportOfferId id = 0;
  std::string reportName;
  std::string specifierId;
  bool enabled = true;
  int minSamplingSeconds = 60;
  int maxSamplingSeconds = 3600;
  int historySeconds = 0;
  // [{"rid"?, "point_id", "resource_id", "report_type"?, "reading_type"?}]
  nlohmann::json dataPoints = nlohmann::json::array();
};

enum class ReportRequestKind { OneShot, Periodic, History };
enum class ReportRequestStatus { Active, Completed, Cancelled };

std::string toString(ReportRequestKind value);
std::string toString(ReportRequestStatus value);

// A report request the program made.
struct ReportRequest {
  std::optional<ReportOfferId> offerId;
  // The program's reportRequestID.
  std::string programRequestId;
  std::string specifierId;
  std::string reportName;
  ReportRequestKind kind = ReportRequestKind::OneShot;
  int granularitySeconds = 0;
  int reportBackSeconds = 0;
  // Null for a one-shot request.
  std::optional<Timestamp> startAt;
  std::optional<int> durationSeconds;
  std::vector<std::string> rids;
};

// Binds an offer's data points to the connection's telemetry points. Throws
// std::invalid_argument for a data point the worker cannot report.
ReportOffer bindOffer(const ReportOfferRow &row,
                      const std::vector<TelemetryPoint> &points);

} // namespace grid_services::openadr2b
