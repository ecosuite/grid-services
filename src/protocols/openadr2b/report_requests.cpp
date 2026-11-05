#include "protocols/openadr2b/report_requests.hpp"

namespace grid_services::openadr2b {

namespace {

std::string describeSpan(int seconds) {
  if (seconds % 86400 == 0) {
    return std::to_string(seconds / 86400) + "d";
  }
  if (seconds % 3600 == 0) {
    return std::to_string(seconds / 3600) + "h";
  }
  if (seconds % 60 == 0) {
    return std::to_string(seconds / 60) + "m";
  }
  return std::to_string(seconds) + "s";
}

} // namespace

std::string describeRequest(const ReportRequest &request) {
  const std::string report = request.reportName + " (" + request.specifierId +
                             ", request " + request.programRequestId + ")";
  switch (request.kind) {
  case ReportRequestKind::OneShot:
    return "The program asked for " + report + " once";
  case ReportRequestKind::History:
    return "The program asked for the history of " + report + " over " +
           describeSpan(request.durationSeconds.value_or(0));
  case ReportRequestKind::Periodic:
    return "The program asked for " + report + " every " +
           describeSpan(request.reportBackSeconds) +
           (request.granularitySeconds > 0
                ? " at " + describeSpan(request.granularitySeconds) +
                      " intervals"
                : " on change") +
           (request.durationSeconds.value_or(0) > 0
                ? " for " + describeSpan(*request.durationSeconds)
                : " until cancelled");
  }
  return report;
}

void ReportRequestLog::activity(std::string message) {
  if (sink_ != nullptr) {
    sink_->activity({.category = ActivityCategory::Reports,
                     .message = std::move(message)});
  }
}

void ReportRequestLog::requested(const ReportRequest &request) {
  store_.recordReportRequest(request);
  activity(describeRequest(request));
}

void ReportRequestLog::ended(const std::string &programRequestId,
                             ReportRequestStatus status) {
  store_.endReportRequest(programRequestId, status);
  if (status == ReportRequestStatus::Cancelled) {
    activity("The program cancelled report request " + programRequestId);
  }
}

void ReportRequestLog::allEnded(ReportRequestStatus status) {
  store_.endAllReportRequests(status);
}

} // namespace grid_services::openadr2b
