#include <gtest/gtest.h>

#include <algorithm>
#include <ctime>
#include <regex>
#include <set>
#include <sstream>

#include <oadr/helper/ISO8601Duration.h>
#include <oadr/helper/globaltime/GlobalTime.h>
#include <oadrsd/2b/oadr_20b.hxx>

#include "protocols/openadr2b/openadr2b_adapter.hpp"
#include "core/event_sink.hpp"
#include "core/logging.hpp"
#include "support/fake_datum_reader.hpp"
#include "support/memory_store.hpp"
#include "support/oadr_fixtures.hpp"
#include "support/scripted_http.hpp"

using namespace grid_services;
using namespace grid_services::openadr2b;
using namespace grid_services::testing;
namespace fixtures = grid_services::testing::fixtures;

namespace {

constexpr const char *registerParty = "EiRegisterParty";
constexpr const char *eiEvent = "EiEvent";
constexpr const char *eiReport = "EiReport";
constexpr const char *eiOpt = "EiOpt";
constexpr const char *oadrPoll = "OadrPoll";

// liboadr's report scheduling reads its global clock; the adapter's runDue
// reads the clock it is given. Tests drive both.
class TestClock : public IGlobalTime {
public:
  time_t now() override { return time; }
  std::time_t time = 2'000'000'000;
};

std::string isoTime(std::time_t time) {
  char text[32];
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

class Openadr2bAdapterTest : public ::testing::Test {
protected:
  void TearDown() override { GlobalTime::SetDefaultGlobalTime(); }

  void SetUp() override {
    GlobalTime::SetGlobalTime(&clock);
    ConnectionConfig config;
    config.protocol = "openadr2b";
    connection = db->addConnection(config);
    store = std::make_unique<MemoryRunnerStore>(db, connection);
    sink = std::make_unique<StoreEventSink>(*store, Opt::In, *log);

    settings.vtnUrl = "https://vtn.example.com/";
    settings.venName = "site-ven";
    settings.venId = "ven-1";
    settings.marketContext = "http://market.example.com";
    // Every signal the VEN supports; a connection accepts only SIMPLE unless
    // it is told otherwise.
    settings.acceptedSignals = {"SIMPLE", "ELECTRICITY_PRICE", "LOAD_DISPATCH"};

    http->defaults[eiEvent] = {fixtures::response(), "200", std::nullopt};
    http->defaults[eiReport] = {fixtures::registeredReport(), "200",
                                std::nullopt};
    http->defaults[oadrPoll] = {fixtures::response(), "200", std::nullopt};
    http->defaults[eiOpt] = {fixtures::createdOpt(), "200", std::nullopt};
    http->defaults[registerParty] = {fixtures::response(), "200", std::nullopt};

    adapter = std::make_unique<Openadr2bAdapter>(
        db->connections.at(connection), settings, *store,
        std::make_unique<ScriptedHttp>(http), [this] { return clock.time; });
  }

  // Replaces the adapter with one for a connection accepting `signals`.
  void acceptOnly(std::vector<std::string> signals) {
    auto accepting = settings;
    accepting.acceptedSignals = std::move(signals);
    adapter = std::make_unique<Openadr2bAdapter>(
        db->connections.at(connection), accepting, *store,
        std::make_unique<ScriptedHttp>(http), [this] { return clock.time; });
  }

  // Replaces the adapter with one offering `offers`, read from `reader`.
  void offerReports(std::vector<ReportOffer> offers) {
    adapter = std::make_unique<Openadr2bAdapter>(
        db->connections.at(connection), settings, *store,
        std::make_unique<ScriptedHttp>(http), [this] { return clock.time; },
        ReportSetup{std::move(offers), reader});
  }

  // A TELEMETRY_USAGE offer with a power point averaged from watts and an
  // energy point differenced from wattHours, both on one stream.
  static ReportOffer usageOffer(const std::string &reportName = "TELEMETRY_USAGE",
                                int historySeconds = 7200) {
    ReportOffer offer;
    offer.id = 7;
    offer.reportName = reportName;
    offer.specifierId = "usage-1";
    offer.minSamplingSeconds = 60;
    offer.maxSamplingSeconds = 3600;
    offer.historySeconds = historySeconds;
    ReportDataPoint power;
    power.rid = "power";
    power.pointId = "site-power";
    power.resourceId = "SITE-1";
    power.source.nodeId = 604;
    power.source.sourceId = "/S/1";
    power.source.property = "watts";
    ReportDataPoint energy;
    energy.rid = "energy";
    energy.pointId = "site-energy";
    energy.resourceId = "SITE-1";
    energy.reportType = "usage";
    energy.readingType = "Direct Read";
    energy.item.kind = "energy_real";
    energy.item.units = "Wh";
    energy.source.nodeId = 604;
    energy.source.sourceId = "/S/1";
    energy.source.property = "wattHours";
    energy.source.propertyClass = datum::PropertyClass::Accumulating;
    energy.source.reduce = datum::Reduce::Difference;
    offer.dataPoints = {power, energy};
    return offer;
  }

  // Puts a datum on the offer's stream `secondsAgo` before the clock.
  void addDatum(long secondsAgo, double watts, double wattHours) {
    reader->add(604, "/S/1",
                {datum::Timestamp(std::chrono::seconds(clock.time - secondsAgo)),
                 {{"watts", watts}, {"wattHours", wattHours}}});
  }

  // Registers and runs the first poll, which syncs reports and events.
  void registerAndSync(const std::vector<fixtures::EventSpec> &events = {}) {
    http->queue(registerParty,
                fixtures::createdPartyRegistration("ven-1", "reg-1"));
    adapter->registerConnection(*sink);
    http->queue(eiEvent, fixtures::distributeEvent("req-sync", events));
    adapter->poll(*sink);
  }

  PollResult deliver(const std::string &requestId,
                     const std::vector<fixtures::EventSpec> &events) {
    http->queue(oadrPoll, fixtures::distributeEvent(requestId, events));
    return adapter->poll(*sink);
  }

  TestClock clock;
  Openadr2bSettings settings;
  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  std::shared_ptr<HttpScript> http = std::make_shared<HttpScript>();
  std::shared_ptr<FakeDatumReader> reader = std::make_shared<FakeDatumReader>();
  std::shared_ptr<spdlog::logger> log = makeLogger("test");
  ConnectionId connection = 0;
  std::unique_ptr<MemoryRunnerStore> store;
  std::unique_ptr<StoreEventSink> sink;
  std::unique_ptr<Openadr2bAdapter> adapter;
};

AdapterError::Kind kindOf(const std::function<void()> &call) {
  try {
    call();
  } catch (const AdapterError &error) {
    return error.kind();
  }
  ADD_FAILURE() << "expected an AdapterError";
  return AdapterError::Kind::Stopped;
}

} // namespace

TEST_F(Openadr2bAdapterTest, RegistrationReturnsTheProgramsIdentity) {
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-9", "reg-9", "PT15S"));
  const auto info = adapter->registerConnection(*sink);

  EXPECT_EQ(info.clientId, "ven-9");
  EXPECT_EQ(info.registrationId, "reg-9");
  EXPECT_EQ(info.pollIntervalSeconds, 15);
  EXPECT_EQ(adapter->requestedPollInterval(), std::chrono::seconds(15));

  const auto exchange = adapter->takeExchange();
  ASSERT_EQ(exchange.size(), 2u);
  EXPECT_EQ(exchange[0]["direction"], "to_program");
  EXPECT_EQ(exchange[0]["format"], "xml");
  EXPECT_NE(exchange[0]["body"].get<std::string>().find(
                "oadrCreatePartyRegistration"),
            std::string::npos);
  EXPECT_EQ(exchange[1]["direction"], "from_program");
  EXPECT_NE(exchange[1]["body"].get<std::string>().find(
                "oadrCreatedPartyRegistration"),
            std::string::npos);
  for (const auto &message : exchange) {
    EXPECT_EQ(message.size(), 4u) << message;
    EXPECT_TRUE(std::regex_match(
        message["at"].get<std::string>(),
        std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z)")))
        << message["at"];
  }
  EXPECT_TRUE(adapter->takeExchange().is_null());
}

TEST_F(Openadr2bAdapterTest, RejectedRegistrationIsPermanent) {
  http->queue(registerParty, fixtures::createdPartyRegistration(
                                 "ven-1", "reg-1", "PT10S", "452"));
  EXPECT_EQ(kindOf([&] { adapter->registerConnection(*sink); }),
            AdapterError::Kind::Permanent);
}

TEST_F(Openadr2bAdapterTest, CredentialRejectionIsPermanentServerErrorsAreNot) {
  http->queue(registerParty, "", "403");
  EXPECT_EQ(kindOf([&] { adapter->registerConnection(*sink); }),
            AdapterError::Kind::Permanent);
  http->queue(registerParty, "", "503");
  EXPECT_EQ(kindOf([&] { adapter->registerConnection(*sink); }),
            AdapterError::Kind::Transient);
  http->queue(registerParty, "<not xml");
  EXPECT_EQ(kindOf([&] { adapter->registerConnection(*sink); }),
            AdapterError::Kind::Transient);
}

TEST_F(Openadr2bAdapterTest, FirstPollRegistersReportsAndRequestsEvents) {
  registerAndSync({fixtures::EventSpec{}});

  EXPECT_EQ(http->requestsContaining(eiReport, "oadrRegisterReport").size(), 1u);
  EXPECT_EQ(http->requestsContaining(eiReport, "TELEMETRY_STATUS").size(), 1u);
  EXPECT_EQ(http->requestsContaining(eiEvent, "oadrRequestEvent").size(), 1u);

  const auto event = db->event(1);
  ASSERT_TRUE(event);
  EXPECT_EQ(event->programEventId, "event-1");
  EXPECT_EQ(event->programRequestId, "req-sync");

  const auto created = http->requestsContaining(eiEvent, "oadrCreatedEvent");
  ASSERT_EQ(created.size(), 1u);
  EXPECT_NE(created[0].body.find("optIn"), std::string::npos);
  EXPECT_EQ(event->respondedModification, 0);

  // Sync happens once.
  adapter->poll(*sink);
  EXPECT_EQ(http->requestsContaining(eiEvent, "oadrRequestEvent").size(), 1u);
}

TEST_F(Openadr2bAdapterTest, RoutinePollHasNoSummary) {
  registerAndSync();
  const auto result = adapter->poll(*sink);
  EXPECT_EQ(result.outcome, PollOutcome::Ok);
  EXPECT_FALSE(result.summary);
  EXPECT_FALSE(result.moreQueued);
}

TEST_F(Openadr2bAdapterTest, MessagesFromTheProgramMayHaveMoreQueued) {
  registerAndSync();
  EXPECT_TRUE(deliver("req-1", {fixtures::EventSpec{}}).moreQueued);
  http->queue(oadrPoll,
              fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  EXPECT_TRUE(adapter->poll(*sink).moreQueued);
}

TEST_F(Openadr2bAdapterTest, UnchangedEventsAreNotCountedAsChanges) {
  registerAndSync();
  deliver("req-1", {fixtures::EventSpec{}});
  const auto result = deliver("req-2", {fixtures::EventSpec{}});

  EXPECT_EQ(*result.summary, "Received events from the program (0 changed)");
}

TEST_F(Openadr2bAdapterTest, ModificationKeepsAnOperatorOptOut) {
  registerAndSync({fixtures::EventSpec{}});
  store->setOpt(1, Opt::Out, OptSource::Operator);

  fixtures::EventSpec modified;
  modified.modification = 1;
  deliver("req-2", {modified});

  const auto created = http->requestsContaining(eiEvent, "oadrCreatedEvent");
  ASSERT_EQ(created.size(), 2u);
  EXPECT_NE(created[1].body.find("optOut"), std::string::npos);
  EXPECT_EQ(db->event(1)->modificationNumber, 1);
  EXPECT_EQ(db->event(1)->opt, Opt::Out);
}

TEST_F(Openadr2bAdapterTest, EventsMissingFromTheListHaveEnded) {
  fixtures::EventSpec future;
  future.id = "future";
  registerAndSync({future});
  // An event received while it was still to come, whose window has passed.
  NormalizedEvent past;
  past.programEventId = "past";
  past.startAt = Timestamp(std::chrono::seconds(1'577'836'800));
  past.durationSeconds = 3600;
  db->addEvent(connection, past);

  deliver("req-2", {});
  EXPECT_EQ(db->event(1)->status, ProgramStatus::Cancelled);
  EXPECT_EQ(db->event(2)->status, ProgramStatus::Completed);
}

TEST_F(Openadr2bAdapterTest, IgnoresEventsForOtherVens) {
  fixtures::EventSpec other;
  other.venIds = {"ven-2"};
  registerAndSync({other});
  EXPECT_FALSE(db->event(1));
}

TEST_F(Openadr2bAdapterTest, EventsWithoutRequiredResponseAreNotAnswered) {
  fixtures::EventSpec quiet;
  quiet.responseRequired = "never";
  registerAndSync({quiet});
  EXPECT_TRUE(db->event(1));
  EXPECT_TRUE(http->requestsContaining(eiEvent, "oadrCreatedEvent").empty());
}

TEST_F(Openadr2bAdapterTest, UndeliveredAnswersAreSentAgain) {
  registerAndSync();
  http->queue(eiEvent, "", "500");
  EXPECT_EQ(kindOf([&] { deliver("req-1", {fixtures::EventSpec{}}); }),
            AdapterError::Kind::Transient);
  ASSERT_EQ(db->responsesFor(1).size(), 1u);
  EXPECT_FALSE(db->responsesFor(1)[0].ok);

  // The next poll asks for the full event list and answers again.
  http->queue(eiEvent, fixtures::distributeEvent("req-2", {fixtures::EventSpec{}}));
  adapter->poll(*sink);
  EXPECT_EQ(db->responsesFor(1).size(), 2u);
  EXPECT_TRUE(db->responsesFor(1)[1].ok);
  EXPECT_EQ(db->event(1)->respondedModification, 0);
}

TEST_F(Openadr2bAdapterTest, PollReportsRegistrationChangesFromTheProgram) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::cancelPartyRegistration());
  EXPECT_EQ(adapter->poll(*sink).outcome, PollOutcome::RegistrationCancelled);
  http->queue(oadrPoll, fixtures::requestReregistration());
  EXPECT_EQ(adapter->poll(*sink).outcome, PollOutcome::ReregistrationRequested);
}

TEST_F(Openadr2bAdapterTest, ErrorPollResponseIsTransient) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::response("463"));
  EXPECT_EQ(kindOf([&] { adapter->poll(*sink); }),
            AdapterError::Kind::Transient);
}

TEST_F(Openadr2bAdapterTest, UnregisterAcceptsCancelledOrUnknown) {
  registerAndSync();
  http->queue(registerParty, fixtures::canceledPartyRegistration("200"));
  adapter->unregisterConnection(*sink);
  http->queue(registerParty, fixtures::canceledPartyRegistration("453"));
  adapter->unregisterConnection(*sink);
  http->queue(registerParty, fixtures::canceledPartyRegistration("500"));
  EXPECT_EQ(kindOf([&] { adapter->unregisterConnection(*sink); }),
            AdapterError::Kind::Transient);
}

TEST_F(Openadr2bAdapterTest, TestDescribesWhatTheVtnKnows) {
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-1", "reg-1"));
  const auto known = adapter->test(*sink);
  EXPECT_EQ(known.description,
            "reached VTN vtn-1; it has this VEN registered as ven-1");
  EXPECT_TRUE(known.registered);
  EXPECT_EQ(known.clientId, "ven-1");

  http->queue(registerParty, fixtures::createdPartyRegistrationUnknown());
  const auto unknown = adapter->test(*sink);
  EXPECT_EQ(unknown.description,
            "reached VTN vtn-1; it has no registration for this VEN");
  EXPECT_FALSE(unknown.registered);
  EXPECT_FALSE(unknown.clientId);
}

TEST_F(Openadr2bAdapterTest, ResumeConfirmsTheRegistration) {
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-1", "reg-7"));
  const auto info = adapter->resume({}, *sink);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->registrationId, "reg-7");

  // The next poll re-registers reports and re-requests events.
  http->queue(eiEvent, fixtures::distributeEvent("r", {}));
  adapter->poll(*sink);
  EXPECT_EQ(http->requestsContaining(eiReport, "oadrRegisterReport").size(), 1u);

  http->queue(registerParty, fixtures::createdPartyRegistrationUnknown());
  EXPECT_FALSE(adapter->resume({}, *sink));
}

TEST_F(Openadr2bAdapterTest, SendsPeriodicStatusReports) {
  registerAndSync();

  http->queue(oadrPoll, fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  EXPECT_EQ(*adapter->poll(*sink).summary, "The program requested reports");
  EXPECT_EQ(http->requestsContaining(eiReport, "oadrCreatedReport").size(), 1u);

  // Reports are due at the start and then every reporting interval (PT1M).
  adapter->runDueWork(*sink);
  auto updates = http->requestsContaining(eiReport, "oadrUpdateReport");
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_NE(updates[0].body.find("rr-1"), std::string::npos);
  EXPECT_NE(updates[0].body.find("oadrManualOverride>false"), std::string::npos);

  clock.time += 30;
  adapter->runDueWork(*sink);
  EXPECT_EQ(http->requestsContaining(eiReport, "oadrUpdateReport").size(), 1u);

  adapter->setParticipation(Participation::Paused);
  clock.time += 31;
  adapter->runDueWork(*sink);
  updates = http->requestsContaining(eiReport, "oadrUpdateReport");
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_NE(updates[1].body.find("oadrManualOverride>true"), std::string::npos);
}

TEST_F(Openadr2bAdapterTest, FailedReportDeliveryIsTransient) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);

  http->queue(eiReport, "", "500");
  EXPECT_EQ(kindOf([&] { adapter->runDueWork(*sink); }),
            AdapterError::Kind::Transient);

  // The job moved on; the next interval sends again.
  clock.time += 60;
  adapter->runDueWork(*sink);
  EXPECT_EQ(http->requestsContaining(eiReport, "oadrUpdateReport").size(), 2u);
}

TEST_F(Openadr2bAdapterTest, RegisteringAgainDropsTheOldReportRequests) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);
  ASSERT_EQ(http->requestsContaining(eiReport, "oadrUpdateReport").size(), 1u);

  // The program asks the VEN to register again; its requests are lost on
  // its side, and the new registration offers the reports afresh.
  http->queue(oadrPoll, fixtures::requestReregistration());
  ASSERT_EQ(adapter->poll(*sink).outcome, PollOutcome::ReregistrationRequested);
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-1", "reg-1"));
  adapter->registerConnection(*sink);
  http->queue(eiEvent, fixtures::distributeEvent("req-again", {}));
  adapter->poll(*sink);
  http->queue(oadrPoll, fixtures::createStatusReport("rr-2", isoTime(clock.time)));
  adapter->poll(*sink);

  clock.time += 60;
  adapter->runDueWork(*sink);
  const auto updates = http->requestsContaining(eiReport, "oadrUpdateReport");
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_NE(updates[1].body.find("rr-2"), std::string::npos);
  EXPECT_EQ(updates[1].body.find("rr-1"), std::string::npos);

  const auto requests = db->reportRequestsFor(connection);
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[0].request.programRequestId, "rr-1");
  EXPECT_EQ(requests[0].status, ReportRequestStatus::Completed);
  EXPECT_EQ(requests[1].request.programRequestId, "rr-2");
  EXPECT_EQ(requests[1].status, ReportRequestStatus::Active);
}

// ---------------------------------------------------------------------------
// OpenADR 2.0b Profile Specification v1.1, section 11.2 conformance rules.
//
// Each test is named after the conformance rule it checks and quotes the
// requirement. Most of these document gaps in the current VEN and fail until
// the VEN is fixed.

namespace {

std::unique_ptr<oadr2b::oadr::oadrPayload> parse(const std::string &xml) {
  std::istringstream stream(xml);
  return oadr2b::oadr::oadrPayload_(
      stream, xml_schema::flags::keep_dom | xml_schema::flags::dont_validate |
                  xml_schema::flags::dont_initialize);
}

// The payloads the VEN sent to `endpoint`, or to any endpoint when it is
// empty, oldest first.
std::vector<std::unique_ptr<oadr2b::oadr::oadrPayload>>
sent(const HttpScript &http, const std::string &endpoint = "") {
  std::vector<std::unique_ptr<oadr2b::oadr::oadrPayload>> payloads;
  for (const auto &request : http.requests) {
    if (endpoint.empty() || request.endpoint == endpoint) {
      payloads.push_back(parse(request.body));
    }
  }
  return payloads;
}

struct Answer {
  unsigned long long modification = 0;
  std::string code;
  std::string opt;
  std::string requestId;
};

// Every eventResponse the VEN sent for `eventId`, oldest first.
std::vector<Answer> answersFor(const HttpScript &http,
                               const std::string &eventId) {
  std::vector<Answer> answers;
  for (const auto &payload : sent(http, eiEvent)) {
    const auto &message = payload->oadrSignedObject();
    if (!message.oadrCreatedEvent().present()) {
      continue;
    }
    const auto &created = message.oadrCreatedEvent()->eiCreatedEvent();
    if (!created.eventResponses().present()) {
      continue;
    }
    for (const auto &response : created.eventResponses()->eventResponse()) {
      if (response.qualifiedEventID().eventID() != eventId) {
        continue;
      }
      answers.push_back({response.qualifiedEventID().modificationNumber(),
                         response.responseCode(),
                         static_cast<const std::string &>(response.optType()),
                         response.requestID()});
    }
  }
  return answers;
}

// The eiResponse code of every oadrCreatedEvent the VEN sent, oldest first.
std::vector<std::string> createdEventCodes(const HttpScript &http) {
  std::vector<std::string> codes;
  for (const auto &payload : sent(http, eiEvent)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrCreatedEvent().present()) {
      codes.push_back(message.oadrCreatedEvent()
                          ->eiCreatedEvent()
                          .eiResponse()
                          .responseCode());
    }
  }
  return codes;
}

bool isError(const std::string &code) {
  return code.size() == 3 && code[0] == '4';
}

// Update reports the VEN sent for `reportRequestId`.
std::vector<oadrReportType> updatesFor(const HttpScript &http,
                                       const std::string &reportRequestId) {
  std::vector<oadrReportType> reports;
  for (const auto &payload : sent(http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (!message.oadrUpdateReport().present()) {
      continue;
    }
    for (const auto &report : message.oadrUpdateReport()->oadrReport()) {
      if (report.reportRequestID() == reportRequestId) {
        reports.push_back(report);
      }
    }
  }
  return reports;
}

// The rID and value of every payloadFloat in an update report, interval by
// interval, with the data quality alongside.
struct FloatPayload {
  std::string rid;
  float value;
  std::string quality;
};

std::vector<std::vector<FloatPayload>> floatPayloads(const oadrReportType &report) {
  std::vector<std::vector<FloatPayload>> intervals;
  if (!report.intervals().present()) {
    return intervals;
  }
  for (const auto &interval : report.intervals()->interval()) {
    std::vector<FloatPayload> payloads;
    for (const auto &base : interval.streamPayloadBase()) {
      const auto *data =
          dynamic_cast<const oadr2b::oadr::oadrReportPayloadType *>(&base);
      if (data == nullptr) {
        continue;
      }
      const auto *value =
          dynamic_cast<const oadr2b::ei::PayloadFloatType *>(&data->payloadBase());
      if (value == nullptr) {
        continue;
      }
      payloads.push_back(
          {data->rID(), value->value(),
           data->oadrDataQuality().present()
               ? static_cast<std::string>(*data->oadrDataQuality())
               : ""});
    }
    intervals.push_back(payloads);
  }
  return intervals;
}

// The response codes of the oadrCreatedReport payloads the VEN sent.
std::vector<std::string> createdReportCodes(const HttpScript &http) {
  std::vector<std::string> codes;
  for (const auto &payload : sent(http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrCreatedReport().present()) {
      codes.push_back(message.oadrCreatedReport()->eiResponse().responseCode());
    }
  }
  return codes;
}

// The metadata reports in the first oadrRegisterReport the VEN sent.
std::vector<oadrReportType> metadataReports(const HttpScript &http) {
  for (const auto &payload : sent(http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrRegisterReport().present()) {
      const auto &reports = message.oadrRegisterReport()->oadrReport();
      return {reports.begin(), reports.end()};
    }
  }
  return {};
}

const oadrReportType *findReport(const std::vector<oadrReportType> &reports,
                                 const std::string &reportName) {
  for (const auto &report : reports) {
    if (report.reportName() && *report.reportName() == reportName) {
      return &report;
    }
  }
  return nullptr;
}

unsigned int seconds(const std::string &duration) {
  return ISO8601Duration::TotalSeconds(duration);
}

// The oadrCreateOpt payloads the VEN sent.
std::vector<oadrCreateOptType> createOpts(const HttpScript &http) {
  std::vector<oadrCreateOptType> opts;
  for (const auto &payload : sent(http, eiOpt)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrCreateOpt().present()) {
      opts.push_back(*message.oadrCreateOpt());
    }
  }
  return opts;
}

class Openadr2bConformanceTest : public Openadr2bAdapterTest {
protected:
  // Exercises every kind of message the VEN sends: registration, report
  // registration, event request and answer, poll, report creation, update
  // and cancellation, and an operator opt change.
  void runSession() {
    registerAndSync({fixtures::EventSpec{}});
    http->queue(oadrPoll,
                fixtures::createStatusReport("rr-1", isoTime(clock.time)));
    adapter->poll(*sink);
    adapter->runDueWork(*sink);
    adapter->respond(*db->event(1), Opt::Out, *sink);
    http->queue(oadrPoll, fixtures::cancelReport("rr-1"));
    adapter->poll(*sink);
  }
};

} // namespace

// Rule 36: "An event cancellation received by the VEN MUST be acknowledged
// with an oadrCreatedEvent with the optType element set as follows, unless the
// oadrResponseRequired is set to 'never': optIn = Confirm to cancellation;
// optOut = Cannot cancel."
TEST_F(Openadr2bConformanceTest, Rule36_CancellationIsAcknowledgedWithOptIn) {
  registerAndSync({fixtures::EventSpec{}});
  store->setOpt(1, Opt::Out, OptSource::Operator);

  fixtures::EventSpec cancelled;
  cancelled.modification = 1;
  cancelled.status = "cancelled";
  deliver("req-2", {cancelled});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 2u);
  EXPECT_EQ(answers[1].modification, 1u);
  EXPECT_EQ(answers[1].opt, "optIn");
}

// Rule 19: "If an oadrDistributeEvent payload has as mix of valid and invalid
// events, the VEN implementation MUST only respond to the relevant valid events
// and not reject the entire message."
// Rule 48: "In case of a logical error for an event, optType is set to optOut."
TEST_F(Openadr2bConformanceTest,
       Rule19_InvalidEventDoesNotRejectTheWholeDistribution) {
  registerAndSync();
  fixtures::EventSpec valid;
  valid.id = "valid";
  fixtures::EventSpec invalid;
  invalid.id = "invalid";
  invalid.floatPayload = false;

  EXPECT_NO_THROW(deliver("req-1", {valid, invalid}));

  const auto validAnswers = answersFor(*http, "valid");
  ASSERT_EQ(validAnswers.size(), 1u);
  EXPECT_EQ(validAnswers[0].code, "200");
  EXPECT_EQ(validAnswers[0].opt, "optIn");
  for (const auto &answer : answersFor(*http, "invalid")) {
    EXPECT_TRUE(isError(answer.code)) << answer.code;
    EXPECT_EQ(answer.opt, "optOut");
  }
}

// Rule 48: "The following logical errors MUST be detected by implementations
// • VEN receives non-matching market context (462) • VEN receives non-matching
// eiTarget (462) • VEN receives unsupported signalName (460). In case of a
// logical error for an event, optType is set to optOut."
// Rule 62: the VEN "MUST reply with a createdEvent message for every EiEvent
// message in which the responseRequired is set to 'always'".
TEST_F(Openadr2bConformanceTest, Rule48_EventForAnotherVenIsAnsweredWith462) {
  fixtures::EventSpec other;
  other.venIds = {"ven-2"};
  registerAndSync({other});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "462");
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 48, as above: "VEN receives non-matching market context (462)".
TEST_F(Openadr2bConformanceTest,
       Rule48_EventInAnotherMarketContextIsAnsweredWith462) {
  fixtures::EventSpec elsewhere;
  elsewhere.marketContext = "http://other-market.example.com";
  registerAndSync({elsewhere});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "462");
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 512: "For marketContext, the normalization rules specified in
// [RFC3986] (page 40) MUST be applied, allowing for hostname and scheme (at
// least) to be case insensitive and still be equivalent."
// Passes today because market contexts are not compared at all; it keeps the
// rule 48 check from being too strict.
TEST_F(Openadr2bConformanceTest,
       Rule512_MarketContextMatchIgnoresSchemeAndHostCase) {
  fixtures::EventSpec spec;
  spec.marketContext = "HTTP://Market.Example.COM";
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "200");
  EXPECT_EQ(answers[0].opt, "optIn");
}

// Rule 33: "The implementation MUST provide an application layer error
// indication as a result of the following conditions: ... Illogical request –
// Old date on new event, durations do not add up correctly, etc."
// Rule 8: "oadrDistributeEvent eventSignal interval durations for a given
// event MUST add up to eiEvent eiActivePeriod duration."
TEST_F(Openadr2bConformanceTest,
       Rule33_IntervalsThatDoNotAddUpAreAnsweredWithAnError) {
  fixtures::EventSpec spec;
  spec.duration = "PT3H";
  spec.intervals = {{"PT1H", 1.0}};
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_TRUE(isError(answers[0].code)) << answers[0].code;
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 21: "If venID, vtnID, or eventID value is included in the payload, the
// receiving entity MUST validate that the ID value is as expected and generate
// an error if an unexpected value is received."
// Rule 33: "ID not as expected (452)".
TEST_F(Openadr2bConformanceTest, Rule21_DistributionFromAnotherVtnIsRejected) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::distributeEvent(
                            "req-1", {fixtures::EventSpec{}}, "vtn-2"));
  try {
    adapter->poll(*sink);
  } catch (const AdapterError &) {
    // Failing the poll is acceptable, as long as the VTN hears about it.
  }

  EXPECT_FALSE(db->event(1));
  const auto codes = createdEventCodes(*http);
  ASSERT_FALSE(codes.empty());
  EXPECT_EQ(codes.back(), "452");
}

// Rule 12: "Always – The VEN MUST respond to the event with an
// oadrCreatedEvent eventResponse. This includes unchanged, new, changed, and
// canceled events."
// Rule 62 repeats this for "the event is one in which the VEN is already
// aware", and rule 42 requires the received requestID in the eventResponse.
TEST_F(Openadr2bConformanceTest,
       Rule12_UnchangedEventsAreAnsweredWhenResponseRequired) {
  registerAndSync();
  deliver("req-1", {fixtures::EventSpec{}});
  deliver("req-2", {fixtures::EventSpec{}});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 2u);
  EXPECT_EQ(answers[1].requestId, "req-2");
  EXPECT_EQ(answers[1].opt, "optIn");
}

// Rule 58: "If the VTN sends an oadrEvent with an eventID that the VEN is
// already aware of, but which has a lower modification number than one in
// which the VEN is already aware then this is an error and the VEN SHOULD
// respond with the appropriate error code (450)."
TEST_F(Openadr2bConformanceTest, Rule58_StaleModificationIsAnsweredWith450) {
  fixtures::EventSpec current;
  current.modification = 2;
  registerAndSync({current});

  fixtures::EventSpec stale;
  stale.modification = 1;
  deliver("req-2", {stale});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 2u);
  EXPECT_EQ(answers[1].modification, 1u);
  EXPECT_EQ(answers[1].code, "450");
  EXPECT_EQ(db->event(1)->modificationNumber, 2);
}

// Rule 60: "If the VTN sends an oadrEvent object with the eventStatus set to
// 'cancelled' and has an eventID that the VEN is not aware of then the VEN
// SHOULD ignore the event since it is not currently in its list of known
// events, but still MUST respond with the createdEvent if required to do so by
// oadrResponseRequired."
// Rule 21: "A VEN MUST NOT generate an error upon receipt of a canceled event
// whose eventID is not previously known."
TEST_F(Openadr2bConformanceTest,
       Rule60_UnknownCancelledEventIsAnsweredButNotStored) {
  registerAndSync();
  fixtures::EventSpec unknown;
  unknown.id = "never-seen";
  unknown.status = "cancelled";
  deliver("req-1", {unknown});

  const auto answers = answersFor(*http, "never-seen");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "200");
  EXPECT_EQ(answers[0].opt, "optIn");
  EXPECT_FALSE(db->event(1));
}

// Rule 402: "oadrCreatePartyRegistration MUST include the following payload
// elements: ... registrationID – when reregistering only".
// Section 8.4, Figure 15: after oadrRequestReregistration the VEN sends
// "oadrCreatePartyRegistration(VEN Info, registrationID)".
// Rule 406: a registration without venID and registrationID makes the VTN
// "erase all existing reporting and registration information".
TEST_F(Openadr2bConformanceTest,
       Rule402_ReregistrationReferencesTheCurrentRegistration) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::requestReregistration());
  ASSERT_EQ(adapter->poll(*sink).outcome, PollOutcome::ReregistrationRequested);

  // The runner registers again on this outcome.
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-1", "reg-1"));
  adapter->registerConnection(*sink);

  const auto registrations = sent(*http, registerParty);
  const auto &last = registrations.back()->oadrSignedObject();
  ASSERT_TRUE(last.oadrCreatePartyRegistration().present());
  const auto &create = *last.oadrCreatePartyRegistration();
  ASSERT_TRUE(create.registrationID().present());
  EXPECT_EQ(*create.registrationID(), "reg-1");
  ASSERT_TRUE(create.venID().present());
  EXPECT_EQ(*create.venID(), "ven-1");
}

// Rule 500: "If a logical response is required by the VEN to the received
// operational payload, the VEN MUST send that logical response asynchronously
// via a transport request."
// Section 8.4, Figure 15: "The response to this request is an oadrResponse
// acknowledgement followed by an asynchronous oadrCreatePartyRegistration
// request from the VEN."
TEST_F(Openadr2bConformanceTest, Rule500_ReregistrationRequestIsAcknowledged) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::requestReregistration());
  adapter->poll(*sink);

  int acknowledgements = 0;
  for (const auto &payload : sent(*http)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrResponse().present()) {
      ++acknowledgements;
      EXPECT_EQ(message.oadrResponse()->eiResponse().responseCode(), "200");
    }
  }
  EXPECT_EQ(acknowledgements, 1);
}

// Rule 500, as above.
// Section 8.4, Figure 16: "The VTN or VEN may cancel an active registration
// using the oadrCancelPartyRegistration payload, referencing the
// registrationID. The other party responds with an oadrCanceledRegistration
// payload."
TEST_F(Openadr2bConformanceTest, Rule500_ProgramCancellationIsAcknowledged) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::cancelPartyRegistration());
  adapter->poll(*sink);

  int acknowledgements = 0;
  for (const auto &payload : sent(*http)) {
    const auto &message = payload->oadrSignedObject();
    if (!message.oadrCanceledPartyRegistration().present()) {
      continue;
    }
    ++acknowledgements;
    const auto &canceled = *message.oadrCanceledPartyRegistration();
    EXPECT_EQ(canceled.eiResponse().responseCode(), "200");
    EXPECT_EQ(canceled.eiResponse().requestID(), "cancel-1");
    ASSERT_TRUE(canceled.registrationID().present());
    EXPECT_EQ(*canceled.registrationID(), "reg-1");
  }
  EXPECT_EQ(acknowledgements, 1);
}

// Rule 407: "If a device receives an oadrCancelPartyRegistration from the
// other party, it MUST erase all information about this device, including
// exchanged Metadata reports, requested reports, and registration
// information."
TEST_F(Openadr2bConformanceTest, Rule407_ProgramCancellationStopsReports) {
  registerAndSync();
  http->queue(oadrPoll,
              fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);
  ASSERT_EQ(updatesFor(*http, "rr-1").size(), 1u);

  http->queue(oadrPoll, fixtures::cancelPartyRegistration());
  ASSERT_EQ(adapter->poll(*sink).outcome, PollOutcome::RegistrationCancelled);
  clock.time += 120;
  adapter->runDueWork(*sink);

  EXPECT_EQ(updatesFor(*http, "rr-1").size(), 1u);
}

// Rule 400: "The following ID's SHOULD be validated by the implementations
// receiving the payload. If the ID does not match the expected ID, the device
// SHOULD include a 452 error in eiResponse. • venID • vtnID •
// registrationID"
TEST_F(Openadr2bConformanceTest,
       Rule400_CancellationOfAnotherRegistrationIsRejected) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::cancelPartyRegistration("reg-other"));
  EXPECT_EQ(adapter->poll(*sink).outcome, PollOutcome::Ok);

  std::vector<std::string> codes;
  for (const auto &payload : sent(*http)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrCanceledPartyRegistration().present()) {
      codes.push_back(
          message.oadrCanceledPartyRegistration()->eiResponse().responseCode());
    }
  }
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_EQ(codes[0], "452");
}

// Rule 301: implementations "MUST ... Send oadrCreatedReport payload in
// response to an oadrCreateReport payload requesting a Metadata report" and
// "Send oadrRegisterReport payload containing a Metadata report should an
// oadrCreateReport payload request Metadata reports."
// Rule 327: Metadata reports are requested with the reportSpecifierID
// "METADATA".
// Rule 311: "If the oadrRegisterReport payload is being sent as the result of
// a oadrCreateReport request, the optional oadrRegisterReport.reportRequestID
// element must be present with a value that matches the reportRequestID used
// when the request was made."
// Rule 306: "An implementation (VEN or VTN) sending a payload containing a
// Metadata report MUST supply all of its reporting capabilities."
TEST_F(Openadr2bConformanceTest,
       Rule301_MetadataRequestIsAnsweredWithRegisterReport) {
  offerReports({usageOffer()});
  registerAndSync();
  http->queue(oadrPoll, fixtures::createReport(
                            {{"rr-meta", "METADATA", "0", "PT0S", ""}}));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  EXPECT_EQ(http->requestsContaining(eiReport, "oadrCreatedReport").size(), 1u);
  std::vector<oadrRegisterReportType> registers;
  for (const auto &payload : sent(*http, eiReport)) {
    if (payload->oadrSignedObject().oadrRegisterReport().present()) {
      registers.push_back(*payload->oadrSignedObject().oadrRegisterReport());
    }
  }
  // The first is the unsolicited one sent after registration.
  ASSERT_EQ(registers.size(), 2u);
  ASSERT_TRUE(registers[1].reportRequestID().present());
  EXPECT_EQ(*registers[1].reportRequestID(), "rr-meta");
  const std::vector<oadrReportType> reports(registers[1].oadrReport().begin(),
                                            registers[1].oadrReport().end());
  EXPECT_NE(findReport(reports, "METADATA_TELEMETRY_STATUS"), nullptr);
  EXPECT_NE(findReport(reports, "METADATA_TELEMETRY_USAGE"), nullptr);
  EXPECT_TRUE(updatesFor(*http, "rr-meta").empty());
}

// Rule 324: "If reportBackDuration is 0, then the report MUST be sent
// immediately and only once using the oadrUpdateReport or oadrRegisterReport
// payloads. ... For Telemetry reports with a reportBackDuration of 0, the
// granularity value MUST be ignored and only one interval of data for each
// data point MUST be returned in the report."
TEST_F(Openadr2bConformanceTest, Rule324_OneShotStatusReportContainsData) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::createStatusReport(
                            "rr-once", isoTime(clock.time), "PT0S"));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);
  clock.time += 120;
  adapter->runDueWork(*sink);

  const auto reports = updatesFor(*http, "rr-once");
  ASSERT_EQ(reports.size(), 1u);
  ASSERT_TRUE(reports[0].intervals().present());
  EXPECT_EQ(reports[0].intervals()->interval().size(), 1u);
}

// Rule 336: "An oadrCancelReport MAY include multiple reportRequestID values.
// If the receiving party cannot successfully cancel all the reports listed or
// does not recognize one of the included reportRequestID values, it MUST
// return a 4xx error in the responseCode element of oadrCanceledReport."
TEST_F(Openadr2bConformanceTest, Rule336_CancellingAnUnknownReportIsAnError) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::cancelReport("rr-unknown"));
  adapter->poll(*sink);

  std::vector<std::string> codes;
  for (const auto &payload : sent(*http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrCanceledReport().present()) {
      codes.push_back(
          message.oadrCanceledReport()->eiResponse().responseCode());
    }
  }
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_TRUE(isError(codes[0])) << codes[0];
}

// Rule 344: "VTN or VEN MAY include oadrReportRequest object in
// oadrRegisteredReport payload (Piggyback Report Request). VEN and VTNs are
// not required to implement sending Piggy Back Report Requests, but they must
// be able to receive such a request and act upon it."
TEST_F(Openadr2bConformanceTest, Rule344_PiggybackReportRequestIsActedOn) {
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-1", "reg-1"));
  adapter->registerConnection(*sink);
  http->queue(eiReport, fixtures::registeredReport(
                            {{"rr-piggy", "TELEMETRY_STATUS", "status", "PT1M",
                              isoTime(clock.time)}}));
  http->queue(eiEvent, fixtures::distributeEvent("req-sync", {}));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  EXPECT_EQ(http->requestsContaining(eiReport, "oadrCreatedReport").size(), 1u);
  EXPECT_EQ(updatesFor(*http, "rr-piggy").size(), 1u);
}

// Rule 66: "The receipt of an eventStatus equal to completed MUST NOT cause
// the VEN to change its operational status with respect to executing the
// event."
// Rule 30: "Event completion times are determined by adding the event
// duration to the randomized dtstart time." With a startafter window the event
// may still be running after dtstart + duration.
TEST_F(Openadr2bConformanceTest,
       Rule66_CompletedStatusDoesNotEndAnEventInsideItsRandomizationWindow) {
  fixtures::EventSpec spec;
  // Ended five minutes ago; the randomization window runs five more.
  spec.start = isoTime(std::time(nullptr) - 65 * 60);
  spec.duration = "PT1H";
  spec.intervals = {{"PT1H", 1.0}};
  spec.startAfter = "PT10M";
  spec.status = "completed";
  registerAndSync({spec});

  ASSERT_TRUE(db->event(1));
  EXPECT_EQ(db->event(1)->status, ProgramStatus::Active);
}

// Rule 510: "A VTN MUST be capable of sending and a VEN MUST be capable of
// receiving the following standard event signals: • SIMPLE •
// ELECTRICTY_PRICE with a signalType of price • LOAD_DISPATCH with a
// signalType of setpoint"
// Rule 104 and Table 1 give their units: none, currency/kWh and powerXXX.
// Rule 7 allows SIMPLE in either case. These are the only signals the VEN
// supports, and a connection accepts them when its accepted signals say so.
TEST_F(Openadr2bConformanceTest, Rule510_RequiredSignalsAreAccepted) {
  fixtures::EventSpec lower;
  lower.id = "simple";
  fixtures::EventSpec upper;
  upper.id = "SIMPLE";
  upper.signalName = "SIMPLE";
  fixtures::EventSpec price;
  price.id = "price";
  price.signalName = "ELECTRICITY_PRICE";
  price.signalType = "price";
  price.units = "currencyPerKWh";
  fixtures::EventSpec dispatch;
  dispatch.id = "dispatch";
  dispatch.signalName = "LOAD_DISPATCH";
  dispatch.signalType = "setpoint";
  dispatch.units = "powerReal";
  registerAndSync({lower, upper, price, dispatch});

  for (const auto *id : {"simple", "SIMPLE", "price", "dispatch"}) {
    const auto answers = answersFor(*http, id);
    ASSERT_EQ(answers.size(), 1u) << id;
    EXPECT_EQ(answers[0].code, "200") << id;
    EXPECT_EQ(answers[0].opt, "optIn") << id;
  }
}

// Rule 109, as below: a signal the VEN supports but the site cannot act on
// "cannot be currently supported". A connection accepts only SIMPLE unless
// it is told otherwise.
TEST_F(Openadr2bConformanceTest,
       Rule109_SignalsTheConnectionDoesNotAcceptAreAnsweredWith460) {
  acceptOnly(Openadr2bSettings{}.acceptedSignals);
  fixtures::EventSpec simple;
  simple.id = "simple";
  fixtures::EventSpec price;
  price.id = "price";
  price.signalName = "ELECTRICITY_PRICE";
  price.signalType = "price";
  price.units = "currencyPerKWh";
  fixtures::EventSpec mixed;
  mixed.id = "mixed";
  mixed.moreSignals = {
      {"LOAD_DISPATCH", "setpoint", "powerReal", {{"PT3H", 5.0}}}};
  registerAndSync({simple, price, mixed});

  const auto accepted = answersFor(*http, "simple");
  ASSERT_EQ(accepted.size(), 1u);
  EXPECT_EQ(accepted[0].code, "200");
  EXPECT_EQ(accepted[0].opt, "optIn");
  for (const auto *id : {"price", "mixed"}) {
    const auto answers = answersFor(*http, id);
    ASSERT_EQ(answers.size(), 1u) << id;
    EXPECT_EQ(answers[0].code, "460") << id;
    EXPECT_EQ(answers[0].opt, "optOut") << id;
    EXPECT_FALSE(store->findProgramEvent(id)) << id;
  }
  EXPECT_TRUE(store->findProgramEvent("simple"));
}

// Rule 48: "The following logical errors MUST be detected by implementations
// ... • VEN receives unsupported signalName (460). In case of a logical error
// for an event, optType is set to optOut."
TEST_F(Openadr2bConformanceTest,
       Rule48_UnsupportedSignalNameIsAnsweredWith460) {
  fixtures::EventSpec spec;
  spec.signalName = "LOAD_CONTROL";
  spec.signalType = "x-loadControlCapacity";
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "460");
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 109: "A VEN MUST generate an oadrCreatedEvent application level 460
// error if any requested signal in an event cannot be supported. Supported
// means that the requested combination of signalType and Unit cannot be
// currently supported."
TEST_F(Openadr2bConformanceTest,
       Rule109_UnsupportedSignalTypeIsAnsweredWith460) {
  fixtures::EventSpec spec;
  spec.signalName = "ELECTRICITY_PRICE";
  spec.signalType = "priceRelative";
  spec.units = "currencyPerKWh";
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "460");
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 109, as above: a supported signal in units we do not support.
TEST_F(Openadr2bConformanceTest, Rule109_UnsupportedUnitsAreAnsweredWith460) {
  fixtures::EventSpec spec;
  spec.signalName = "LOAD_DISPATCH";
  spec.signalType = "setpoint";
  spec.units = "energyReal";
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "460");
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 109, as above: "if any requested signal in an event cannot be
// supported".
TEST_F(Openadr2bConformanceTest,
       Rule109_OneUnsupportedSignalIsAnsweredWith460) {
  fixtures::EventSpec spec;
  spec.moreSignals = {{"BID_LOAD", "setpoint", "powerReal", {{"PT3H", 5.0}}}};
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "460");
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 33: "The implementation MUST provide an application layer error
// indication as a result of the following conditions: ... Illogical request –
// Old date on new event". An event is old when its whole window has passed.
TEST_F(Openadr2bConformanceTest,
       Rule33_NewEventThatHasAlreadyEndedIsAnsweredWithAnError) {
  fixtures::EventSpec spec;
  spec.start = "2020-01-01T00:00:00Z";
  spec.duration = "PT1H";
  spec.intervals = {{"PT1H", 1.0}};
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_TRUE(isError(answers[0].code)) << answers[0].code;
  EXPECT_EQ(answers[0].opt, "optOut");
}

// Rule 33, as above. Rule 47: "An event with an overall duration of 0
// indicates an event with no defined end time and will remain active until
// explicitly canceled", so an old start alone does not make it old. Passes
// today; it keeps the old-date check from refusing open-ended events.
TEST_F(Openadr2bConformanceTest,
       Rule33_OpenEndedEventWithAnOldStartIsAccepted) {
  fixtures::EventSpec spec;
  spec.start = "2020-01-01T00:00:00Z";
  spec.duration = "PT0S";
  spec.intervals = {{"PT0S", 1.0}};
  registerAndSync({spec});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "200");
  EXPECT_EQ(answers[0].opt, "optIn");
}

// OpenLEADR writes a zero duration as "P", which the 2.0b schema allows. An
// open-ended event from an OpenLEADR VTN has to be understood, not fail the
// whole distribution.
TEST_F(Openadr2bConformanceTest, OpenEndedEventWithBareDurationIsAccepted) {
  fixtures::EventSpec spec;
  spec.duration = "P";
  spec.intervals = {{"P", 1.0}};
  registerAndSync({spec});

  const auto stored = db->event(1);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->durationSeconds, 0);
  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].code, "200");
  EXPECT_EQ(answers[0].opt, "optIn");
}

// As above, for a one-shot report request from an OpenLEADR VTN.
TEST_F(Openadr2bConformanceTest, OneShotReportWithBareDurationIsSent) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::createStatusReport(
                            "rr-once", isoTime(clock.time), "P"));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);
  clock.time += 120;
  adapter->runDueWork(*sink);

  EXPECT_EQ(updatesFor(*http, "rr-once").size(), 1u);
}

// Rule 510: "A VEN MUST be capable of producing TELEMETRY_USAGE reports, at
// least for certification ... The device MUST be able to send some telemetry
// data (i.e., in case it does not have any metering resources attached, it
// MUST provide sample data)."
// Rule 331: METADATA_TELEMETRY_USAGE has reportType usage, units powerReal,
// energyReal or pulseCount, and readingType Direct Read.
// Rule 333: "The reportDescription:oadrSamplingRate element is REQUIRED for
// any of the telemetry reports". Rule 312: the Metadata report includes
// duration, and "oadrSamplingRate:oadrMaxPeriod MUST NOT be larger than the
// duration". Rule 308: reportSpecifierIDs are unique within the Metadata.
TEST_F(Openadr2bConformanceTest, Rule510_MetadataOffersTelemetryUsage) {
  offerReports({usageOffer()});
  registerAndSync();
  const auto reports = metadataReports(*http);
  const auto *usage = findReport(reports, "METADATA_TELEMETRY_USAGE");
  ASSERT_NE(usage, nullptr);
  const auto *status = findReport(reports, "METADATA_TELEMETRY_STATUS");
  ASSERT_NE(status, nullptr);
  EXPECT_NE(usage->reportSpecifierID(), status->reportSpecifierID());

  EXPECT_EQ(usage->reportRequestID(), "0");
  ASSERT_TRUE(usage->duration().present());
  const auto duration = seconds(usage->duration()->duration());
  ASSERT_FALSE(usage->oadrReportDescription().empty());
  EXPECT_EQ(usage->oadrReportDescription().size(), 2u);
  for (const auto &description : usage->oadrReportDescription()) {
    EXPECT_EQ(static_cast<const std::string &>(description.reportType()),
              "usage");
    EXPECT_TRUE(description.itemBase().present());
    ASSERT_TRUE(description.reportDataSource().present());
    EXPECT_EQ(description.reportDataSource()->resourceID().front(), "SITE-1");
    ASSERT_TRUE(description.oadrSamplingRate().present());
    EXPECT_LE(seconds(description.oadrSamplingRate()->oadrMaxPeriod()),
              duration);
  }
}

// Rule 510, as above. The values come from the site's datums.
// Rule 331: TELEMETRY_USAGE data is an oadrReportPayload with a payloadFloat.
// Rule 317: a granularity of one minute over a two minute report-back
// period gives two intervals.
TEST_F(Openadr2bConformanceTest, Rule510_TelemetryUsageIsReportedFromDatums) {
  offerReports({usageOffer()});
  registerAndSync();
  // Two minutes of datums before the clock: minute one averages 100 W and
  // uses 10 Wh, minute two averages 300 W and uses 20 Wh. The datum at the
  // clock is the reading at the end of the window.
  addDatum(180, 50, 990);
  addDatum(120, 100, 1000);
  addDatum(90, 100, 1005);
  addDatum(60, 200, 1010);
  addDatum(30, 400, 1020);
  addDatum(0, 900, 1030);

  fixtures::ReportRequestSpec spec{"rr-usage", "usage-1", "power", "PT2M",
                                   isoTime(clock.time), "PT1M"};
  spec.moreRids = {"energy"};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  EXPECT_EQ(createdReportCodes(*http), std::vector<std::string>{"200"});
  const auto updates = updatesFor(*http, "rr-usage");
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(*updates[0].reportName(), "TELEMETRY_USAGE");
  const auto intervals = floatPayloads(updates[0]);
  ASSERT_EQ(intervals.size(), 2u);
  ASSERT_EQ(intervals[0].size(), 2u);
  EXPECT_EQ(intervals[0][0].rid, "power");
  EXPECT_FLOAT_EQ(intervals[0][0].value, 100);
  EXPECT_EQ(intervals[0][0].quality, "Quality Good - Non Specific");
  EXPECT_EQ(intervals[0][1].rid, "energy");
  EXPECT_FLOAT_EQ(intervals[0][1].value, 10);
  EXPECT_FLOAT_EQ(intervals[1][0].value, 300);
  EXPECT_FLOAT_EQ(intervals[1][1].value, 20);

  // The request and the delivery are on record.
  const auto requests = db->reportRequestsFor(connection);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].request.programRequestId, "rr-usage");
  EXPECT_EQ(requests[0].request.kind, ReportRequestKind::Periodic);
  EXPECT_EQ(requests[0].request.offerId, 7);
  EXPECT_EQ(requests[0].request.granularitySeconds, 60);
  EXPECT_EQ(requests[0].request.reportBackSeconds, 120);
  EXPECT_EQ(requests[0].request.rids, (std::vector<std::string>{"power", "energy"}));
  EXPECT_EQ(requests[0].status, ReportRequestStatus::Active);
  const auto deliveries = db->reportDeliveriesFor(connection);
  ASSERT_EQ(deliveries.size(), 1u);
  EXPECT_TRUE(deliveries[0].ok);
  EXPECT_EQ(deliveries[0].payload.size(), 4u);
  EXPECT_EQ(deliveries[0].payload[1]["rid"], "energy");
  EXPECT_EQ(deliveries[0].payload[1]["value"], 10.0);
  EXPECT_EQ(deliveries[0].payload[1]["duration_seconds"], 60);
  // Each value says where it came from, and the delivery records the read
  // behind the values: one request for the stream both points share.
  EXPECT_EQ(deliveries[0].payload[1]["source"]["property"], "wattHours");
  EXPECT_EQ(deliveries[0].payload[1]["source"]["reduce"], "difference");
  EXPECT_EQ(deliveries[0].payload[0]["source"]["property"], "watts");
  ASSERT_EQ(deliveries[0].reads.size(), 1u);
  EXPECT_EQ(deliveries[0].reads[0]["kind"], "list");
  EXPECT_EQ(deliveries[0].reads[0]["path"], "/fake/list");
  EXPECT_EQ(deliveries[0].reads[0]["query"]["sourceId"], "/S/1");
  EXPECT_EQ(deliveries[0].reads[0]["datum_count"], 6);
  EXPECT_TRUE(deliveries[0].reads[0]["error"].is_null());
  // The window reaches one granularity back for a baseline and one second
  // past the end for the closing reading.
  EXPECT_EQ(deliveries[0].reads[0]["start"],
            toIso8601(std::chrono::system_clock::from_time_t(clock.time - 180)));
  EXPECT_EQ(deliveries[0].reads[0]["end"],
            toIso8601(std::chrono::system_clock::from_time_t(clock.time + 1)));
}

// Rule 21: "If venID, vtnID, or eventID value is included in the payload, the
// receiving entity MUST validate that the ID value is as expected and generate
// an error if an unexpected value is received."
// Rule 400: "If the ID does not match the expected ID, the device SHOULD
// include a 452 error in eiResponse."
TEST_F(Openadr2bConformanceTest,
       Rule21_ReregistrationRequestForAnotherVenIsRejected) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::requestReregistration("ven-other"));
  EXPECT_EQ(adapter->poll(*sink).outcome, PollOutcome::Ok);

  std::vector<std::string> codes;
  for (const auto &payload : sent(*http)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrResponse().present()) {
      codes.push_back(message.oadrResponse()->eiResponse().responseCode());
    }
  }
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_EQ(codes[0], "452");
}

// Rule 21, as above.
// Rule 304: "The following IDs SHOULD be validated by the implementations
// receiving the payload. If the ID does not match the expected ID, the device
// SHOULD include a 452 error in the eiResponse payload. • venID ..."
TEST_F(Openadr2bConformanceTest,
       Rule21_ReportRequestForAnotherVenIsRejected) {
  registerAndSync();
  http->queue(oadrPoll,
              fixtures::createReport({{"rr-1", "TELEMETRY_STATUS", "status",
                                       "PT1M", isoTime(clock.time)}},
                                     "create-report-1", "ven-other"));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  EXPECT_EQ(createdReportCodes(*http), std::vector<std::string>{"452"});
  EXPECT_TRUE(updatesFor(*http, "rr-1").empty());
  EXPECT_TRUE(db->reportRequestsFor(connection).empty());
}

// Rule 21 and rule 304, as above.
TEST_F(Openadr2bConformanceTest,
       Rule21_ReportCancellationForAnotherVenIsRejected) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  http->queue(oadrPoll, fixtures::cancelReport("rr-1", false, "ven-other"));
  adapter->poll(*sink);

  std::vector<std::string> codes;
  for (const auto &payload : sent(*http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrCanceledReport().present()) {
      codes.push_back(message.oadrCanceledReport()->eiResponse().responseCode());
    }
  }
  EXPECT_EQ(codes, std::vector<std::string>{"452"});
  // The report goes on.
  adapter->runDueWork(*sink);
  EXPECT_EQ(updatesFor(*http, "rr-1").size(), 1u);
}

// Rule 21 and rule 304, as above. The VTN's own Metadata is acknowledged
// when it is for this VEN.
TEST_F(Openadr2bConformanceTest,
       Rule21_ReportRegistrationForAnotherVenIsRejected) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::registerReport("ven-other"));
  adapter->poll(*sink);
  http->queue(oadrPoll, fixtures::registerReport("ven-1"));
  adapter->poll(*sink);

  std::vector<std::string> codes;
  for (const auto &payload : sent(*http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrRegisteredReport().present()) {
      codes.push_back(
          message.oadrRegisteredReport()->eiResponse().responseCode());
    }
  }
  EXPECT_EQ(codes, (std::vector<std::string>{"452", "200"}));
}

// Section 8.6 lists oadrUpdateReport among the payloads a VTN may answer
// oadrPoll with. Rule 500: "If a logical response is required by the VEN to
// the received operational payload, the VEN MUST send that logical response
// asynchronously via a transport request." Section 8.3.2.3: "The response
// sent by the target party uses the oadrUpdatedReport payload to acknowledge
// receipt of the report."
TEST_F(Openadr2bConformanceTest, Rule500_ReportFromTheProgramIsAnswered) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::updateReport("update-report-1"));
  adapter->poll(*sink);

  std::vector<std::string> requestIds;
  std::vector<std::string> codes;
  for (const auto &payload : sent(*http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrUpdatedReport().present()) {
      requestIds.push_back(
          message.oadrUpdatedReport()->eiResponse().requestID());
      codes.push_back(message.oadrUpdatedReport()->eiResponse().responseCode());
    }
  }
  // Rule 303: the response carries the request's requestID.
  EXPECT_EQ(requestIds, std::vector<std::string>{"update-report-1"});
  // Rule 304: the VEN requested no reports, so the reportRequestID is not one
  // it expects.
  EXPECT_EQ(codes, std::vector<std::string>{"452"});
}

// Rule 9: "oadrDistributeEvent eiEventSignal's with a signalName of 'SIMPLE'
// MUST use signalPayload values of 0=normal; 1=moderate; 2=high; 3=special."
// Rule 48: "When a VTN or VEN receives schema compliant OpenADR payload that
// has logical errors, the receiving device MUST provide an application layer
// error indication of 4xx ... In case of a logical error for an event, optType
// is set to optOut."
TEST_F(Openadr2bConformanceTest,
       Rule9_SimpleLevelOutOfRangeIsAnsweredWithAnError) {
  registerAndSync();
  fixtures::EventSpec event;
  event.intervals = {{"PT3H", 7.0}};
  deliver("req-2", {event});

  const auto answers = answersFor(*http, "event-1");
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_TRUE(isError(answers[0].code)) << answers[0].code;
  EXPECT_EQ(answers[0].opt, "optOut");
  EXPECT_FALSE(db->event(1).has_value());
}

// Section 9.1.5: "404 Not Found – the VEN or VTN does not support the
// requested operation. The requestor must not re-send the request." and
// "406 Not Acceptable – ... The requestor must not re-send the request without
// first modifying it."
TEST_F(Openadr2bConformanceTest, Section9_1_5_NotFoundAndNotAcceptableAreFinal) {
  registerAndSync();
  http->queue(oadrPoll, "", "404");
  EXPECT_EQ(kindOf([&] { adapter->poll(*sink); }),
            AdapterError::Kind::Permanent);
  http->queue(oadrPoll, "", "406");
  EXPECT_EQ(kindOf([&] { adapter->poll(*sink); }),
            AdapterError::Kind::Permanent);
}

// Rule 328: "VTN or VEN MAY include oadrCancelReport object in
// oadrUpdatedReport payload to enable the implementation receiving the report
// to cancel a report as part of its response. When receiving such a payload,
// VTN or VEN MUST process it as defined in Section 8.3.2.3."
// Section 8.3.2.3: "As part of the oadrUpdatedReport response the target
// party may cancel the sending of any future reports using the optional
// oadrCancelReport object ... There is no confirmation of the cancellation".
TEST_F(Openadr2bConformanceTest,
       Rule328_CancellationInTheUpdatedReportStopsTheReport) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);

  http->queue(eiReport, fixtures::updatedReport("200", "OK", "rr-1"));
  adapter->runDueWork(*sink);
  ASSERT_EQ(updatesFor(*http, "rr-1").size(), 1u);

  clock.time += 60;
  adapter->runDueWork(*sink);
  clock.time += 60;
  adapter->runDueWork(*sink);

  EXPECT_EQ(updatesFor(*http, "rr-1").size(), 1u);
  const auto requests = db->reportRequestsFor(connection);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].status, ReportRequestStatus::Cancelled);
}

// Rule 325: "For all history reports (HISTORY_XXX), this duration is in the
// PAST and reflects the historical data requested. If this value is 0 then the
// report MUST include all the history from dtstart time."
// Rule 324: "If reportBackDuration is 0, then the report MUST be sent
// immediately and only once".
TEST_F(Openadr2bConformanceTest,
       Rule325_ZeroDurationHistoryReportsEverythingSinceDtstart) {
  offerReports({usageOffer("HISTORY_USAGE", 3600)});
  registerAndSync();
  for (long ago = 600; ago > 0; ago -= 60) {
    addDatum(ago, static_cast<double>(ago), 0);
  }

  fixtures::ReportRequestSpec spec{"rr-hist", "usage-1", "power", "PT0S",
                                   isoTime(clock.time - 600), "PT0S", "PT0S"};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  EXPECT_EQ(createdReportCodes(*http), std::vector<std::string>{"200"});
  const auto updates = updatesFor(*http, "rr-hist");
  ASSERT_EQ(updates.size(), 1u);
  const auto intervals = floatPayloads(updates[0]);
  ASSERT_EQ(intervals.size(), 10u);
  EXPECT_FLOAT_EQ(intervals[0].at(0).value, 600);
  EXPECT_FLOAT_EQ(intervals[9].at(0).value, 60);
}

// Rule 340: "If a source party requests more data from the target party in an
// oadrReportRequest (e.g., based on the requested duration), the target party
// MUST return an application error code 454 in an
// oadrCreatedReport:eiResponse."
// Rule 325: a zero duration asks for "all the history from dtstart time", so
// a dtstart two hours back asks for two hours of an hour's history.
TEST_F(Openadr2bConformanceTest,
       Rule340_ZeroDurationHistoryBeyondTheOfferIsRefused) {
  offerReports({usageOffer("HISTORY_USAGE", 3600)});
  registerAndSync();

  fixtures::ReportRequestSpec spec{"rr-hist", "usage-1", "power", "PT0S",
                                   isoTime(clock.time - 7200), "PT0S", "PT0S"};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  EXPECT_EQ(createdReportCodes(*http), std::vector<std::string>{"454"});
  EXPECT_TRUE(db->reportRequestsFor(connection).empty());
}

// Rule 306: "An implementation receiving a Metadata report ... MUST also
// implicitly cancel all previously scheduled reports, except for periodic
// requests for metadata reports, which MUST be explicitly canceled. If
// implementations receiving a Metadata report wish to continue to receive a
// previously requested report(s), it MUST send a new report request after the
// receipt of the Metadata report."
// Answering a METADATA request sends the program a Metadata report, so the
// program has cancelled everything it asked for before.
TEST_F(Openadr2bConformanceTest,
       Rule306_AnsweringAMetadataRequestEndsEarlierRequests) {
  registerAndSync();
  http->queue(oadrPoll, fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);
  ASSERT_EQ(updatesFor(*http, "rr-1").size(), 1u);

  http->queue(oadrPoll, fixtures::createReport(
                            {{"rr-meta", "METADATA", "0", "PT0S", ""}}));
  adapter->poll(*sink);
  clock.time += 60;
  adapter->runDueWork(*sink);
  clock.time += 60;
  adapter->runDueWork(*sink);

  EXPECT_EQ(updatesFor(*http, "rr-1").size(), 1u);
  const auto requests = db->reportRequestsFor(connection);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].status, ReportRequestStatus::Cancelled);
}

// Rule 318: "If an implementation has no valid data for a data point which
// was included in its Metadata report and it has been asked to report this
// value via a report request, the implementation MUST include a placeholder
// value in the report and set the
// intervals:interval:oadrReportPayload:dataQuality element to one of the
// 'Quality Bad' enumerated values."
// "No Quality - No Value" is not one of them.
TEST_F(Openadr2bConformanceTest, Rule318_MissingDataIsMarkedQualityBad) {
  offerReports({usageOffer()});
  registerAndSync();

  fixtures::ReportRequestSpec spec{"rr-usage", "usage-1", "power", "PT1M",
                                   isoTime(clock.time)};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  const auto intervals = floatPayloads(updatesFor(*http, "rr-usage").at(0));
  ASSERT_EQ(intervals.size(), 1u);
  ASSERT_EQ(intervals[0].size(), 1u);
  EXPECT_EQ(intervals[0][0].quality.rfind("Quality Bad - ", 0), 0u)
      << intervals[0][0].quality;
}

// Rule 331: "When the well-known report names are used as part of the
// oadrRegisterReport, they MUST be prefixed with 'METADATA_' and the values
// shown in the table MUST be used for the specified report." TELEMETRY_USAGE
// and HISTORY_USAGE: reportType usage, units powerReal, energyReal or
// pulseCount, readingType Direct Read.
// Rule 308: "If a reportDescription in the Metadata report has a reportName
// element that contains one of the well known alliance report profile names,
// the data points characteristics and allowable values MUST conform to the
// table shown in conformance rule 331."
// An offer that would break the profile is a configuration error, as an offer
// in terms OpenADR lacks already is.
TEST_F(Openadr2bConformanceTest,
       Rule331_WellKnownReportsMustUseTheProfilesTerms) {
  auto summed = usageOffer();
  summed.dataPoints = {summed.dataPoints[0]};
  summed.dataPoints[0].readingType = "Summed";
  EXPECT_EQ(kindOf([&] { offerReports({summed}); }),
            AdapterError::Kind::Permanent);

  auto demand = usageOffer("HISTORY_USAGE");
  demand.dataPoints = {demand.dataPoints[0]};
  demand.dataPoints[0].reportType = "demand";
  EXPECT_EQ(kindOf([&] { offerReports({demand}); }),
            AdapterError::Kind::Permanent);

  auto temperature = usageOffer();
  temperature.dataPoints = {temperature.dataPoints[0]};
  temperature.dataPoints[0].item.kind = "temperature";
  temperature.dataPoints[0].item.units = "celsius";
  EXPECT_EQ(kindOf([&] { offerReports({temperature}); }),
            AdapterError::Kind::Permanent);
}

// Rule 308: "All rID element values MUST be unique within a specific
// reportSpecifierID."
TEST_F(Openadr2bConformanceTest, Rule308_RidsMustBeUniqueWithinAReport) {
  auto offer = usageOffer();
  offer.dataPoints[1].rid = offer.dataPoints[0].rid;
  EXPECT_EQ(kindOf([&] { offerReports({offer}); }),
            AdapterError::Kind::Permanent);
}

// ---------------------------------------------------------------------------
// Conformance rules the VEN already meets. These pass today and keep it that
// way.

// Rule 1: "The date-time values MUST be specified using [ISO8601] utc-time
// (also called zulu time). ... Although [ISO8601] allows representation of
// Zulu time with a zero hour offset (e.g, 2013-04-22T15:26:44.123+0000), it
// MUST NOT be used."
TEST_F(Openadr2bConformanceTest, Rule1_OutboundDateTimesAreZulu) {
  runSession();

  const std::regex dateTime(
      "<(?:\\w+:)?(?:date-time|createdDateTime)>([^<]*)<");
  const std::regex zulu("\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z");
  int found = 0;
  for (const auto &request : http->requests) {
    for (std::sregex_iterator
             it(request.body.begin(), request.body.end(), dateTime),
         end;
         it != end; ++it) {
      ++found;
      EXPECT_TRUE(std::regex_match((*it)[1].str(), zulu)) << (*it)[1];
    }
  }
  EXPECT_GT(found, 0);
}

// Rule 1: "Representation of duration also follows [ISO8601]. However, use of
// decimal values MUST NOT be used in OpenADR."
TEST_F(Openadr2bConformanceTest, Rule1_OutboundDurationsHaveNoDecimals) {
  runSession();

  const std::regex duration(
      "<(?:\\w+:)?(?:duration|oadrMinPeriod|oadrMaxPeriod)>(P[^<]*)<");
  int found = 0;
  for (const auto &request : http->requests) {
    for (std::sregex_iterator
             it(request.body.begin(), request.body.end(), duration),
         end;
         it != end; ++it) {
      ++found;
      EXPECT_EQ((*it)[1].str().find_first_of(".,"), std::string::npos)
          << (*it)[1];
    }
  }
  EXPECT_GT(found, 0);
}

// Rule 45: "Messages sent between VENs and VTNs MUST NOT include a
// schemaLocation attribute."
TEST_F(Openadr2bConformanceTest, Rule45_OutboundMessagesHaveNoSchemaLocation) {
  runSession();
  for (const auto &request : http->requests) {
    EXPECT_EQ(request.body.find("schemaLocation"), std::string::npos)
        << request.body;
  }
}

// Rule 508: "All payloads sent by a VEN to a VTN MUST contain a valid venID."
TEST_F(Openadr2bConformanceTest, Rule508_OutboundMessagesCarryTheVenId) {
  runSession();
  for (const auto &request : http->requests) {
    EXPECT_NE(request.body.find("venID>ven-1<"), std::string::npos)
        << request.body;
  }
}

// Rule 509: "The optional schemaVersion attribute MUST be included in all
// payloads exchanged. The value of the schemaVersion attribute MUST be either
// '2.0a' or '2.0b'".
TEST_F(Openadr2bConformanceTest, Rule509_OutboundMessagesDeclareSchemaVersion) {
  runSession();
  for (const auto &request : http->requests) {
    EXPECT_NE(request.body.find("schemaVersion=\"2.0b\""), std::string::npos)
        << request.body;
  }
}

// Rule 511: "B Profile endpoints MUST use the following template in the
// simple HTTP mode ... https://<hostname>(:port)/(prefix/)OpenADR2/Simple/
// 2.0b/<service>"
TEST_F(Openadr2bConformanceTest, Rule511_EndpointsFollowTheUrlTemplate) {
  Openadr2bSettings settings;
  settings.vtnUrl = "https://vtn.example.com:8443/prefix/";
  settings.venName = "site-ven";
  settings.venId = "ven-1";
  Openadr2bAdapter prefixed(db->connections.at(connection), settings, *store,
                            std::make_unique<ScriptedHttp>(http),
                            [this] { return clock.time; });
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-1", "reg-1"));
  prefixed.registerConnection(*sink);
  http->queue(eiEvent, fixtures::distributeEvent("req-sync", {}));
  prefixed.poll(*sink);

  std::set<std::string> services;
  for (const auto &request : http->requests) {
    services.insert(request.endpoint);
    EXPECT_EQ(request.url,
              "https://vtn.example.com:8443/prefix/OpenADR2/Simple/2.0b/" +
                  request.endpoint);
  }
  EXPECT_EQ(services, (std::set<std::string>{registerParty, eiReport, eiEvent,
                                             oadrPoll}));
}

// Rule 402: "If the oadrTransportName is simpleHTTP, then oadrHttpPullModel
// MUST be set to true or false. If oadrHttpPullModel is set to false ... the
// oadrTransportAddress MUST be included" and oadrCreatePartyRegistration "MUST
// NOT include ... registrationID – for a new registration".
TEST_F(Openadr2bConformanceTest,
       Rule402_NewRegistrationDeclaresSimpleHttpPull) {
  http->queue(registerParty,
              fixtures::createdPartyRegistration("ven-1", "reg-1"));
  adapter->registerConnection(*sink);

  const auto registrations = sent(*http, registerParty);
  ASSERT_EQ(registrations.size(), 1u);
  const auto &message = registrations[0]->oadrSignedObject();
  ASSERT_TRUE(message.oadrCreatePartyRegistration().present());
  const auto &create = *message.oadrCreatePartyRegistration();
  EXPECT_EQ(static_cast<const std::string &>(create.oadrProfileName()), "2.0b");
  EXPECT_EQ(static_cast<const std::string &>(create.oadrTransportName()),
            "simpleHttp");
  ASSERT_TRUE(create.oadrHttpPullModel().present());
  EXPECT_TRUE(*create.oadrHttpPullModel());
  EXPECT_FALSE(create.registrationID().present());
}

// Rule 309: "Upon power-up or after a system reset (followed by an initial
// registration) PUSH and PULL implementations of both VTN and VEN MUST send an
// oadrRegisterReport payload ... prior to initiating other non-registration
// service operations except an oartRequestEvent payload".
// Rule 405: "After a completed new registration, VENs and VTNs MUST exchange
// their Metadata reports, and VENs ... must use oadrRequestEvent to obtain all
// relevant events prior to initiating other payload exchanges."
TEST_F(Openadr2bConformanceTest,
       Rule309_ReportsAndEventsAreSyncedBeforePolling) {
  registerAndSync();

  const auto indexOf = [&](const std::string &endpoint,
                           const std::string &text) {
    for (size_t i = 0; i < http->requests.size(); ++i) {
      const auto &request = http->requests[i];
      if (request.endpoint == endpoint &&
          request.body.find(text) != std::string::npos) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  const int registerReport = indexOf(eiReport, "oadrRegisterReport");
  const int requestEvent = indexOf(eiEvent, "oadrRequestEvent");
  const int firstPoll = indexOf(oadrPoll, "oadrPoll");
  ASSERT_GE(registerReport, 0);
  ASSERT_GE(requestEvent, 0);
  ASSERT_GE(firstPoll, 0);
  EXPECT_LT(registerReport, firstPoll);
  EXPECT_LT(requestEvent, firstPoll);
}

// Rule 42: "A VEN receiving and event in an oadrDistributeEvent payload MUST
// use the received requestID value in the eiCreatedEvent:eventResponse ...
// The eiResponse:requestID in oadrCreatedEvent MUST be left empty if the
// payload contains eventResponses."
TEST_F(Openadr2bConformanceTest,
       Rule42_CreatedEventCarriesTheDistributionRequestId) {
  registerAndSync({fixtures::EventSpec{}});

  int created = 0;
  for (const auto &payload : sent(*http, eiEvent)) {
    const auto &message = payload->oadrSignedObject();
    if (!message.oadrCreatedEvent().present()) {
      continue;
    }
    ++created;
    const auto &event = message.oadrCreatedEvent()->eiCreatedEvent();
    EXPECT_EQ(event.eiResponse().requestID(), "");
    ASSERT_TRUE(event.eventResponses().present());
    for (const auto &response : event.eventResponses()->eventResponse()) {
      EXPECT_EQ(response.requestID(), "req-sync");
    }
  }
  EXPECT_EQ(created, 1);
}

// Rule 59: "If the VTN sends an oadrEvent object with the eventStatus set to
// 'cancelled' and has an eventID that the VEN is aware of then the VEN SHOULD
// cancel the existing event".
TEST_F(Openadr2bConformanceTest, Rule59_CancellingAKnownEventCancelsIt) {
  registerAndSync({fixtures::EventSpec{}});
  fixtures::EventSpec cancelled;
  cancelled.modification = 1;
  cancelled.status = "cancelled";
  deliver("req-2", {cancelled});

  EXPECT_EQ(db->event(1)->status, ProgramStatus::Cancelled);
}

// Rule 331: METADATA_TELEMETRY_STATUS has reportType x-resourceStatus, no
// units and readingType x-notApplicable.
// Rule 305: a Metadata report MUST NOT include intervals or dtstart.
// Rule 311: "The oadrRegisterReport.oadrReport.reportRequestID element in each
// contained metadata report MUST be set to zero", and an unsolicited
// oadrRegisterReport MUST NOT include oadrRegisterReport.reportRequestID.
// Rules 312 and 333: duration and oadrSamplingRate are included, with
// oadrMaxPeriod no larger than duration.
// Rule 510: TELEMETRY_STATUS covers "an attached resource identified with a
// resourceID target".
TEST_F(Openadr2bConformanceTest, Rule331_StatusMetadataFollowsTheProfile) {
  registerAndSync();

  for (const auto &payload : sent(*http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrRegisterReport().present()) {
      EXPECT_FALSE(message.oadrRegisterReport()->reportRequestID().present());
    }
  }
  const auto reports = metadataReports(*http);
  const auto *status = findReport(reports, "METADATA_TELEMETRY_STATUS");
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->reportRequestID(), "0");
  EXPECT_FALSE(status->dtstart().present());
  EXPECT_FALSE(status->intervals().present());
  ASSERT_TRUE(status->duration().present());
  ASSERT_EQ(status->oadrReportDescription().size(), 1u);
  const auto &description = status->oadrReportDescription()[0];
  EXPECT_EQ(static_cast<const std::string &>(description.reportType()),
            "x-resourceStatus");
  EXPECT_EQ(static_cast<const std::string &>(description.readingType()),
            "x-notApplicable");
  EXPECT_FALSE(description.itemBase().present());
  ASSERT_TRUE(description.oadrSamplingRate().present());
  EXPECT_LE(seconds(description.oadrSamplingRate()->oadrMaxPeriod()),
            seconds(status->duration()->duration()));
  ASSERT_TRUE(description.reportDataSource().present());
  EXPECT_FALSE(description.reportDataSource()->resourceID().empty());
}

// Rule 313: a non-Metadata report MUST include dtstart, intervals and the
// reportName without the METADATA prefix, and MUST NOT include
// oadrReportDescription.
// Rule 314: "oadrReport:dtstart MUST be included in all oadrUpdateReport
// payloads."
// Rule 510: TELEMETRY_STATUS carries "the mandatory data points of oadrOnline
// and oadrManualOverride".
TEST_F(Openadr2bConformanceTest, Rule313_StatusReportCarriesItsData) {
  registerAndSync();
  http->queue(oadrPoll,
              fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  const auto reports = updatesFor(*http, "rr-1");
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_TRUE(reports[0].dtstart().present());
  ASSERT_TRUE(reports[0].intervals().present());
  EXPECT_FALSE(reports[0].intervals()->interval().empty());
  ASSERT_TRUE(reports[0].reportName().present());
  EXPECT_EQ(*reports[0].reportName(), "TELEMETRY_STATUS");
  EXPECT_TRUE(reports[0].oadrReportDescription().empty());

  const auto updates = http->requestsContaining(eiReport, "oadrUpdateReport");
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_NE(updates[0].body.find("oadrOnline>"), std::string::npos);
  EXPECT_NE(updates[0].body.find("oadrManualOverride>"), std::string::npos);
}

// Rule 303: "if a value is specified for requestID in the request payload,
// the responding implementation MUST return that value in its response
// payload."
// Rule 329: "oadrPendingReports element MUST contain a list of
// reportRequestID element values that includes all reports that are scheduled
// for future delivery."
TEST_F(Openadr2bConformanceTest, Rule303_ReportResponsesEchoTheRequestId) {
  registerAndSync();
  http->queue(oadrPoll,
              fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  http->queue(oadrPoll, fixtures::cancelReport("rr-1"));
  adapter->poll(*sink);

  int created = 0;
  int canceled = 0;
  for (const auto &payload : sent(*http, eiReport)) {
    const auto &message = payload->oadrSignedObject();
    if (message.oadrCreatedReport().present()) {
      ++created;
      const auto &report = *message.oadrCreatedReport();
      EXPECT_EQ(report.eiResponse().requestID(), "create-report-1");
      const auto &pending = report.oadrPendingReports().reportRequestID();
      EXPECT_EQ(std::count(pending.begin(), pending.end(), "rr-1"), 1);
    }
    if (message.oadrCanceledReport().present()) {
      ++canceled;
      const auto &report = *message.oadrCanceledReport();
      EXPECT_EQ(report.eiResponse().requestID(), "cancel-report-1");
      const auto &pending = report.oadrPendingReports().reportRequestID();
      EXPECT_EQ(std::count(pending.begin(), pending.end(), "rr-1"), 0);
    }
  }
  EXPECT_EQ(created, 1);
  EXPECT_EQ(canceled, 1);
}

// Rule 345: "If reportToFollow is set to true by the report requester in
// oadrCancelReport object canceling periodic reports, the report sender MUST
// send one final additional report to the report requester."
TEST_F(Openadr2bConformanceTest, Rule345_ReportToFollowSendsOneFinalReport) {
  registerAndSync();
  http->queue(oadrPoll,
              fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);
  ASSERT_EQ(updatesFor(*http, "rr-1").size(), 1u);

  http->queue(oadrPoll, fixtures::cancelReport("rr-1", true));
  adapter->poll(*sink);
  for (int i = 0; i < 3; ++i) {
    clock.time += 120;
    adapter->runDueWork(*sink);
  }

  EXPECT_EQ(updatesFor(*http, "rr-1").size(), 2u);
}

// Rule 325: "If reportSpecifier:reportInterval:properties:duration value is 0
// then the report MUST be generated indefinitely."
TEST_F(Openadr2bConformanceTest, Rule325_ZeroDurationReportsRunIndefinitely) {
  registerAndSync();
  // The request's reportInterval duration is PT0S.
  http->queue(oadrPoll,
              fixtures::createStatusReport("rr-1", isoTime(clock.time)));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);
  clock.time += 30 * 24 * 3600;
  adapter->runDueWork(*sink);

  EXPECT_GE(updatesFor(*http, "rr-1").size(), 2u);
}

// Rule 510: "A VEN MUST be capable of utilizing the EiOpt service to further
// qualify the opt state of an event."
// Rule 206: "further qualifications of the optType state for the event can be
// accomplished by sending either an oadrCreatedEvent or an EiOpt
// oadrCreateOpt payload with the eventID specified ... When eventID is
// specified in oadrCreateOpt, the VEN MUST NOT include a vavailablity or
// marketContext in the payload." With no target, or only the venID, "all
// resources associated with the qualifiedEvent MUST have its opt state
// altered".
// Rule 204: "The VEN MUST send an optID value as part of the oadrCreateOpt
// payload".
// Rule 208: venID and optID "MUST be specified by the VEN".
TEST_F(Openadr2bConformanceTest, Rule510_OperatorOptChangeIsSentWithEiOpt) {
  registerAndSync({fixtures::EventSpec{}});
  adapter->respond(*db->event(1), Opt::Out, *sink);

  const auto opts = createOpts(*http);
  ASSERT_EQ(opts.size(), 1u);
  const auto &opt = opts[0];
  ASSERT_TRUE(opt.qualifiedEventID().present());
  EXPECT_EQ(opt.qualifiedEventID()->eventID(), "event-1");
  EXPECT_EQ(opt.qualifiedEventID()->modificationNumber(), 0u);
  EXPECT_EQ(static_cast<const std::string &>(opt.optType()), "optOut");
  EXPECT_FALSE(opt.optID().empty());
  EXPECT_EQ(opt.venID(), "ven-1");
  EXPECT_FALSE(opt.vavailability().present());
  EXPECT_FALSE(opt.marketContext().present());
  EXPECT_FALSE(opt.oadrDeviceClass().present());

  const auto &target = opt.eiTarget();
  for (const auto &venId : target.venID()) {
    EXPECT_EQ(venId, "ven-1");
  }
  EXPECT_TRUE(target.resourceID().empty());
  EXPECT_TRUE(target.groupID().empty());
  EXPECT_TRUE(target.partyID().empty());
  EXPECT_TRUE(target.endDeviceAsset().empty());

  // Only the automatic answer from the sync went out as oadrCreatedEvent.
  EXPECT_EQ(http->requestsContaining(eiEvent, "oadrCreatedEvent").size(), 1u);
  ASSERT_FALSE(db->responsesFor(1).empty());
  EXPECT_EQ(db->responsesFor(1).back().answer, EventAnswer::OptOut);
  EXPECT_TRUE(db->responsesFor(1).back().ok);
}

// Rule 206: "Once the optType state for a certain resource is qualified with
// oadrCreateOpt, subsequent changes to the optType state for the specific
// resource can only be made by another oadrCreateOpt payload."
TEST_F(Openadr2bConformanceTest, Rule206_LaterOptChangesAlsoUseEiOpt) {
  registerAndSync({fixtures::EventSpec{}});
  adapter->respond(*db->event(1), Opt::Out, *sink);
  adapter->respond(*db->event(1), Opt::In, *sink);

  const auto opts = createOpts(*http);
  ASSERT_EQ(opts.size(), 2u);
  EXPECT_EQ(static_cast<const std::string &>(opts[1].optType()), "optIn");
  EXPECT_EQ(http->requestsContaining(eiEvent, "oadrCreatedEvent").size(), 1u);
}

// Rule 12: "Never – The VEN MUST NOT respond to the event with a
// oadrCreatedEvent eventResponse".
// Rule 206: "oadrCreateOpt may not be sent to qualify an event if
// oadrResponseRequired is set to 'never' in the most recently received
// oadrDistributeEvent."
TEST_F(Openadr2bConformanceTest,
       Rule12_OptChangeIsNotSentForEventsThatNeverRequireAResponse) {
  fixtures::EventSpec quiet;
  quiet.responseRequired = "never";
  registerAndSync({quiet});
  try {
    adapter->respond(*db->event(1), Opt::Out, *sink);
  } catch (const AdapterError &) {
    // Refusing the operator's change is acceptable; telling the program is
    // not.
  }

  EXPECT_TRUE(http->requestsContaining(eiEvent, "oadrCreatedEvent").empty());
  EXPECT_TRUE(createOpts(*http).empty());
}

// ---------------------------------------------------------------------------
// Configured reports

TEST_F(Openadr2bAdapterTest, WithoutOffersOnlyStatusIsInTheMetadata) {
  registerAndSync();
  const auto reports = metadataReports(*http);
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_EQ(*reports[0].reportName(), "METADATA_TELEMETRY_STATUS");
}

TEST_F(Openadr2bAdapterTest, DisabledOffersAreNotInTheMetadata) {
  auto offer = usageOffer();
  offer.enabled = false;
  offerReports({offer});
  registerAndSync();
  EXPECT_EQ(findReport(metadataReports(*http), "METADATA_TELEMETRY_USAGE"),
            nullptr);
}

TEST_F(Openadr2bAdapterTest, AnOfferInTermsOpenAdrLacksIsAConfigurationError) {
  auto offer = usageOffer();
  offer.dataPoints[0].reportType = "consumption";
  EXPECT_EQ(kindOf([&] { offerReports({offer}); }),
            AdapterError::Kind::Permanent);
}

TEST_F(Openadr2bAdapterTest, OneShotReportCarriesTheLatestSample) {
  offerReports({usageOffer()});
  registerAndSync();
  addDatum(30, 250, 5000);

  fixtures::ReportRequestSpec spec{"rr-now", "usage-1", "power", "PT0S", ""};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  const auto updates = updatesFor(*http, "rr-now");
  ASSERT_EQ(updates.size(), 1u);
  const auto intervals = floatPayloads(updates[0]);
  ASSERT_EQ(intervals.size(), 1u);
  ASSERT_EQ(intervals[0].size(), 1u);
  EXPECT_FLOAT_EQ(intervals[0][0].value, 250);
  EXPECT_EQ(intervals[0][0].quality, "Quality Good - Non Specific");

  const auto requests = db->reportRequestsFor(connection);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].request.kind, ReportRequestKind::OneShot);
  EXPECT_EQ(requests[0].status, ReportRequestStatus::Completed);
}

TEST_F(Openadr2bAdapterTest, StaleLatestSampleIsMarked) {
  offerReports({usageOffer()});
  registerAndSync();
  addDatum(3600, 250, 5000);

  fixtures::ReportRequestSpec spec{"rr-now", "usage-1", "power", "PT0S", ""};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  const auto intervals = floatPayloads(updatesFor(*http, "rr-now").at(0));
  EXPECT_EQ(intervals.at(0).at(0).quality, "Quality Bad - Last Known Value");
}

TEST_F(Openadr2bAdapterTest, UnreadableDatumsAreReportedAsCommFailure) {
  offerReports({usageOffer()});
  registerAndSync();
  reader->failure = "SolarNetwork answered HTTP 503";

  fixtures::ReportRequestSpec spec{"rr-usage", "usage-1", "power", "PT1M",
                                   isoTime(clock.time)};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  const auto intervals = floatPayloads(updatesFor(*http, "rr-usage").at(0));
  ASSERT_EQ(intervals.size(), 1u);
  EXPECT_EQ(intervals[0].at(0).quality, "Quality Bad - Comm Failure");
  EXPECT_FLOAT_EQ(intervals[0].at(0).value, 0);
  EXPECT_TRUE(db->activityEntry(connection, "Could not read datums").has_value());
  const auto deliveries = db->reportDeliveriesFor(connection);
  ASSERT_EQ(deliveries.size(), 1u);
  EXPECT_TRUE(deliveries[0].ok);
  EXPECT_TRUE(deliveries[0].payload[0]["value"].is_null());
}

TEST_F(Openadr2bAdapterTest, EmptyWindowsAreRecordedAsNoValue) {
  offerReports({usageOffer()});
  registerAndSync();

  fixtures::ReportRequestSpec spec{"rr-usage", "usage-1", "power", "PT1M",
                                   isoTime(clock.time)};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  // The program is told the value is bad (rule 318); the record says why.
  const auto intervals = floatPayloads(updatesFor(*http, "rr-usage").at(0));
  EXPECT_EQ(intervals.at(0).at(0).quality, "Quality Bad - Non Specific");
  const auto deliveries = db->reportDeliveriesFor(connection);
  ASSERT_EQ(deliveries.size(), 1u);
  EXPECT_EQ(deliveries[0].payload[0]["quality"], "no_value");
}

TEST_F(Openadr2bAdapterTest, UnknownSpecifierIsRefused) {
  offerReports({usageOffer()});
  registerAndSync();

  fixtures::ReportRequestSpec spec{"rr-x", "nope", "power", "PT1M",
                                   isoTime(clock.time)};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  EXPECT_EQ(createdReportCodes(*http), std::vector<std::string>{"452"});
  EXPECT_TRUE(updatesFor(*http, "rr-x").empty());
  EXPECT_TRUE(db->reportRequestsFor(connection).empty());
}

TEST_F(Openadr2bAdapterTest, HistoryBeyondTheOfferIsRefused) {
  offerReports({usageOffer("HISTORY_USAGE", 3600)});
  registerAndSync();

  fixtures::ReportRequestSpec spec{"rr-hist", "usage-1", "power", "PT0S",
                                   isoTime(clock.time - 7200), "PT0S", "PT2H"};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  EXPECT_EQ(createdReportCodes(*http), std::vector<std::string>{"454"});
  EXPECT_TRUE(updatesFor(*http, "rr-hist").empty());
}

TEST_F(Openadr2bAdapterTest, HistoryIsServedAtTheOfferedSampling) {
  offerReports({usageOffer("HISTORY_USAGE", 3600)});
  registerAndSync();
  for (long ago = 600; ago > 0; ago -= 60) {
    addDatum(ago, static_cast<double>(ago), 0);
  }

  fixtures::ReportRequestSpec spec{"rr-hist", "usage-1", "power", "PT0S",
                                   isoTime(clock.time - 600), "PT0S", "PT10M"};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  EXPECT_EQ(createdReportCodes(*http), std::vector<std::string>{"200"});
  const auto updates = updatesFor(*http, "rr-hist");
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(*updates[0].reportName(), "HISTORY_USAGE");
  const auto intervals = floatPayloads(updates[0]);
  ASSERT_EQ(intervals.size(), 10u);
  EXPECT_FLOAT_EQ(intervals[0].at(0).value, 600);
  EXPECT_FLOAT_EQ(intervals[9].at(0).value, 60);

  const auto requests = db->reportRequestsFor(connection);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].request.kind, ReportRequestKind::History);
  EXPECT_EQ(requests[0].request.durationSeconds, 600);
  EXPECT_EQ(requests[0].status, ReportRequestStatus::Completed);
}

TEST_F(Openadr2bAdapterTest, CancelledRequestsAreRecordedAsCancelled) {
  offerReports({usageOffer()});
  registerAndSync();
  fixtures::ReportRequestSpec spec{"rr-usage", "usage-1", "power", "PT1M",
                                   isoTime(clock.time)};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);
  EXPECT_EQ(db->reportRequestsFor(connection).at(0).status,
            ReportRequestStatus::Active);

  http->queue(oadrPoll, fixtures::cancelReport("rr-usage"));
  adapter->poll(*sink);
  EXPECT_EQ(db->reportRequestsFor(connection).at(0).status,
            ReportRequestStatus::Cancelled);
  EXPECT_TRUE(db->activityEntry(connection, "cancelled report request rr-usage")
                  .has_value());
}

TEST_F(Openadr2bAdapterTest, UnregisteringEndsTheRequests) {
  offerReports({usageOffer()});
  registerAndSync();
  fixtures::ReportRequestSpec spec{"rr-usage", "usage-1", "power", "PT1M",
                                   isoTime(clock.time)};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  http->queue(registerParty, fixtures::canceledPartyRegistration());
  adapter->unregisterConnection(*sink);
  EXPECT_EQ(db->reportRequestsFor(connection).at(0).status,
            ReportRequestStatus::Cancelled);
}

TEST_F(Openadr2bAdapterTest, ARejectedUpdateIsRecordedAsFailed) {
  offerReports({usageOffer()});
  registerAndSync();
  fixtures::ReportRequestSpec spec{"rr-usage", "usage-1", "power", "PT1M",
                                   isoTime(clock.time)};
  http->queue(oadrPoll, fixtures::createReport({spec}));
  adapter->poll(*sink);

  http->queue(eiReport, fixtures::updatedReport("450", "Nope"));
  EXPECT_THROW(adapter->runDueWork(*sink), AdapterError);
  const auto deliveries = db->reportDeliveriesFor(connection);
  ASSERT_EQ(deliveries.size(), 1u);
  EXPECT_FALSE(deliveries[0].ok);
  EXPECT_EQ(deliveries[0].responseCode, "450");
  EXPECT_TRUE(db->activityEntry(connection, "was not delivered").has_value());
}
