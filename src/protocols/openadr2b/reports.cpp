#include "protocols/openadr2b/reports.hpp"

#include <set>
#include <stdexcept>

namespace grid_services::openadr2b {

std::string toString(ReportRequestKind value) {
  switch (value) {
  case ReportRequestKind::OneShot:
    return "one_shot";
  case ReportRequestKind::Periodic:
    return "periodic";
  case ReportRequestKind::History:
    return "history";
  }
  return "unknown";
}

std::string toString(ReportRequestStatus value) {
  switch (value) {
  case ReportRequestStatus::Active:
    return "active";
  case ReportRequestStatus::Completed:
    return "completed";
  case ReportRequestStatus::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

namespace {

std::string requiredString(const nlohmann::json &object, const char *key,
                           const std::string &where) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_string() ||
      found->get<std::string>().empty()) {
    throw std::invalid_argument(where + " needs a " + key);
  }
  return found->get<std::string>();
}

std::string optionalString(const nlohmann::json &object, const char *key,
                           const std::string &fallback) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    return fallback;
  }
  if (!found->is_string()) {
    throw std::invalid_argument(std::string(key) + " must be a string");
  }
  return found->get<std::string>();
}

} // namespace

ReportOffer bindOffer(const ReportOfferRow &row,
                      const std::vector<TelemetryPoint> &points) {
  ReportOffer offer;
  offer.id = row.id;
  offer.reportName = row.reportName;
  offer.specifierId = row.specifierId;
  offer.enabled = row.enabled;
  offer.minSamplingSeconds = row.minSamplingSeconds;
  offer.maxSamplingSeconds = row.maxSamplingSeconds;
  offer.historySeconds = row.historySeconds;

  const std::string report = "report " + row.specifierId;
  if (!row.dataPoints.is_array()) {
    throw std::invalid_argument(report + ": data_points must be an array");
  }
  std::set<std::string> rids;
  for (const auto &entry : row.dataPoints) {
    if (!entry.is_object()) {
      throw std::invalid_argument(report + ": each data point must be an "
                                           "object");
    }
    ReportDataPoint point;
    point.pointId = requiredString(entry, "point_id", report + " data point");
    point.rid = optionalString(entry, "rid", point.pointId);
    const std::string where = report + " data point " + point.rid;
    if (point.rid.empty()) {
      throw std::invalid_argument(where + " needs a rid");
    }
    if (!rids.insert(point.rid).second) {
      throw std::invalid_argument(where + " is listed twice");
    }
    point.resourceId = requiredString(entry, "resource_id", where);
    point.reportType = optionalString(entry, "report_type", point.reportType);
    point.readingType =
        optionalString(entry, "reading_type", point.readingType);

    const TelemetryPoint *bound = nullptr;
    for (const auto &candidate : points) {
      if (candidate.pointId == point.pointId) {
        bound = &candidate;
        break;
      }
    }
    if (bound == nullptr) {
      throw std::invalid_argument(where + " names telemetry point " +
                                  point.pointId +
                                  ", which the connection does not have");
    }
    if (bound->item.kind == "state") {
      throw std::invalid_argument(where + " names telemetry point " +
                                  point.pointId +
                                  ", a state, which OpenADR reports do not "
                                  "carry");
    }
    point.item = bound->item;
    point.source = bound->source;
    offer.dataPoints.push_back(std::move(point));
  }
  return offer;
}

} // namespace grid_services::openadr2b
