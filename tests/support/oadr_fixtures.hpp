#pragma once

#include <string>
#include <utility>
#include <vector>

// OpenADR 2.0b payloads for tests, written against the vendored schema.
namespace grid_services::testing::fixtures {

// An event signal beyond the event's first.
struct SignalSpec {
  std::string name;
  std::string type;
  // An itemBase element name, such as "powerReal"; empty for none.
  std::string units;
  // (duration, value) per interval.
  std::vector<std::pair<std::string, double>> intervals = {{"PT3H", 1.0}};
};

struct EventSpec {
  std::string id = "event-1";
  int modification = 0;
  std::string status = "far";
  std::string start = "2030-07-14T21:00:00Z";
  std::string duration = "PT3H";
  // (duration, value) per interval.
  std::vector<std::pair<std::string, double>> intervals = {{"PT3H", 1.0}};
  std::string signalName = "simple";
  std::string signalType = "level";
  // The first signal's itemBase element name, such as "powerReal"; empty for
  // none.
  std::string units;
  std::vector<SignalSpec> moreSignals;
  std::string responseRequired = "always";
  std::vector<std::string> venIds = {"ven-1"};
  std::string testEvent = "false";
  std::string comment;
  std::string marketContext = "http://market.example.com";
  // False leaves the intervals without a signal payload, making the event
  // invalid.
  bool floatPayload = true;
  // The startafter randomization window, such as "PT10M"; empty for none.
  std::string startAfter;
};

// A report request. An empty `start` leaves out the reportInterval.
struct ReportRequestSpec {
  std::string reportRequestId = "rr-1";
  std::string reportSpecifierId = "TELEMETRY_STATUS";
  std::string rid = "status";
  std::string backDuration = "PT1M";
  std::string start;
  std::string granularity = "PT1M";
  // The reportInterval duration, when `start` is set.
  std::string duration = "PT0S";
  // Further rIDs after `rid`.
  std::vector<std::string> moreRids;
};

std::string distributeEvent(const std::string &requestId,
                            const std::vector<EventSpec> &events,
                            const std::string &vtnId = "vtn-1");
std::string createdPartyRegistration(const std::string &venId,
                                     const std::string &registrationId,
                                     const std::string &pollFrequency = "PT10S",
                                     const std::string &code = "200");
// A registration query answer for a VEN the VTN does not know.
std::string createdPartyRegistrationUnknown();
std::string response(const std::string &code = "200");
// `piggyback` report requests ride along in the response.
std::string
registeredReport(const std::vector<ReportRequestSpec> &piggyback = {});
std::string canceledPartyRegistration(const std::string &code = "200");
// The VTN's answer to an oadrUpdateReport. A `cancelReportRequestId` asks
// the VEN to stop that report in the same answer.
std::string updatedReport(const std::string &code = "200",
                          const std::string &description = "OK",
                          const std::string &cancelReportRequestId = "");
std::string
cancelPartyRegistration(const std::string &registrationId = "reg-1");
std::string requestReregistration(const std::string &venId = "ven-1");
// The VTN's answer to an oadrCreateOpt.
std::string createdOpt(const std::string &code = "200");
// A periodic TELEMETRY_STATUS report request for the "status" rID.
std::string createStatusReport(const std::string &reportRequestId,
                               const std::string &start,
                               const std::string &backDuration = "PT1M");
// A non-empty `venId` is included as the request's venID.
std::string createReport(const std::vector<ReportRequestSpec> &requests,
                         const std::string &requestId = "create-report-1",
                         const std::string &venId = "");
std::string cancelReport(const std::string &reportRequestId,
                         bool reportToFollow = false,
                         const std::string &venId = "ven-1");
// The VTN's own Metadata, with no reports to offer.
std::string registerReport(const std::string &venId = "ven-1");
// A report the VTN sends the VEN, for a request the VEN never made.
std::string updateReport(const std::string &requestId = "update-report-1");

} // namespace grid_services::testing::fixtures
