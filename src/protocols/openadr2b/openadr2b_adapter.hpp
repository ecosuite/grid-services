#pragma once

#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <oadr/manager/reportmanager/ISendReport.h>
#include <oadr/manager/reportmanager/ReportManager.h>
#include <oadr/ven/IOadrMessage.h>
#include <oadr/ven/VEN2b.h>

#include "core/adapter.hpp"
#include "core/store.hpp"
#include "datum/datum.hpp"
#include "protocols/openadr2b/normalize.hpp"
#include "protocols/openadr2b/report_requests.hpp"
#include "protocols/openadr2b/report_service.hpp"
#include "protocols/openadr2b/reports.hpp"
#include "protocols/openadr2b/store.hpp"
#include "protocols/openadr2b/tick_scheduler.hpp"

namespace grid_services {
class SecretOpener;
}

namespace grid_services::openadr2b {

struct Openadr2bOptions {
  std::chrono::seconds connectTimeout{10};
  std::chrono::seconds requestTimeout{30};
};

// The reports a connection offers and where their values come from. With no
// reader, offered data points are reported as unreadable.
struct ReportSetup {
  std::vector<ReportOffer> offers;
  std::shared_ptr<datum::DatumReader> reader;
};

// OpenADR 2.0b over simple HTTP pull, built on liboadr's request layer.
//
// Event state lives in the database, not in memory: each distributeEvent is
// compared against the stored events, so a restart loses nothing. Report
// timing runs on the runner thread through a tick-driven scheduler.
class Openadr2bAdapter : public Adapter,
                         private ISendReport,
                         private IOadrMessage {
public:
  // `store` keeps the program's report requests. `clock` drives report
  // timing; tests replace it. Throws AdapterError when the offered reports
  // cannot be expressed in OpenADR.
  Openadr2bAdapter(ConnectionConfig config, Openadr2bSettings settings,
                   Openadr2bStore &store, std::unique_ptr<IHttp> http,
                   std::function<std::time_t()> clock = nullptr,
                   ReportSetup reports = {});
  ~Openadr2bAdapter() override;

  std::optional<RegistrationInfo> resume(const ConnectionState &state,
                                         ProgramEventSink &sink) override;
  RegistrationInfo registerConnection(ProgramEventSink &sink) override;
  void unregisterConnection(ProgramEventSink &sink) override;
  TestResult test(ProgramEventSink &sink) override;
  PollResult poll(ProgramEventSink &sink) override;
  void respond(const KnownEvent &event, Opt opt,
               ProgramEventSink &sink) override;
  void runDueWork(ProgramEventSink &sink) override;
  void setParticipation(Participation participation) override;
  std::optional<std::chrono::seconds> requestedPollInterval() const override;
  std::string describeClient(const std::string &clientId) const override;
  nlohmann::json takeExchange() override;

private:
  using Answer = std::pair<KnownEvent, Opt>;

  // ISendReport, called by liboadr's ReportManager.
  void sendUpdateReport(oadrUpdateReportType::oadrReport_sequence &sequence,
                        time_t dtstart, std::string reportRequestID,
                        time_t createdDateTime) override;
  void sendCreatedReport(const std::set<std::string> &pendingReports,
                         std::string requestID, std::string responseCode,
                         std::string responseDescription) override;
  void sendCanceledReport(const std::set<std::string> &pendingReports,
                          std::string requestID, std::string responseCode,
                          std::string responseDescription) override;
  void sendRegisterReport(oadrRegisterReportType::oadrReport_sequence &sequence,
                          std::string reportRequestID) override;

  // IOadrMessage, called by VEN2b for every message in and out.
  void OnOadrMessageReceived(std::string &message) override;
  void OnOadrMessageSent(std::string &message) override;

  ISendReport *reportSender() { return this; }
  // Starts an operation: a fresh exchange record, and the sink the report
  // service reports to.
  void begin(ProgramEventSink &sink);
  void recordMessage(const char *direction, const std::string &message);
  void syncAfterRegistration(ProgramEventSink &sink);
  void registerReports(ProgramEventSink &sink);
  // Answers a report request the VEN cannot serve, or hands it to the
  // report manager.
  void handleCreateReport(oadrCreateReportType &createReport);
  void requestEvents(ProgramEventSink &sink);
  // Stores and answers the events in a distribution, and returns a summary.
  std::string
  handleDistributeEvent(const oadr2b::oadr::oadrDistributeEventType &message,
                        ProgramEventSink &sink);
  // Why the VEN will not act on `event`, if it will not. Fills `normalized`
  // as far as the event allows.
  std::optional<Refusal> refusal(const oadr2b::oadr::oadrEvent &event,
                                 const std::string &requestId,
                                 const std::optional<KnownEvent> &known,
                                 NormalizedEvent &normalized) const;
  void sendAnswers(eventResponses::eventResponse_sequence &responses,
                   const std::vector<Answer> &answers,
                   ProgramEventSink &sink);
  void throwIfReportFailed();
  // Cancels the reports the program cancelled in its answers to them.
  void applyCancelsInAnswers();
  RegistrationInfo registrationInfo();
  PollResult cancelledByProgram(
      const oadr2b::oadr::oadrCancelPartyRegistrationType &cancel);
  PollResult reregistrationRequested();
  // Answers a report the program sent, which the VEN never asks for.
  PollResult
  reportFromProgram(const oadr2b::oadr::oadrUpdateReportType &update);
  // Answers a payload that names another VEN with 452, and says so, or
  // returns nothing when the payload is for this VEN (OpenADR 2.0b rule 21).
  // A payload that names no VEN is for this one.
  std::optional<PollResult>
  refusedForAnotherVen(const oadr2b::oadr::oadrSignedObject &message);
  // Drops the registration and everything exchanged under it.
  void forgetRegistration();
  // Drops every report request the program made and records them as
  // ended with `status`. The program asks again after it registers.
  void dropReportRequests(ProgramEventSink *sink, ReportRequestStatus status);

  ConnectionConfig config_;
  Openadr2bSettings settings_;
  std::function<std::time_t()> clock_;
  std::unique_ptr<VEN2b> ven_;
  TickScheduler reportScheduler_;
  ReportRequestLog reportRequests_;
  std::unique_ptr<ReportService> reportService_;
  std::unique_ptr<ReportManager> reportManager_;
  ProgramEventSink *sink_ = nullptr;

  bool needsReportRegistration_ = false;
  bool needsEventRequest_ = false;
  std::optional<std::string> reportFailure_;
  // Cancellations the program sent in its answers to reports, held until
  // the report manager is done sending.
  std::vector<oadrCancelReportType> cancelsInAnswers_;
  bool reportStopped_ = false;
  nlohmann::json exchange_ = nlohmann::json::array();
};

// Builds adapters for 'openadr2b' connections. `opener` opens sealed client
// keys; it may be null when no connection uses one.
AdapterFactory makeOpenadr2bFactory(const SecretOpener *opener,
                                    Openadr2bOptions options = {});

} // namespace grid_services::openadr2b
