#include "protocols/openadr2b/openadr2b_adapter.hpp"

#include <algorithm>
#include <ctime>
#include <set>

#include <oadr/request/Oadr2bHelper.h>

#include "protocols/openadr2b/curl_transport.hpp"
#include "core/errors.hpp"
#include "crypto/client_credential.hpp"
#include "crypto/sealed_box.hpp"
#include "datum/solarnetwork_client.hpp"

namespace grid_services::openadr2b {

namespace {

// Wraps liboadr calls so that anything other than an AdapterError, such as
// an XML parsing failure, surfaces as a transient error.
template <typename Fn> auto guarded(const char *what, Fn &&fn) {
  try {
    return fn();
  } catch (const AdapterError &) {
    throw;
  } catch (const std::exception &error) {
    throw AdapterError::transient(std::string(what) + ": " + error.what());
  }
}

void ensureHttpOk(Oadr2bRequest &request, const char *what) {
  const auto code = request.httpResponseCode();
  if (code == "200") {
    return;
  }
  const auto message = std::string(what) + " failed with HTTP " + code;
  if (code == "401" || code == "403") {
    throw AdapterError::permanent(message +
                                  "; the VTN rejected our credentials");
  }
  // Neither may be sent again unchanged (OpenADR 2.0b section 9.1.5).
  if (code == "404") {
    throw AdapterError::permanent(message +
                                  "; the VTN does not support the request");
  }
  if (code == "406") {
    throw AdapterError::permanent(message +
                                  "; the VTN cannot accept the request");
  }
  throw AdapterError::transient(message);
}

std::string describe(const oadr2b::ei::EiResponseType &response) {
  std::string text = response.responseCode();
  if (response.responseDescription()) {
    text += " " + std::string(*response.responseDescription());
  }
  return text;
}

oadr2b::ei::OptTypeType::value toOptType(Opt opt) {
  return opt == Opt::In ? oadr2b::ei::OptTypeType::optIn
                        : oadr2b::ei::OptTypeType::optOut;
}

// Sends a request whose delivery the program confirms with an eiResponse.
// Returns why it was not delivered, or nothing when it was. `confirmation`
// finds the eiResponse in the program's reply, or returns null.
template <typename Send, typename Confirmation>
std::string deliveryFailure(Send &&send, Confirmation &&confirmation) {
  try {
    auto request = send();
    if (request->httpResponseCode() != "200") {
      return "HTTP " + request->httpResponseCode();
    }
    if (auto *response = request->response(); response != nullptr) {
      const oadr2b::ei::EiResponseType *ei =
          confirmation(response->oadrSignedObject());
      if (ei != nullptr && ei->responseCode() != "200") {
        return "the VTN answered " + describe(*ei);
      }
    }
    return "";
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Stopped) {
      throw;
    }
    return error.what();
  }
}

ProgramStatus endedStatus(const KnownEvent &event) {
  const auto end = event.startAt + std::chrono::seconds(event.durationSeconds);
  return event.durationSeconds > 0 && now() >= end ? ProgramStatus::Completed
                                                   : ProgramStatus::Cancelled;
}

const oadr2b::ei::EiResponseType *
responseConfirmation(const oadr2b::oadr::oadrSignedObject &reply) {
  return reply.oadrResponse().present() ? &reply.oadrResponse()->eiResponse()
                                        : nullptr;
}

// Appended to a summary when an acknowledgement did not reach the program.
std::string unacknowledged(const std::string &failure) {
  return failure.empty() ? "" : " (acknowledging it failed: " + failure + ")";
}

std::string withoutTrailingSlash(std::string url) {
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

} // namespace

Openadr2bAdapter::Openadr2bAdapter(ConnectionConfig config,
                                   Openadr2bSettings settings,
                                   Openadr2bStore &store,
                                   std::unique_ptr<IHttp> http,
                                   std::function<std::time_t()> clock,
                                   ReportSetup reports)
    : config_(std::move(config)), settings_(std::move(settings)),
      clock_(clock ? std::move(clock)
                   : [] { return std::time(nullptr); }),
      ven_(std::make_unique<VEN2b>(std::move(http),
                                   withoutTrailingSlash(settings_.vtnUrl),
                                   settings_.venName,
                                   settings_.venId.value_or(""), "")),
      reportRequests_(store) {
  try {
    reportService_ = std::make_unique<ReportService>(
        settings_.venName, settings_.marketContext, std::move(reports.offers),
        std::move(reports.reader), reportRequests_, clock_);
  } catch (const std::invalid_argument &error) {
    throw AdapterError::permanent(error.what());
  }
  reportManager_ = std::make_unique<ReportManager>(
      &reportScheduler_, reportService_.get(), reportSender());
  ven_->setOadrMessage(this);
}

Openadr2bAdapter::~Openadr2bAdapter() = default;

// ---------------------------------------------------------------------------
// Exchange capture

void Openadr2bAdapter::begin(ProgramEventSink &sink) {
  exchange_ = nlohmann::json::array();
  sink_ = &sink;
  reportService_->setSink(&sink);
  reportRequests_.setSink(&sink);
}

void Openadr2bAdapter::recordMessage(const char *direction,
                                     const std::string &message) {
  exchange_.push_back(
      {{"direction", direction},
       {"at", toIso8601(std::chrono::system_clock::now())},
       {"format", "xml"},
       {"body", message}});
}

void Openadr2bAdapter::OnOadrMessageSent(std::string &message) {
  recordMessage("to_program", message);
}

void Openadr2bAdapter::OnOadrMessageReceived(std::string &message) {
  recordMessage("from_program", message);
}

nlohmann::json Openadr2bAdapter::takeExchange() {
  if (exchange_.empty()) {
    return nullptr;
  }
  nlohmann::json taken = std::move(exchange_);
  exchange_ = nlohmann::json::array();
  return taken;
}

// ---------------------------------------------------------------------------
// Registration

RegistrationInfo Openadr2bAdapter::registrationInfo() {
  RegistrationInfo info{ven_->venID(), ven_->registrationID(), std::nullopt};
  if (const auto seconds = ven_->pollFrequencyInSeconds(); seconds > 0) {
    info.pollIntervalSeconds = static_cast<int>(seconds);
  }
  return info;
}

std::optional<RegistrationInfo>
Openadr2bAdapter::resume(const ConnectionState &, ProgramEventSink &sink) {
  return guarded("registration query", [&]() -> std::optional<RegistrationInfo> {
    begin(sink);
    auto request = ven_->queryRegistration();
    ensureHttpOk(*request, "Registration query");

    // Decide from the answer itself: VEN2b keeps a registration it learned
    // earlier even when the VTN now says it has none.
    auto *response = request->response();
    if (response == nullptr ||
        !response->oadrSignedObject().oadrCreatedPartyRegistration().present()) {
      throw AdapterError::transient("unexpected response to registration query");
    }
    const auto &created =
        response->oadrSignedObject().oadrCreatedPartyRegistration().get();
    const bool known = created.eiResponse().responseCode() == "200" &&
                       created.registrationID() &&
                       !created.registrationID()->empty() && created.venID() &&
                       !created.venID()->empty();
    if (!known) {
      ven_->clearRegistration();
      return std::nullopt;
    }
    // Reports and events are re-established on the next poll, since the
    // VTN's report requests were lost with the previous process.
    needsReportRegistration_ = true;
    needsEventRequest_ = true;
    return registrationInfo();
  });
}

RegistrationInfo Openadr2bAdapter::registerConnection(ProgramEventSink &sink) {
  return guarded("registration", [&] {
    begin(sink);
    auto request = ven_->createPartyRegistration(
        oadrProfileType(oadrProfileType::cxx_2_0b),
        oadrTransportType(oadrTransportType::simpleHttp), "", false, false,
        true);
    ensureHttpOk(*request, "Registration");

    auto *response = request->response();
    if (response == nullptr ||
        !response->oadrSignedObject().oadrCreatedPartyRegistration().present()) {
      throw AdapterError::transient("unexpected response to registration");
    }
    const auto &created =
        response->oadrSignedObject().oadrCreatedPartyRegistration().get();
    if (created.eiResponse().responseCode() != "200") {
      throw AdapterError::permanent("the VTN rejected the registration: " +
                                    describe(created.eiResponse()));
    }

    needsReportRegistration_ = true;
    needsEventRequest_ = true;
    return registrationInfo();
  });
}

void Openadr2bAdapter::unregisterConnection(ProgramEventSink &sink) {
  guarded("cancel registration", [&] {
    begin(sink);
    auto request = ven_->cancelPartyRegistration();
    ensureHttpOk(*request, "Cancel registration");

    auto *response = request->response();
    if (response == nullptr ||
        !response->oadrSignedObject().oadrCanceledPartyRegistration().present()) {
      throw AdapterError::transient(
          "unexpected response to cancel registration");
    }
    const auto &canceled =
        response->oadrSignedObject().oadrCanceledPartyRegistration().get();
    const std::string code = canceled.eiResponse().responseCode();
    // 453 means the VTN did not know the registration, which is the outcome
    // we wanted anyway.
    if (code != "200" && code != "453") {
      throw AdapterError::transient("the VTN refused to cancel: " +
                                    describe(canceled.eiResponse()));
    }
    forgetRegistration();
  });
}

void Openadr2bAdapter::forgetRegistration() {
  ven_->clearRegistration();
  needsReportRegistration_ = false;
  needsEventRequest_ = false;
  // The program's report requests went with the registration (rule 407).
  dropReportRequests(sink_, ReportRequestStatus::Cancelled);
}

void Openadr2bAdapter::dropReportRequests(ProgramEventSink *sink,
                                          ReportRequestStatus status) {
  reportManager_.reset();
  reportScheduler_.clear();
  cancelsInAnswers_.clear();
  reportManager_ = std::make_unique<ReportManager>(
      &reportScheduler_, reportService_.get(), reportSender());
  if (sink != nullptr) {
    reportRequests_.allEnded(status);
  }
}

PollResult Openadr2bAdapter::cancelledByProgram(
    const oadr2b::oadr::oadrCancelPartyRegistrationType &cancel) {
  const std::string requestId = cancel.requestID();
  const std::string registrationId = cancel.registrationID();
  const auto answer = [&](const std::string &code,
                          const std::string &description) {
    return deliveryFailure(
        [&] {
          return ven_->canceledPartyRegistration(registrationId, code,
                                                 description, requestId);
        },
        responseConfirmation);
  };

  // Only our own registration can be cancelled (OpenADR 2.0b rule 400).
  const bool ours = registrationId == ven_->registrationID() &&
                    (!cancel.venID() || *cancel.venID() == ven_->venID());
  if (!ours) {
    const auto failure = answer("452", "Not this VEN's registration");
    return {PollOutcome::Ok,
            "Refused the program's cancellation of registration " +
                registrationId + ", which is not this VEN's" +
                unacknowledged(failure),
            true};
  }

  // The program has cancelled whatever we answer, so a failed answer does
  // not keep the registration. Reports go with it (rule 407).
  const auto failure = answer("200", "OK");
  forgetRegistration();
  return {PollOutcome::RegistrationCancelled,
          "The program cancelled the registration" + unacknowledged(failure)};
}

PollResult Openadr2bAdapter::reregistrationRequested() {
  // The registration is kept: registering again references it (rule 402).
  const auto failure =
      deliveryFailure([&] { return ven_->reregistrationResponse("200", "OK"); },
                      responseConfirmation);
  return {PollOutcome::ReregistrationRequested,
          "The program requested re-registration" + unacknowledged(failure)};
}

TestResult Openadr2bAdapter::test(ProgramEventSink &sink) {
  return guarded("connection test", [&]() -> TestResult {
    begin(sink);
    auto request = ven_->queryRegistration();
    ensureHttpOk(*request, "Registration query");

    auto *response = request->response();
    if (response == nullptr ||
        !response->oadrSignedObject().oadrCreatedPartyRegistration().present()) {
      throw AdapterError::transient("unexpected response to registration query");
    }
    const auto &created =
        response->oadrSignedObject().oadrCreatedPartyRegistration().get();
    if (created.eiResponse().responseCode() != "200") {
      throw AdapterError::transient("the VTN answered " +
                                    describe(created.eiResponse()));
    }

    TestResult result;
    result.description = "reached VTN " + std::string(created.vtnID());
    if (created.registrationID() && !created.registrationID()->empty()) {
      result.registered = true;
      result.description += "; it has this VEN registered";
      if (created.venID()) {
        result.clientId = std::string(*created.venID());
        result.description += " as " + *result.clientId;
      }
    } else {
      result.description += "; it has no registration for this VEN";
    }
    return result;
  });
}

// ---------------------------------------------------------------------------
// Polling and events

void Openadr2bAdapter::syncAfterRegistration(ProgramEventSink &sink) {
  if (needsReportRegistration_) {
    registerReports(sink);
    needsReportRegistration_ = false;
  }
  if (needsEventRequest_) {
    requestEvents(sink);
    needsEventRequest_ = false;
  }
}

void Openadr2bAdapter::registerReports(ProgramEventSink &sink) {
  // Whatever the program asked for before is not served any more. It asks
  // again once it sees the new registration.
  dropReportRequests(&sink, ReportRequestStatus::Completed);

  oadrRegisterReportType::oadrReport_sequence sequence;
  reportService_->OnGenerateRegisterReport(sequence);
  reportManager_->setRegisteredReports(sequence);
  auto request = ven_->registerReport(sequence);
  ensureHttpOk(*request, "Report registration");

  // The VTN may request reports in its answer (OpenADR 2.0b rule 344).
  auto *response = request->response();
  if (response == nullptr ||
      !response->oadrSignedObject().oadrRegisteredReport().present()) {
    return;
  }
  const auto &registered =
      response->oadrSignedObject().oadrRegisteredReport().get();
  if (registered.oadrReportRequest().empty()) {
    return;
  }
  oadrCreateReportType createReport(registered.eiResponse().requestID());
  createReport.oadrReportRequest(registered.oadrReportRequest());
  handleCreateReport(createReport);
}

void Openadr2bAdapter::handleCreateReport(oadrCreateReportType &createReport) {
  // A request the VEN cannot serve is refused as a whole, with the code
  // that says why, before the report manager sees it (rules 340 and 452).
  for (const auto &request : createReport.oadrReportRequest()) {
    if (const auto why = reportService_->refusal(request)) {
      sendCreatedReport(reportManager_->getPendingReports(),
                        createReport.requestID(), why->first, why->second);
      throwIfReportFailed();
      return;
    }
  }
  // A request for Metadata ends the requests made before it, which the
  // program cancels on receiving the Metadata (rule 306).
  const bool metadata = std::any_of(
      createReport.oadrReportRequest().begin(),
      createReport.oadrReportRequest().end(), [](const auto &request) {
        return request.reportSpecifier().reportSpecifierID() == "METADATA";
      });
  reportService_->setCancelling(metadata);
  reportManager_->manageCreateReport(createReport);
  reportService_->setCancelling(false);
  applyCancelsInAnswers();
  throwIfReportFailed();
}

void Openadr2bAdapter::applyCancelsInAnswers() {
  auto cancels = std::move(cancelsInAnswers_);
  cancelsInAnswers_.clear();
  reportService_->setCancelling(true);
  for (const auto &cancel : cancels) {
    reportManager_->manageUpdatedReportCancel(cancel);
  }
  reportService_->setCancelling(false);
}

void Openadr2bAdapter::requestEvents(ProgramEventSink &sink) {
  auto request = ven_->requestEvent();
  ensureHttpOk(*request, "Event request");
  auto *response = request->response();
  if (response != nullptr &&
      response->oadrSignedObject().oadrDistributeEvent().present()) {
    handleDistributeEvent(
        response->oadrSignedObject().oadrDistributeEvent().get(), sink);
  }
}

PollResult Openadr2bAdapter::poll(ProgramEventSink &sink) {
  return guarded("poll", [&]() -> PollResult {
    begin(sink);
    syncAfterRegistration(sink);

    auto request = ven_->poll();
    ensureHttpOk(*request, "Poll");
    auto *payload = request->response();
    if (payload == nullptr) {
      throw AdapterError::transient("empty response to poll");
    }
    const auto &message = payload->oadrSignedObject();

    if (auto refused = refusedForAnotherVen(message)) {
      return *refused;
    }

    if (message.oadrResponse().present()) {
      const auto &response = message.oadrResponse()->eiResponse();
      if (response.responseCode() != "200") {
        throw AdapterError::transient("the VTN answered the poll with " +
                                      describe(response));
      }
      return {};
    }

    if (message.oadrDistributeEvent().present()) {
      return {PollOutcome::Ok,
              handleDistributeEvent(message.oadrDistributeEvent().get(), sink),
              true};
    }

    if (message.oadrCreateReport().present()) {
      auto createReport = message.oadrCreateReport().get();
      handleCreateReport(createReport);
      return {PollOutcome::Ok, "The program requested reports", true};
    }

    if (message.oadrRegisterReport().present()) {
      auto registered = ven_->registeredReport(
          message.oadrRegisterReport()->requestID(), "200", "OK");
      ensureHttpOk(*registered, "Registered report");
      return {PollOutcome::Ok, "The program registered its reports", true};
    }

    if (message.oadrCancelReport().present()) {
      auto cancelReport = message.oadrCancelReport().get();
      reportService_->setCancelling(true);
      reportManager_->manageCancelReport(cancelReport);
      reportService_->setCancelling(false);
      throwIfReportFailed();
      return {PollOutcome::Ok, "The program cancelled reports", true};
    }

    if (message.oadrUpdateReport().present()) {
      return reportFromProgram(*message.oadrUpdateReport());
    }

    if (message.oadrCancelPartyRegistration().present()) {
      return cancelledByProgram(*message.oadrCancelPartyRegistration());
    }

    if (message.oadrRequestReregistration().present()) {
      return reregistrationRequested();
    }

    return {PollOutcome::Ok, "Ignored an unsupported message from the program",
            true};
  });
}

PollResult Openadr2bAdapter::reportFromProgram(
    const oadr2b::oadr::oadrUpdateReportType &update) {
  // The VEN requests no reports, so any report the program sends answers a
  // request it did not make (rules 304 and 500).
  const bool unrequested = !update.oadrReport().empty();
  const auto failure = deliveryFailure(
      [&] {
        return ven_->updatedReport(
            update.requestID(), unrequested ? "452" : "200",
            unrequested ? "This VEN requested no reports" : "OK");
      },
      responseConfirmation);
  return {PollOutcome::Ok,
          std::string(unrequested ? "Refused a report from the program, which "
                                    "this VEN did not request"
                                  : "The program sent an empty report") +
              unacknowledged(failure),
          true};
}

std::optional<PollResult> Openadr2bAdapter::refusedForAnotherVen(
    const oadr2b::oadr::oadrSignedObject &message) {
  const std::string ours = ven_->venID();
  if (ours.empty()) {
    return std::nullopt;
  }
  const auto another = [&](const auto &venId) {
    return venId.present() && std::string(*venId) != ours;
  };
  const std::string why = "Not this VEN's venID";
  const auto refused = [&](const std::string &what, const std::string &venId,
                           const std::string &failure) {
    return PollResult{PollOutcome::Ok,
                      "Refused the program's " + what + " for VEN " + venId +
                          ", which is not this VEN" + unacknowledged(failure),
                      true};
  };

  if (message.oadrRequestReregistration().present()) {
    const std::string venId = message.oadrRequestReregistration()->venID();
    if (venId == ours) {
      return std::nullopt;
    }
    const auto failure = deliveryFailure(
        [&] { return ven_->reregistrationResponse("452", why); },
        responseConfirmation);
    return refused("re-registration request", venId, failure);
  }
  if (message.oadrCreateReport().present() &&
      another(message.oadrCreateReport()->venID())) {
    const auto &request = *message.oadrCreateReport();
    sendCreatedReport(reportManager_->getPendingReports(), request.requestID(),
                      "452", why);
    throwIfReportFailed();
    return refused("report request", *request.venID(), "");
  }
  if (message.oadrCancelReport().present() &&
      another(message.oadrCancelReport()->venID())) {
    const auto &cancel = *message.oadrCancelReport();
    sendCanceledReport(reportManager_->getPendingReports(), cancel.requestID(),
                       "452", why);
    throwIfReportFailed();
    return refused("report cancellation", *cancel.venID(), "");
  }
  if (message.oadrRegisterReport().present() &&
      another(message.oadrRegisterReport()->venID())) {
    const auto &registration = *message.oadrRegisterReport();
    auto answer =
        ven_->registeredReport(registration.requestID(), "452", why);
    ensureHttpOk(*answer, "Registered report");
    return refused("report registration", *registration.venID(), "");
  }
  return std::nullopt;
}

std::string Openadr2bAdapter::handleDistributeEvent(
    const oadr2b::oadr::oadrDistributeEventType &message,
    ProgramEventSink &sink) {
  const std::string requestId = message.requestID();

  // A distribution from a VTN other than ours is refused as a whole
  // (OpenADR 2.0b rules 21 and 33).
  const std::string vtnId = message.vtnID();
  if (!ven_->vtnID().empty() && vtnId != ven_->vtnID()) {
    eventResponses::eventResponse_sequence none;
    const auto failure = deliveryFailure(
        [&] {
          return ven_->createdEvent("452", "Unexpected vtnID", none, requestId);
        },
        responseConfirmation);
    return "Refused events from VTN " + vtnId +
           ", which this VEN is not "
           "registered with" +
           unacknowledged(failure);
  }

  eventResponses::eventResponse_sequence responses;
  std::vector<Answer> answers;
  std::set<std::string> listed;
  int changes = 0;
  int refused = 0;

  for (const auto &event : message.oadrEvent()) {
    const auto &descriptor = event.eiEvent().eventDescriptor();
    const std::string eventId = descriptor.eventID();
    const auto modification =
        static_cast<unsigned int>(descriptor.modificationNumber());
    const bool responseRequired = event.oadrResponseRequired() ==
                                  oadr2b::oadr::ResponseRequiredType::always;
    const auto known = sink.knownEvent(eventId);

    NormalizedEvent normalized;
    if (const auto why = refusal(event, requestId, known, normalized)) {
      // Refused events are not stored. A stale modification leaves the
      // stored event as it is; any other refusal of a known event ends it
      // below, since the program's current version cannot be acted on.
      ++refused;
      if (why->code == "450") {
        listed.insert(eventId);
      }
      if (responseRequired) {
        // A logical error is answered with optOut (rule 48).
        Oadr2bHelper::appendEventResponse(responses, why->code, why->reason,
                                          eventId, modification,
                                          toOptType(Opt::Out), requestId);
      }
      continue;
    }
    listed.insert(eventId);

    const bool cancelled = normalized.status == ProgramStatus::Cancelled;
    if (cancelled && !known) {
      // Nothing to cancel, but the cancellation is still confirmed (rules
      // 21 and 60).
      if (responseRequired) {
        Oadr2bHelper::appendEventResponse(responses, "200", "OK", eventId,
                                          modification, toOptType(Opt::In),
                                          requestId);
      }
      continue;
    }

    const auto update = sink.eventReceived(normalized);
    if (update.changed) {
      ++changes;
    }
    // Every event is answered each time it is delivered, changed or not
    // (rules 12 and 62), and a cancellation is always confirmed (rule 36).
    if (responseRequired) {
      const Opt opt = cancelled ? Opt::In : update.event.opt;
      Oadr2bHelper::appendEventResponse(responses, "200", "OK", eventId,
                                        modification, toOptType(opt),
                                        requestId);
      answers.emplace_back(update.event, opt);
    }
  }

  // A distributeEvent lists every open event for the VEN, so an open event
  // that is missing has ended.
  for (const auto &open : sink.openEvents()) {
    if (listed.count(open.programEventId) == 0) {
      sink.eventEnded(open, endedStatus(open));
      ++changes;
    }
  }

  if (!responses.empty()) {
    sendAnswers(responses, answers, sink);
  }
  return "Received events from the program (" + std::to_string(changes) +
         " changed" +
         (refused > 0 ? ", " + std::to_string(refused) + " refused" : "") + ")";
}

std::optional<Refusal> Openadr2bAdapter::refusal(
    const oadr2b::oadr::oadrEvent &event, const std::string &requestId,
    const std::optional<KnownEvent> &known, NormalizedEvent &normalized) const {
  try {
    normalized = normalizeEvent(event, requestId);
  } catch (const std::exception &error) {
    // Missing expected information (rule 33).
    return Refusal{"459", error.what()};
  }

  // An out-of-order modification is an error whatever its status (rule 58).
  if (known && known->modificationNumber > normalized.modificationNumber) {
    return Refusal{"450", "modification " +
                              std::to_string(normalized.modificationNumber) +
                              " is older than modification " +
                              std::to_string(known->modificationNumber)};
  }
  // A cancellation needs no further checks: there is nothing to act on.
  if (normalized.status == ProgramStatus::Cancelled) {
    return std::nullopt;
  }

  // Rule 48 and 512.
  if (!targetsVen(event, ven_->venID())) {
    return Refusal{"462", "not targeted at this VEN"};
  }
  if (settings_.marketContext && normalized.marketContext &&
      !sameMarketContext(*normalized.marketContext, *settings_.marketContext)) {
    return Refusal{"462", "market context " + *normalized.marketContext +
                              " is not this VEN's"};
  }
  // Rules 48 and 109.
  if (auto why = unsupportedSignal(event)) {
    return why;
  }
  if (auto why = unacceptedSignal(normalized, settings_.acceptedSignals)) {
    return why;
  }
  // Rules 9 and 48.
  if (auto why = outOfRangeSignal(normalized)) {
    return why;
  }
  // Rules 8 and 33.
  for (const auto &signal : normalized.signals) {
    int total = 0;
    for (const auto &interval : signal.intervals) {
      total += interval.durationSeconds;
    }
    if (total != normalized.durationSeconds) {
      return Refusal{"454", "signal " + signal.name + " intervals last " +
                                std::to_string(total) + "s, the event " +
                                std::to_string(normalized.durationSeconds) +
                                "s"};
    }
  }

  // The event may run until the end of its randomization window (rule 18).
  const auto effectiveEnd =
      normalized.startAt +
      std::chrono::seconds(normalized.durationSeconds +
                           normalized.startAfterSeconds.value_or(0));
  const bool ended = normalized.durationSeconds > 0 && now() >= effectiveEnd;
  // Old date on a new event (rule 33).
  if (!known && ended) {
    return Refusal{"454", "the event has already ended"};
  }
  // The program may call an event completed while its randomization window
  // is still open; the VEN keeps running it (rule 66).
  if (normalized.status == ProgramStatus::Completed &&
      normalized.startAfterSeconds.value_or(0) > 0 && !ended) {
    normalized.status = ProgramStatus::Active;
  }
  return std::nullopt;
}

void Openadr2bAdapter::sendAnswers(
    eventResponses::eventResponse_sequence &responses,
    const std::vector<Answer> &answers, ProgramEventSink &sink) {
  const auto failure = deliveryFailure(
      [&] { return ven_->createdEvent("200", "OK", responses); },
      responseConfirmation);

  for (const auto &[event, opt] : answers) {
    sink.responseSent(event, optAnswer(opt), event.modificationNumber,
                      failure.empty(), failure);
  }

  if (!failure.empty()) {
    // Ask for the full event list next time so unanswered events are
    // answered again.
    needsEventRequest_ = true;
    throw AdapterError::transient("opt answers were not delivered: " + failure);
  }
}

void Openadr2bAdapter::respond(const KnownEvent &event, Opt opt,
                               ProgramEventSink &sink) {
  // The program takes no answer for this event, through EiEvent or EiOpt
  // (OpenADR 2.0b rules 12 and 206).
  if (!event.responseRequired) {
    return;
  }
  guarded("opt change", [&] {
    begin(sink);
    // The event was answered when it arrived, so a change qualifies that
    // answer through EiOpt (rules 206 and 510). The whole VEN is targeted.
    const auto failure = deliveryFailure(
        [&] {
          return ven_->createOptEvent(
              "opt-" + event.programEventId, toOptType(opt),
              OptReasonValue(opt == Opt::In
                                 ? OptReasonEnumeratedType::participating
                                 : OptReasonEnumeratedType::notParticipating),
              event.programEventId,
              static_cast<unsigned int>(event.modificationNumber));
        },
        [](const oadr2b::oadr::oadrSignedObject &reply)
            -> const oadr2b::ei::EiResponseType * {
          return reply.oadrCreatedOpt().present()
                     ? &reply.oadrCreatedOpt()->eiResponse()
                     : nullptr;
        });
    sink.responseSent(event, optAnswer(opt), event.modificationNumber,
                      failure.empty(), failure);
    if (!failure.empty()) {
      throw AdapterError::transient("opt change was not delivered: " + failure);
    }
  });
}

// ---------------------------------------------------------------------------
// Reports

void Openadr2bAdapter::runDueWork(ProgramEventSink &sink) {
  guarded("reports", [&] {
    begin(sink);
    reportScheduler_.runDue(clock_());
    applyCancelsInAnswers();
    throwIfReportFailed();
  });
}

void Openadr2bAdapter::throwIfReportFailed() {
  if (reportStopped_) {
    reportStopped_ = false;
    reportFailure_.reset();
    throw AdapterError::stopped();
  }
  if (reportFailure_) {
    const auto message = *reportFailure_;
    reportFailure_.reset();
    throw AdapterError::transient(message);
  }
}

namespace {

template <typename Fn>
void sendReportMessage(const char *what, std::optional<std::string> &failure,
                       bool &stopped, Fn &&send) {
  try {
    auto request = send();
    if (request->httpResponseCode() != "200") {
      failure = std::string(what) + " failed with HTTP " +
                request->httpResponseCode();
    }
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Stopped) {
      stopped = true;
    } else {
      failure = std::string(what) + " failed: " + error.what();
    }
  } catch (const std::exception &error) {
    failure = std::string(what) + " failed: " + error.what();
  }
}

} // namespace

void Openadr2bAdapter::sendUpdateReport(
    oadrUpdateReportType::oadrReport_sequence &sequence, time_t dtstart,
    std::string reportRequestID, time_t createdDateTime) {
  ReportDelivery delivery;
  delivery.programRequestId = reportRequestID;
  if (auto generated = reportService_->takeGenerated(reportRequestID)) {
    delivery.payload = std::move(generated->payload);
    delivery.reads = std::move(generated->reads);
    delivery.intervalStart = generated->intervalStart;
    delivery.intervalEnd = generated->intervalEnd;
  }

  if (sequence.empty()) {
    delivery.message = "nothing to report";
  } else {
    std::optional<std::string> failure;
    bool stopped = false;
    sendReportMessage("Report update", failure, stopped, [&] {
      auto request = ven_->updateReport(sequence, dtstart, reportRequestID,
                                        createdDateTime);
      if (auto *response = request->response(); response != nullptr &&
          response->oadrSignedObject().oadrUpdatedReport().present()) {
        const auto &updated = *response->oadrSignedObject().oadrUpdatedReport();
        // The program may cancel reports in its answer (rule 328). The
        // report manager is still sending this one, so the cancellation
        // waits until it returns.
        if (updated.oadrCancelReport().present()) {
          cancelsInAnswers_.push_back(*updated.oadrCancelReport());
        }
        const auto &ei = updated.eiResponse();
        delivery.responseCode = std::string(ei.responseCode());
        if (ei.responseCode() != "200") {
          failure = "the VTN answered " + describe(ei);
        }
      }
      return request;
    });
    if (stopped) {
      reportStopped_ = true;
      return;
    }
    delivery.ok = !failure.has_value();
    delivery.message = failure.value_or("");
    if (failure) {
      reportFailure_ = failure;
    }
  }

  if (sink_ != nullptr) {
    sink_->reportDelivered(delivery);
    // A request that is not periodic is over once its report has gone.
    if (reportManager_->getPeriodicReports().count(reportRequestID) == 0) {
      reportRequests_.ended(reportRequestID, ReportRequestStatus::Completed);
    }
  }
}

void Openadr2bAdapter::sendCreatedReport(
    const std::set<std::string> &pendingReports, std::string requestID,
    std::string responseCode, std::string responseDescription) {
  sendReportMessage("Created report", reportFailure_, reportStopped_, [&] {
    return ven_->createdReport(requestID, responseCode, responseDescription,
                               pendingReports);
  });
}

void Openadr2bAdapter::sendRegisterReport(
    oadrRegisterReportType::oadrReport_sequence &sequence,
    std::string reportRequestID) {
  sendReportMessage("Metadata report", reportFailure_, reportStopped_, [&] {
    return ven_->registerReport(sequence, "", reportRequestID);
  });
}

void Openadr2bAdapter::sendCanceledReport(
    const std::set<std::string> &pendingReports, std::string requestID,
    std::string responseCode, std::string responseDescription) {
  sendReportMessage("Canceled report", reportFailure_, reportStopped_, [&] {
    return ven_->canceledReport(requestID, responseCode, responseDescription,
                                pendingReports);
  });
}

// ---------------------------------------------------------------------------

void Openadr2bAdapter::setParticipation(Participation participation) {
  reportService_->setParticipation(participation);
}

std::optional<std::chrono::seconds>
Openadr2bAdapter::requestedPollInterval() const {
  const auto seconds = ven_->pollFrequencyInSeconds();
  if (seconds == 0) {
    return std::nullopt;
  }
  return std::chrono::seconds(seconds);
}

std::string Openadr2bAdapter::describeClient(const std::string &clientId) const {
  return "VEN " + clientId;
}

AdapterFactory makeOpenadr2bFactory(const SecretOpener *opener,
                                    Openadr2bOptions options) {
  return [opener, options](const AdapterContext &context)
             -> std::unique_ptr<Adapter> {
    auto &store = openadr2bStore(context.store);
    auto settings = store.loadOpenadr2bSettings();

    auto tls = profileTls();
    tls.verifyServer = settings.verifyServerCertificate;
    tls.caCertPem = settings.caCertPem.value_or("");
    if (const auto credential = context.store.loadClientCredential()) {
      auto opened = openClientCredential(*credential, opener);
      tls.clientCertPem = std::move(opened.certPem);
      tls.clientKeyPem = std::move(opened.keyPem);
    }

    ReportSetup reports;
    try {
      const auto points = context.store.loadTelemetryPoints();
      for (const auto &row : store.loadReportOffers()) {
        reports.offers.push_back(bindOffer(row, points));
      }
    } catch (const std::invalid_argument &error) {
      throw AdapterError::permanent(error.what());
    }
    const bool anyDataPoints =
        std::any_of(reports.offers.begin(), reports.offers.end(),
                    [](const ReportOffer &offer) {
                      return offer.enabled && !offer.dataPoints.empty();
                    });
    if (const auto credential = context.store.loadDataCredential()) {
      auto opened = openDataCredential(*credential, opener);
      reports.reader = std::make_shared<datum::SolarNetworkClient>(
          datum::SolarNetworkCredential{std::move(opened.host),
                                        std::move(opened.token),
                                        std::move(opened.secret)},
          options.connectTimeout, options.requestTimeout, &context.abort);
    } else if (anyDataPoints) {
      throw AdapterError::permanent(
          "reports are configured but the connection has no SolarNetwork "
          "credential to read their values with");
    }

    auto transport = std::make_unique<CurlTransport>(
        std::move(tls), options.connectTimeout, options.requestTimeout,
        &context.abort);
    return std::make_unique<Openadr2bAdapter>(
        context.config, std::move(settings), store, std::move(transport),
        nullptr, std::move(reports));
  };
}

} // namespace grid_services::openadr2b
