#pragma once

#include <string>

#include "core/adapter.hpp"
#include "protocols/openadr2b/reports.hpp"
#include "protocols/openadr2b/store.hpp"

namespace grid_services::openadr2b {

// Records the report requests the program makes, and tells the operator
// about them through the sink of the operation in progress.
class ReportRequestLog {
public:
  explicit ReportRequestLog(Openadr2bStore &store) : store_(store) {}

  void setSink(ProgramEventSink *sink) { sink_ = sink; }

  // The program asked for a report.
  void requested(const ReportRequest &request);

  // A report request was cancelled by the program, or ran its course.
  void ended(const std::string &programRequestId, ReportRequestStatus status);

  // Every report request ended: the registration is gone, or the reports
  // were registered afresh.
  void allEnded(ReportRequestStatus status);

private:
  void activity(std::string message);

  Openadr2bStore &store_;
  ProgramEventSink *sink_ = nullptr;
};

// "The program asked for TELEMETRY_USAGE (usage-1, request rr-1) every 5m
// at 1m intervals until cancelled", for the activity log.
std::string describeRequest(const ReportRequest &request);

} // namespace grid_services::openadr2b
