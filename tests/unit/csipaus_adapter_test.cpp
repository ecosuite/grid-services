#include <gtest/gtest.h>

#include <algorithm>
#include <regex>
#include <set>

#include "core/event_sink.hpp"
#include "core/logging.hpp"
#include "protocols/csipaus/csipaus_adapter.hpp"
#include "support/csipaus_fixtures.hpp"
#include "support/fake_datum_reader.hpp"
#include "support/memory_store.hpp"
#include "support/scripted_rest.hpp"

using namespace grid_services;
using namespace grid_services::csipaus;
using namespace grid_services::testing;
namespace fixtures = grid_services::testing::csipaus_fixtures;

namespace {

const std::string ourLfdi = fixtures::deviceLfdi;
const std::string otherLfdi = "3E4F45AB31EDFE5B67E343E5E4562E31984E23E5";

class CsipausAdapterTest : public ::testing::Test {
protected:
  void SetUp() override {
    ConnectionConfig config;
    config.protocol = "csipaus";
    connection = db->addConnection(config);
    store = std::make_unique<MemoryRunnerStore>(db, connection);
    sink = std::make_unique<StoreEventSink>(*store, Opt::In, *log);

    settings.dcapUrl = "https://utility.example.com/dcap";
    identity = {ourLfdi, fixtures::deviceSfdi};

    rest->on("GET /dcap", fixtures::deviceCapability());
    rest->on("GET /edev/3",
             fixtures::endDevice("/edev/3", ourLfdi, fixtures::deviceSfdi));
    rest->on("GET /edev/3/fsa", fixtures::functionSetAssignmentsList(3600));
    rest->on("GET /derp", fixtures::derProgramList(60));
    rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500));
    rest->on("GET /derp/1/derc",
             fixtures::derControlList({fixtures::derControlElement(
                 "C1000000000000000000000000000007", 1, 1790000000, 1800,
                 2000)}));
    rest->on("POST /rsps/1/rsp", "", 201);
    rest->on("GET /tm", fixtures::time(1790000600));
    serverHasUs();
    build();
  }

  void build() {
    adapter = std::make_unique<CsipausAdapter>(
        db->connections.at(connection), settings, identity,
        std::make_unique<ScriptedRest>(rest), *store, [this] { return now; },
        [this] { return wallNow; });
  }

  void serverHasUs() {
    rest->on("GET /edev",
             fixtures::endDeviceList(
                 {fixtures::endDeviceElement("/edev/2", otherLfdi, 1),
                  fixtures::endDeviceElement("/edev/3", ourLfdi,
                                             fixtures::deviceSfdi)}));
  }

  void listControls(int currentStatus) {
    rest->on("GET /derp/1/derc",
             fixtures::derControlList({fixtures::derControlElement(
                 controlId, currentStatus, 1790000000, 1800, 2000)}));
  }

  std::optional<KnownEvent> control() {
    return store->findProgramEvent(controlId);
  }

  // The fixtures' control in "Network support" (primacy 2), and `outranking`
  // in "Site limits" (primacy 1).
  void listTwoPrograms(const std::vector<std::string> &outranking,
                       const std::string &responseRequired = "03",
                       const std::vector<std::pair<std::string, int>> &caps = {
                           {"opModExpLimW", 2000}}) {
    rest->on("GET /derp", fixtures::twoDerProgramList(60));
    rest->on("GET /derp/1/derc", fixtures::derControlList(outranking));
    rest->on("GET /derp/2/derc",
             fixtures::derControlList({fixtures::derControlElement(
                 controlId, 1, 1790000000, 1800, 1789990000, caps,
                 responseRequired)}));
  }

  // A control of "Site limits" holding export for the whole of the fixtures'
  // control.
  std::string outrankingControl(int currentStatus = 1) {
    return fixtures::derControlElement(outrankingId, currentStatus, 1789999000,
                                       3600, 1789980000,
                                       {{"opModExpLimW", 1000}});
  }

  // The status of each answer sent for `mrid`.
  std::vector<std::string> statusesSentFor(const std::string &mrid) {
    std::vector<std::string> statuses;
    const std::regex status("<status>(\\d+)</status>");
    for (const auto &body : responseBodies()) {
      std::smatch match;
      if (body.find("<subject>" + mrid + "</subject>") != std::string::npos &&
          std::regex_search(body, match, status)) {
        statuses.push_back(match[1].str());
      }
    }
    return statuses;
  }

  bool limitsName(const std::string &eventId) {
    const auto &limits = db->csipausLimits.at(connection);
    return std::any_of(limits.begin(), limits.end(), [&](const auto &segment) {
      return segment.programEventId == eventId;
    });
  }

  std::optional<KnownEvent> standingCaps() {
    return store->findProgramEvent(defaultId);
  }

  std::vector<std::string> responseBodies() {
    std::vector<std::string> bodies;
    for (const auto &request : rest->requestsTo("POST /rsps/1/rsp")) {
      bodies.push_back(request.body);
    }
    return bodies;
  }

  RegistrationInfo registerNow() {
    return adapter->registerConnection(*sink);
  }

  // Every request is answered 500, as CACTUS does while comms are disabled.
  void serverFails() {
    for (auto &[key, response] : rest->defaults) {
      response = {500, "COMMS DISABLED", {}, std::nullopt};
    }
  }

  // The worker restarted: a new adapter on the same store resumes.
  std::optional<RegistrationInfo> restart() {
    adapter.reset();
    store = std::make_unique<MemoryRunnerStore>(db, connection);
    sink = std::make_unique<StoreEventSink>(*store, Opt::In, *log);
    build();
    ConnectionState state;
    state.registrationId = "/edev/3";
    return adapter->resume(state, *sink);
  }

  AdapterError failure(const std::function<void()> &operation) {
    try {
      operation();
    } catch (const AdapterError &error) {
      return error;
    }
    ADD_FAILURE() << "expected an AdapterError";
    return AdapterError::stopped();
  }

  // Both clocks move on by `wait`.
  void advance(std::chrono::seconds wait) {
    now += wait;
    wallNow += wait;
  }

  // What a poll `wait` from now fetches.
  std::vector<std::string> pollAfter(Clock::duration wait) {
    now += wait;
    const auto before = rest->requests.size();
    adapter->poll(*sink);
    std::vector<std::string> fetched;
    for (auto i = before; i < rest->requests.size(); ++i) {
      fetched.push_back(rest->requests[i].key);
    }
    return fetched;
  }

  // The site's DER, whose connected and operating state are on one stream
  // of `reader`, at the DER the server's DERList gives.
  void reportState() {
    rest->on("GET /edev",
             fixtures::endDeviceList(
                 {fixtures::endDeviceElement("/edev/2", otherLfdi, 1),
                  fixtures::reportingEndDeviceElement(
                      "/edev/3", ourLfdi, fixtures::deviceSfdi, 300)}));
    rest->on("GET /edev/3", fixtures::reportingEndDevice(
                                "/edev/3", ourLfdi, fixtures::deviceSfdi, 300));
    rest->on("GET /edev/3/der", fixtures::derList(60));
    rest->on("PUT /edev/3/der/1/dercap", "", 204);
    rest->on("PUT /edev/3/der/1/derg", "", 204);
    rest->on("PUT /edev/3/der/1/ders", "", 204);

    ReportingSetup reporting;
    reporting.der = DerDescription{};
    reporting.der->type = 4;
    reporting.der->rtgMaxW = 5000;
    reporting.der->modesSupported = {"export", "energize", "connect"};
    reporting.der->modesEnabled = reporting.der->modesSupported;
    reporting.state.connected = statePoint("der-connected", "connected");
    reporting.state.operating = statePoint("der-operating", "operating");
    reporting.reader = reader;
    adapter = std::make_unique<CsipausAdapter>(
        db->connections.at(connection), settings, identity,
        std::make_unique<ScriptedRest>(rest), *store, [this] { return now; },
        [this] { return wallNow; }, std::move(reporting));
  }

  static TelemetryPoint statePoint(const std::string &pointId,
                                   const std::string &property) {
    TelemetryPoint telemetry;
    telemetry.pointId = pointId;
    telemetry.item.kind = "state";
    telemetry.item.units = "none";
    telemetry.source.nodeId = 604;
    telemetry.source.sourceId = "/S1/der-inv";
    telemetry.source.property = property;
    telemetry.source.reduce = datum::Reduce::Last;
    return telemetry;
  }

  // The inverter's latest datum, 5 s ago.
  void inverterSays(double isConnected, double isOperating) {
    reader->add(604, "/S1/der-inv",
                {wallNow - std::chrono::seconds(5),
                 {{"connected", isConnected}, {"operating", isOperating}}});
  }

  // Registers and polls; the DER is connected, then, 15 s on, is not.
  void disconnects() {
    registerNow();
    adapter->poll(*sink);
    inverterSays(1, 1);
    adapter->runDueWork(*sink);
    ASSERT_FALSE(adapter->pollWanted());
    advance(std::chrono::seconds(15));
    inverterSays(0, 0);
    adapter->runDueWork(*sink);
  }

  // The fixtures' control, switching the DER off by `name`.
  void listSwitchOff(const std::string &name) {
    rest->on("GET /derp/1/derc",
             fixtures::derControlList({fixtures::derControlElement(
                 controlId, 1, 1790000000, 1800, 1789990000, {{name, 0}})}));
  }

  const std::string controlId = "C1000000000000000000000000000007";
  const std::string outrankingId = "C1000000000000000000000000000008";
  const std::string defaultId = "default:B1000000000000000000000000000001";
  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  ConnectionId connection = 0;
  std::unique_ptr<MemoryRunnerStore> store;
  std::shared_ptr<spdlog::logger> log = makeLogger("test");
  std::unique_ptr<StoreEventSink> sink;
  CsipausSettings settings;
  DeviceIdentity identity;
  std::shared_ptr<RestScript> rest = std::make_shared<RestScript>();
  std::shared_ptr<FakeDatumReader> reader = std::make_shared<FakeDatumReader>();
  Clock::time_point now = Clock::time_point(std::chrono::hours(1000));
  // During the fixtures' control, which runs from 1790000000 for 30 minutes.
  Timestamp wallNow = Timestamp(std::chrono::seconds(1790000600));
  std::unique_ptr<CsipausAdapter> adapter;
};

} // namespace

TEST_F(CsipausAdapterTest, RegistersWithTheEndDeviceTheServerHasForIt) {
  const auto info = registerNow();

  EXPECT_EQ(info.clientId, ourLfdi);
  EXPECT_EQ(info.registrationId, "/edev/3");
  EXPECT_EQ(info.pollIntervalSeconds, 60) << "the DERProgramList's poll rate";
  EXPECT_EQ(rest->count("POST /edev"), 0u);
  EXPECT_EQ(rest->count("PUT /edev/3/cp"), 0u);
  EXPECT_EQ(rest->count("GET /edev/3/rg"), 0u);
  EXPECT_FALSE(db->activityEntry(connection, "no Registration"));
}

TEST_F(CsipausAdapterTest, CreatesItsEndDeviceWhenTheServerHasNone) {
  rest->on("GET /edev", fixtures::endDeviceList(
                            {fixtures::endDeviceElement("/edev/2", otherLfdi, 1)}));
  rest->queue("POST /edev", {201, "", {{"Location", "/edev/3"}}, std::nullopt});

  const auto info = registerNow();

  EXPECT_EQ(info.registrationId, "/edev/3");
  const auto posts = rest->requestsTo("POST /edev");
  ASSERT_EQ(posts.size(), 1u);
  EXPECT_NE(posts[0].body.find("<lFDI>" + ourLfdi + "</lFDI>"),
            std::string::npos);
  EXPECT_NE(posts[0].body.find("<sFDI>541127305912</sFDI>"),
            std::string::npos);
}

TEST_F(CsipausAdapterTest, ARefusedInBandRegistrationIsPermanent) {
  rest->on("GET /edev", fixtures::endDeviceList({}));
  rest->on("POST /edev", "", 403);

  EXPECT_EQ(failure([&] { registerNow(); }).kind(),
            AdapterError::Kind::Permanent);
}

TEST_F(CsipausAdapterTest, ChecksTheRegistrationPin) {
  settings.registrationPin = 111115;
  build();
  rest->on("GET /edev/3/rg", fixtures::registration(111115));

  registerNow();
  EXPECT_EQ(rest->count("GET /edev/3/rg"), 1u);

  rest->on("GET /edev/3/rg", fixtures::registration(222225));
  const auto error = failure([&] { registerNow(); });
  EXPECT_EQ(error.kind(), AdapterError::Kind::Permanent);
  EXPECT_NE(std::string(error.what()).find("does not match"),
            std::string::npos);
}

TEST_F(CsipausAdapterTest, APinThatDiffersStillStopsRegistration) {
  settings.registrationPin = 111115;
  settings.connectionPointId = "4102345678";
  build();
  rest->on("GET /edev/3/rg", fixtures::registration(222225));
  rest->on("PUT /edev/3/cp", "", 201);

  const auto error = failure([&] { registerNow(); });

  EXPECT_EQ(error.kind(), AdapterError::Kind::Permanent);
  EXPECT_NE(std::string(error.what()).find("does not match"),
            std::string::npos);
  EXPECT_EQ(rest->count("PUT /edev/3/cp"), 0u);
  EXPECT_EQ(rest->count("GET /edev/3/fsa"), 0u);
  EXPECT_FALSE(db->activityEntry(connection, "no Registration"));
}

// A server need not create a Registration for an EndDevice (CSIP-AUS v1.2
// explainer, section 4.1.5); ALL-05 expects registration to go on without one.
TEST_F(CsipausAdapterTest,
       RegistersWithoutCheckingThePinWhenTheServerGivesNoRegistration) {
  settings.registrationPin = 111115;
  settings.connectionPointId = "4102345678";
  build();
  rest->on("GET /edev", fixtures::endDeviceList(
                            {fixtures::endDeviceElement("/edev/2", otherLfdi, 1)}));
  rest->queue("POST /edev", {201, "", {{"Location", "/edev/3"}}, std::nullopt});
  rest->on("GET /edev/3", fixtures::endDevice("/edev/3", ourLfdi,
                                              fixtures::deviceSfdi, false));
  rest->on("PUT /edev/3/cp", "", 201);

  const auto info = registerNow();

  EXPECT_EQ(info.registrationId, "/edev/3");
  EXPECT_EQ(info.pollIntervalSeconds, 60);
  EXPECT_EQ(rest->count("GET /edev/3/rg"), 0u);
  EXPECT_EQ(rest->count("PUT /edev/3/cp"), 1u);
  EXPECT_EQ(rest->count("GET /edev/3/fsa"), 1u);
  const auto entry = db->activityEntry(
      connection, "no Registration to check the PIN against");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->category, ActivityCategory::Connection);
}

TEST_F(CsipausAdapterTest, SendsTheConnectionPoint) {
  settings.connectionPointId = "4102345678";
  build();
  rest->on("PUT /edev/3/cp", "", 201);

  registerNow();

  const auto puts = rest->requestsTo("PUT /edev/3/cp");
  ASSERT_EQ(puts.size(), 1u);
  EXPECT_NE(puts[0].body.find("<csipaus:connectionPointId>4102345678"
                              "</csipaus:connectionPointId>"),
            std::string::npos);
}

TEST_F(CsipausAdapterTest, ResumesWhileTheServerHasItsEndDevice) {
  ConnectionState state;
  state.registrationId = "/edev/3";

  const auto info = adapter->resume(state, *sink);

  ASSERT_TRUE(info);
  EXPECT_EQ(info->clientId, ourLfdi);
  EXPECT_EQ(info->registrationId, "/edev/3");
  EXPECT_EQ(info->pollIntervalSeconds, 60);
  EXPECT_EQ(rest->count("GET /edev"), 0u);
}

// CSIP-AUS v1.2 explainer, section 5.1.1: a client recreates what a server
// has lost, by finding or POSTing its EndDevice again.
TEST_F(CsipausAdapterTest, ResumingFindsOrRecreatesALostEndDevice) {
  ConnectionState state;
  state.registrationId = "/edev/9";
  auto info = adapter->resume(state, *sink);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->registrationId, "/edev/3") << "found again in the list";

  rest->on("GET /edev", fixtures::endDeviceList({}));
  rest->queue("POST /edev", {201, "", {{"Location", "/edev/3"}}, std::nullopt});
  state.registrationId = "/edev/2";
  rest->on("GET /edev/2", fixtures::endDevice("/edev/2", otherLfdi, 1));
  info = adapter->resume(state, *sink);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->registrationId, "/edev/3");
  EXPECT_EQ(rest->count("POST /edev"), 1u) << "created again";

  EXPECT_FALSE(adapter->resume(ConnectionState{}, *sink));
}

TEST_F(CsipausAdapterTest, TestSaysWhetherTheServerHasItsEndDevice) {
  auto result = adapter->test(*sink);
  EXPECT_TRUE(result.registered);
  EXPECT_EQ(result.clientId, ourLfdi);
  EXPECT_NE(result.description.find("/edev/3"), std::string::npos);

  rest->on("GET /edev", fixtures::endDeviceList({}));
  result = adapter->test(*sink);
  EXPECT_FALSE(result.registered);
  EXPECT_EQ(rest->count("POST /edev"), 0u);
}

TEST_F(CsipausAdapterTest, PollingDescribesTheProgramsAndTheirControls) {
  registerNow();

  const auto result = adapter->poll(*sink);

  EXPECT_EQ(result.outcome, PollOutcome::Ok);
  ASSERT_TRUE(result.summary);
  EXPECT_EQ(*result.summary,
            "DERProgram Site limits (primacy 2): default export 1.5 kW; "
            "active 2026-09-21 14:13 UTC for 30m: export 2 kW");
  EXPECT_EQ(adapter->requestedPollInterval(), std::chrono::seconds(60));
}

TEST_F(CsipausAdapterTest, PollingDescribesOnlyWhatChanged) {
  registerNow();
  adapter->poll(*sink);

  now += std::chrono::seconds(61);
  EXPECT_FALSE(adapter->poll(*sink).summary);

  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(5000));
  now += std::chrono::seconds(61);
  const auto result = adapter->poll(*sink);
  ASSERT_TRUE(result.summary);
  EXPECT_NE(result.summary->find("default export 5 kW"), std::string::npos);
}

TEST_F(CsipausAdapterTest, PollingFetchesEachResourceAtItsPollRate) {
  registerNow();
  adapter->poll(*sink);
  const auto before = rest->requests.size();

  // Past the DERProgramList's 60s, before the EndDeviceList's 300s and the
  // FunctionSetAssignmentsList's 3600s.
  now += std::chrono::seconds(61);
  adapter->poll(*sink);

  std::vector<std::string> fetched;
  for (auto i = before; i < rest->requests.size(); ++i) {
    fetched.push_back(rest->requests[i].key);
  }
  EXPECT_EQ(fetched, (std::vector<std::string>{
                         "GET /derp", "GET /derp/1/dderc", "GET /derp/1/derc"}));

  now += std::chrono::seconds(300);
  adapter->poll(*sink);
  EXPECT_EQ(rest->count("GET /edev"), 2u) << "registering's, and its 300s";
  EXPECT_EQ(rest->count("GET /edev/3"), 1u);
  EXPECT_EQ(rest->count("GET /edev/3/fsa"), 1u);
  EXPECT_EQ(rest->count("GET /dcap"), 1u) << "86400s";
}

// The CSIP-AUS v1.2 explainer's section 4.2.1: each resource at its own
// rate, DeviceCapability and Time included.
TEST_F(CsipausAdapterTest, PollsDeviceCapabilityEndDeviceListAndTimeAtTheirOwnRates) {
  using std::chrono::seconds;
  using Keys = std::vector<std::string>;
  rest->on("GET /dcap", fixtures::deviceCapability(120));
  rest->on("GET /edev",
           fixtures::endDeviceList({fixtures::endDeviceElement(
                                       "/edev/3", ourLfdi, fixtures::deviceSfdi)},
                                   -1, 60));
  rest->on("GET /derp", fixtures::derProgramList(300));
  rest->on("GET /tm", fixtures::time(1790000600, 900));
  registerNow();
  EXPECT_EQ(pollAfter(seconds(0)),
            (Keys{"GET /tm", "GET /derp/1/dderc", "GET /derp/1/derc",
                  "POST /rsps/1/rsp"}));

  EXPECT_EQ(pollAfter(seconds(61)), (Keys{"GET /edev", "GET /edev/3"}))
      << "the EndDevice at its list's rate";
  EXPECT_EQ(pollAfter(seconds(61)),
            (Keys{"GET /dcap", "GET /edev", "GET /edev/3"}));
  EXPECT_EQ(pollAfter(seconds(61)), (Keys{"GET /edev", "GET /edev/3"}));
  EXPECT_EQ(pollAfter(seconds(61)),
            (Keys{"GET /dcap", "GET /edev", "GET /edev/3"}));
  EXPECT_EQ(pollAfter(seconds(61)),
            (Keys{"GET /edev", "GET /edev/3", "GET /derp", "GET /derp/1/dderc",
                  "GET /derp/1/derc"}));
  EXPECT_EQ(rest->count("GET /tm"), 1u);
  pollAfter(seconds(600));
  EXPECT_EQ(rest->count("GET /tm"), 2u) << "at 900s";
  EXPECT_EQ(rest->count("GET /edev/3/fsa"), 1u) << "at 3600s";
}

TEST_F(CsipausAdapterTest, AsksToBePolledWhenTheNextResourceIsDue) {
  registerNow();
  EXPECT_FALSE(adapter->requestedPollInterval()) << "nothing walked yet";

  adapter->poll(*sink);
  EXPECT_EQ(adapter->requestedPollInterval(), std::chrono::seconds(60))
      << "the DERProgramList, due first";
  pollAfter(std::chrono::seconds(50));
  EXPECT_EQ(adapter->requestedPollInterval(), std::chrono::seconds(10));
  pollAfter(std::chrono::milliseconds(9'800));
  EXPECT_EQ(adapter->requestedPollInterval(), std::chrono::seconds(1))
      << "at least a second";
}

TEST_F(CsipausAdapterTest, TheEndDeviceFollowsItsListsPollRate) {
  using std::chrono::seconds;
  using Keys = std::vector<std::string>;
  rest->on("GET /edev",
           fixtures::endDeviceList({fixtures::endDeviceElement(
                                       "/edev/3", ourLfdi, fixtures::deviceSfdi)},
                                   -1, 120));
  registerNow();
  adapter->poll(*sink);
  pollAfter(seconds(61));
  EXPECT_EQ(rest->count("GET /edev/3"), 0u);
  pollAfter(seconds(60));
  EXPECT_EQ(rest->count("GET /edev/3"), 1u);

  // After a restart, the list is read before the EndDevice is polled again.
  build();
  ConnectionState state;
  state.registrationId = "/edev/3";
  ASSERT_TRUE(adapter->resume(state, *sink));
  EXPECT_EQ(rest->count("GET /edev/3"), 2u);
  const auto first = pollAfter(seconds(1));
  ASSERT_GE(first.size(), 3u);
  EXPECT_EQ(Keys(first.begin(), first.begin() + 3),
            (Keys{"GET /dcap", "GET /edev", "GET /edev/3"}));
  EXPECT_EQ(pollAfter(seconds(61)), (Keys{"GET /derp", "GET /derp/1/dderc",
                                          "GET /derp/1/derc"}));
  EXPECT_EQ(rest->count("GET /edev/3"), 3u);
  pollAfter(seconds(60));
  EXPECT_EQ(rest->count("GET /edev/3"), 4u);
}

TEST_F(CsipausAdapterTest, APollGivesTheShortestDERProgramListRateItWalked) {
  rest->on("GET /edev/3/fsa", fixtures::emptyFunctionSetAssignmentsList());
  EXPECT_FALSE(registerNow().pollIntervalSeconds) << "no program yet";
  EXPECT_FALSE(adapter->poll(*sink).pollIntervalSeconds);

  rest->on("GET /edev/3/fsa", fixtures::functionSetAssignmentsList(3600));
  now += std::chrono::seconds(61);
  EXPECT_EQ(adapter->poll(*sink).pollIntervalSeconds, 60);

  rest->on("GET /derp", fixtures::derProgramList(30));
  now += std::chrono::seconds(61);
  EXPECT_EQ(adapter->poll(*sink).pollIntervalSeconds, 30);
}

TEST_F(CsipausAdapterTest, ReadsLinksFromTheLatestDeviceCapability) {
  rest->on("GET /dcap", fixtures::deviceCapability(60));
  registerNow();
  adapter->poll(*sink);
  EXPECT_EQ(rest->count("GET /tm"), 1u);

  rest->on("GET /dcap", fixtures::deviceCapability(60, "/time"));
  rest->on("GET /time", fixtures::time(1790000660));
  pollAfter(std::chrono::seconds(61));
  EXPECT_EQ(rest->count("GET /time"), 1u);
  EXPECT_EQ(rest->count("GET /tm"), 1u);
}

TEST_F(CsipausAdapterTest, AFailedPollWalksEverythingAgain) {
  registerNow();
  adapter->poll(*sink);
  rest->queue("GET /derp/1/derc", {503, "", {}, std::nullopt});

  now += std::chrono::seconds(61);
  EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
            AdapterError::Kind::Transient);
  adapter->poll(*sink);

  EXPECT_EQ(rest->count("GET /edev/3/fsa"), 2u);
}

// CSIP-AUS v1.2 explainer, section 5.1.2.1: a server may remove EndDevices
// as a matter of course, and the client creates its own again.
TEST_F(CsipausAdapterTest, AnEndDeviceTheServerDroppedIsRegisteredAgain) {
  registerNow();
  adapter->poll(*sink);
  rest->on("GET /edev/3", "", 404);

  now += std::chrono::seconds(301);
  const auto result = adapter->poll(*sink);

  EXPECT_EQ(result.outcome, PollOutcome::ReregistrationRequested);
  ASSERT_TRUE(result.summary);
  EXPECT_NE(result.summary->find("no longer has EndDevice /edev/3"),
            std::string::npos);
}

TEST_F(CsipausAdapterTest, UnregisteringOnlyStopsPolling) {
  registerNow();
  const auto before = rest->requests.size();

  adapter->unregisterConnection(*sink);

  EXPECT_EQ(rest->requests.size(), before);
  EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
            AdapterError::Kind::Permanent);
}

TEST_F(CsipausAdapterTest, EachOperationRecordsItsOwnExchange) {
  registerNow();
  const auto registration = adapter->takeExchange();
  ASSERT_TRUE(registration.is_array());
  EXPECT_EQ(registration.front()["url"], "https://utility.example.com/dcap");

  adapter->poll(*sink);
  adapter->poll(*sink);
  const auto lastPoll = adapter->takeExchange();
  // Nothing was due on the second poll.
  EXPECT_TRUE(lastPoll.is_null());
}

TEST_F(CsipausAdapterTest, PausingTellsTheServerNothing) {
  EXPECT_EQ(adapter->describeParticipation(Participation::Paused, Opt::In),
            "Participation paused. CSIP-AUS controls cannot be refused, so "
            "the server is not told and its controls still apply.");
}

TEST_F(CsipausAdapterTest, DescribesItsClientByLfdi) {
  EXPECT_EQ(adapter->describeClient(ourLfdi),
            "LFDI C995-EBDD-FCFA-53DE-23DA-8EA2-BFFF-9183-4D51-D617");
}

TEST_F(CsipausAdapterTest, StoresEachControlAsAnEventTheSiteCannotDecline) {
  registerNow();
  adapter->poll(*sink);

  const auto stored = control();
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->status, ProgramStatus::Active);
  EXPECT_EQ(stored->startAt, Timestamp(std::chrono::seconds(1790000000)));
  EXPECT_EQ(stored->durationSeconds, 1800);
  EXPECT_FALSE(stored->declinable);
  EXPECT_TRUE(stored->responseRequired);
  EXPECT_EQ(stored->opt, Opt::In);
  EXPECT_EQ(stored->signals,
            (std::vector<Signal>{{"export", "limit", "export", {{0, 1800, 2000}}}}));

  const auto &detail = db->csipausEvents.at(stored->id);
  EXPECT_EQ(detail.kind, CsipausEventKind::Control);
  EXPECT_EQ(detail.href, "/derp/1/derc/7");
  EXPECT_EQ(detail.programMrid, "B1000000000000000000000000000001");
  EXPECT_EQ(detail.programDescription, "Site limits");
  EXPECT_EQ(detail.primacy, 2);
  EXPECT_EQ(detail.creationTime, Timestamp(std::chrono::seconds(1789990000)));
  EXPECT_EQ(detail.replyTo, "/rsps/1/rsp");
  EXPECT_EQ(detail.responseRequired, 3);
}

TEST_F(CsipausAdapterTest, StoresEachProgramsDefaultAsAnOpenEndedEvent) {
  registerNow();
  adapter->poll(*sink);

  const auto stored = standingCaps();
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->status, ProgramStatus::Active);
  EXPECT_EQ(stored->startAt, wallNow);
  EXPECT_EQ(stored->durationSeconds, 0);
  EXPECT_FALSE(stored->declinable);
  EXPECT_FALSE(stored->responseRequired);
  EXPECT_EQ(stored->signals,
            (std::vector<Signal>{{"export", "limit", "export", {{0, 0, 1500}}}}));
  EXPECT_EQ(db->csipausEvents.at(stored->id).kind, CsipausEventKind::Default);
  EXPECT_EQ(db->csipausEvents.at(stored->id).mrid,
            "D1000000000000000000000000000001");
}

TEST_F(CsipausAdapterTest, AChangedDefaultModifiesItsEvent) {
  registerNow();
  adapter->poll(*sink);
  const auto first = standingCaps();

  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(5000));
  now += std::chrono::seconds(61);
  wallNow += std::chrono::seconds(61);
  adapter->poll(*sink);

  const auto changed = standingCaps();
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->id, first->id);
  EXPECT_EQ(changed->modificationNumber, 1);
  EXPECT_EQ(changed->startAt, first->startAt);
  EXPECT_EQ(changed->signals.at(0).intervals.at(0).value, 5000);
}

TEST_F(CsipausAdapterTest, TellsTheServerAControlWasReceivedOnce) {
  registerNow();
  adapter->poll(*sink);
  now += std::chrono::seconds(61);
  adapter->poll(*sink);

  const auto bodies = responseBodies();
  ASSERT_EQ(bodies.size(), 1u);
  EXPECT_NE(bodies[0].find("<DERControlResponse"), std::string::npos);
  EXPECT_NE(bodies[0].find("<endDeviceLFDI>" + ourLfdi + "</endDeviceLFDI>"),
            std::string::npos);
  EXPECT_NE(bodies[0].find("<status>1</status>"), std::string::npos);
  EXPECT_NE(bodies[0].find("<subject>" + controlId + "</subject>"),
            std::string::npos);
  const auto responses = db->responsesFor(control()->id);
  ASSERT_EQ(responses.size(), 1u);
  EXPECT_EQ(responses[0].answer, EventAnswer::Received);
  EXPECT_TRUE(responses[0].ok);
}

TEST_F(CsipausAdapterTest, AnswersAControlTheServerWasNotToldOfOnTheNextPoll) {
  registerNow();
  rest->queue("POST /rsps/1/rsp", {503, "", {}, std::nullopt});

  EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
            AdapterError::Kind::Transient);
  adapter->poll(*sink);

  const auto responses = db->responsesFor(control()->id);
  ASSERT_EQ(responses.size(), 2u);
  EXPECT_FALSE(responses[0].ok);
  EXPECT_TRUE(responses[1].ok);
}

TEST_F(CsipausAdapterTest, TellsTheServerAControlWasCancelled) {
  registerNow();
  adapter->poll(*sink);

  listControls(2);
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Cancelled);
  const auto bodies = responseBodies();
  ASSERT_EQ(bodies.size(), 2u);
  EXPECT_NE(bodies[1].find("<status>6</status>"), std::string::npos);
  EXPECT_EQ(db->responsesFor(control()->id).back().answer,
            EventAnswer::Cancelled);
}

TEST_F(CsipausAdapterTest, TellsTheServerOfACancellationInALaterSecond) {
  registerNow();
  adapter->poll(*sink);

  listControls(2);
  now += std::chrono::seconds(61);
  adapter->poll(*sink);
  EXPECT_EQ(control()->status, ProgramStatus::Active)
      << "received was sent this second, so it waits for the next poll";
  EXPECT_EQ(responseBodies().size(), 1u);

  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  EXPECT_EQ(control()->status, ProgramStatus::Cancelled);
  ASSERT_EQ(responseBodies().size(), 2u);
  EXPECT_NE(responseBodies()[1].find("<status>6</status>"), std::string::npos);
}

TEST_F(CsipausAdapterTest, TellsTheServerAControlWasSuperseded) {
  registerNow();
  adapter->poll(*sink);

  listControls(4);
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Superseded);
  EXPECT_NE(responseBodies().back().find("<status>7</status>"),
            std::string::npos);
}

TEST_F(CsipausAdapterTest, AControlPastItsEndHasCompleted) {
  registerNow();
  adapter->poll(*sink);

  now += std::chrono::seconds(61);
  wallNow = Timestamp(std::chrono::seconds(1790001800));
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Completed);
  EXPECT_EQ(standingCaps()->status, ProgramStatus::Active);
}

TEST_F(CsipausAdapterTest, AControlTheServerWithdrewHasBeenCancelled) {
  registerNow();
  adapter->poll(*sink);

  rest->on("GET /derp/1/derc", fixtures::derControlList({}));
  now += std::chrono::seconds(61);
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Cancelled);
}

TEST_F(CsipausAdapterTest, IgnoresControlsThatEndedBeforeTheyWereSeen) {
  wallNow = Timestamp(std::chrono::seconds(1790001800));
  rest->on("GET /tm", fixtures::time(1790001800));
  registerNow();
  adapter->poll(*sink);

  EXPECT_FALSE(control());
  EXPECT_TRUE(standingCaps());
  EXPECT_TRUE(responseBodies().empty());
}

TEST_F(CsipausAdapterTest, RecordsHowFarTheServersClockIs) {
  rest->on("GET /tm", fixtures::time(1790000630));
  registerNow();
  adapter->poll(*sink);

  EXPECT_EQ(db->stateExtras(connection).clockOffsetSeconds, 30);
  EXPECT_TRUE(db->activityEntry(
      connection, "The server's clock is 30 s ahead of the worker's"));
}

TEST_F(CsipausAdapterTest, JudgesControlsByTheServersClock) {
  rest->on("GET /tm", fixtures::time(1790000600, 60));
  registerNow();
  adapter->poll(*sink);
  ASSERT_EQ(control()->status, ProgramStatus::Active);

  // Ten seconds before the control's end by our clock, thirty after by the
  // server's.
  now += std::chrono::seconds(61);
  wallNow = Timestamp(std::chrono::seconds(1790001790));
  rest->on("GET /tm", fixtures::time(1790001830, 60));
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Completed);
}

TEST_F(CsipausAdapterTest, StoresTheCapsInForce) {
  registerNow();
  adapter->poll(*sink);

  const auto limits = db->csipausLimits.at(connection);
  ASSERT_EQ(limits.size(), 2u);
  // The control has no rampTms, so both ramp at the default's setGradW.
  EXPECT_EQ(limits[0], (LimitSegment{"export",
                                     Timestamp(std::chrono::seconds(1790000000)),
                                     Timestamp(std::chrono::seconds(1790001800)),
                                     2000, CsipausEventKind::Control, controlId,
                                     std::nullopt, 27}));
  EXPECT_EQ(limits[1],
            (LimitSegment{"export", Timestamp(std::chrono::seconds(1790001800)),
                          std::nullopt, 1500, CsipausEventKind::Default,
                          defaultId, std::nullopt, 27}));
}

TEST_F(CsipausAdapterTest, StoresTheRampIntoEachRow) {
  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, 100));
  rest->on("GET /derp/1/derc",
           fixtures::derControlList({fixtures::derControlElement(
               controlId, 1, 1790000000, 1800, 1789990000,
               {{"opModExpLimW", 2000}}, "03", 6000)}));
  registerNow();
  adapter->poll(*sink);

  const auto limits = db->csipausLimits.at(connection);
  ASSERT_EQ(limits.size(), 2u);
  EXPECT_EQ(limits[0].programEventId, controlId);
  EXPECT_EQ(limits[0].rampTms, 6000) << "a minute, in hundredths of a second";
  EXPECT_FALSE(limits[0].setGradW.has_value());
  EXPECT_EQ(limits[1].programEventId, defaultId);
  EXPECT_FALSE(limits[1].rampTms.has_value());
  EXPECT_EQ(limits[1].setGradW, 100);
}

TEST_F(CsipausAdapterTest, RowsCarryNoRampWhenNothingSetsOne) {
  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, std::nullopt));
  registerNow();
  adapter->poll(*sink);

  const auto limits = db->csipausLimits.at(connection);
  ASSERT_EQ(limits.size(), 2u);
  for (const auto &row : limits) {
    EXPECT_FALSE(row.rampTms.has_value());
    EXPECT_FALSE(row.setGradW.has_value());
  }
}

TEST_F(CsipausAdapterTest, ReplacesTheLimitsWhenOnlyTheDefaultsSetGradWChanges) {
  registerNow();
  adapter->poll(*sink);
  ASSERT_EQ(db->csipausLimits.at(connection).size(), 2u);
  EXPECT_EQ(db->csipausLimits.at(connection)[1].setGradW, 27);

  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, 100));
  pollAfter(std::chrono::seconds(60));

  const auto limits = db->csipausLimits.at(connection);
  ASSERT_EQ(limits.size(), 2u);
  EXPECT_EQ(limits[0].setGradW, 100) << "the control has no rampTms";
  EXPECT_EQ(limits[1].setGradW, 100);
  EXPECT_EQ(limits[1].value, 1500);
}

TEST_F(CsipausAdapterTest, SaysAControlStartedAndCompletedAsThePlannerSays) {
  registerNow();
  adapter->poll(*sink);
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  ASSERT_EQ(responseBodies().size(), 1u) << "only received, until the site acts";

  db->setLifecycle(control()->id, "control");
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  ASSERT_EQ(responseBodies().size(), 2u);
  EXPECT_NE(responseBodies()[1].find("<status>2</status>"), std::string::npos);

  db->setLifecycle(control()->id, "completed");
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  const auto bodies = responseBodies();
  ASSERT_EQ(bodies.size(), 3u);
  EXPECT_NE(bodies[2].find("<status>3</status>"), std::string::npos);
  EXPECT_EQ(db->responsesFor(control()->id).back().answer,
            EventAnswer::Completed);
}

TEST_F(CsipausAdapterTest, SaysAControlStartedBetweenPolls) {
  registerNow();
  adapter->poll(*sink);
  ASSERT_EQ(responseBodies().size(), 1u);

  db->setLifecycle(control()->id, "control");
  advance(std::chrono::seconds(1));
  const auto before = rest->requests.size();
  adapter->runDueWork(*sink);

  ASSERT_EQ(rest->requests.size(), before + 1) << "no GETs, only the answer";
  EXPECT_EQ(rest->requests.back().key, "POST /rsps/1/rsp");
  EXPECT_NE(responseBodies().back().find("<status>2</status>"),
            std::string::npos);
  EXPECT_EQ(db->responsesFor(control()->id).back().answer,
            EventAnswer::Started);
}

TEST_F(CsipausAdapterTest, SaysAControlStartedOnlyOnceTheServerKnowsItWasReceived) {
  registerNow();
  rest->queue("POST /rsps/1/rsp", {503, "", {}, std::nullopt});
  EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
            AdapterError::Kind::Transient);
  db->setLifecycle(control()->id, "control");

  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  EXPECT_EQ(rest->count("POST /rsps/1/rsp"), 1u) << "received first";

  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);

  const auto responses = db->responsesFor(control()->id);
  ASSERT_EQ(responses.size(), 3u);
  EXPECT_EQ(responses[1].answer, EventAnswer::Received);
  EXPECT_TRUE(responses[1].ok);
  EXPECT_EQ(responses[2].answer, EventAnswer::Started);
  EXPECT_TRUE(responses[2].ok);
}

TEST_F(CsipausAdapterTest, SendsOneStatusPerControlEachRun) {
  registerNow();
  adapter->poll(*sink);
  db->setLifecycle(control()->id, "completed");

  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  ASSERT_EQ(responseBodies().size(), 2u);
  EXPECT_NE(responseBodies()[1].find("<status>2</status>"), std::string::npos);

  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  ASSERT_EQ(responseBodies().size(), 3u);
  EXPECT_NE(responseBodies()[2].find("<status>3</status>"), std::string::npos);

  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  EXPECT_EQ(responseBodies().size(), 3u);
}

// The runner orders a control's responses by their whole-second
// createdDateTime.
TEST_F(CsipausAdapterTest, NeverSendsTwoStatusesForAControlInOneSecond) {
  registerNow();
  adapter->poll(*sink);
  db->setLifecycle(control()->id, "control");

  adapter->runDueWork(*sink);
  EXPECT_EQ(responseBodies().size(), 1u) << "received was sent this second";

  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  ASSERT_EQ(responseBodies().size(), 2u);
  EXPECT_NE(responseBodies()[1].find("<status>2</status>"), std::string::npos);
  EXPECT_NE(responseBodies()[1].find("<createdDateTime>1790000601"
                                     "</createdDateTime>"),
            std::string::npos);
}

// CSIP-AUS v1.2 explainer, section 8.2: retries at least 10 s apart.
TEST_F(CsipausAdapterTest, RetriesAProgressAnswerTheServerRefusedTenSecondsLater) {
  registerNow();
  adapter->poll(*sink);
  db->setLifecycle(control()->id, "control");
  rest->queue("POST /rsps/1/rsp", {500, "", {}, std::nullopt});

  advance(std::chrono::seconds(1));
  const auto error = failure([&] { adapter->runDueWork(*sink); });
  EXPECT_EQ(error.kind(), AdapterError::Kind::Transient);
  EXPECT_NE(std::string(error.what())
                .find("the server was not told about its controls"),
            std::string::npos)
      << error.what();
  auto responses = db->responsesFor(control()->id);
  ASSERT_EQ(responses.size(), 2u);
  EXPECT_EQ(responses[1].answer, EventAnswer::Started);
  EXPECT_FALSE(responses[1].ok);

  advance(std::chrono::seconds(5));
  adapter->runDueWork(*sink);
  EXPECT_EQ(rest->count("POST /rsps/1/rsp"), 2u) << "not yet";

  advance(std::chrono::seconds(5));
  adapter->runDueWork(*sink);
  EXPECT_EQ(rest->count("POST /rsps/1/rsp"), 3u);
  responses = db->responsesFor(control()->id);
  EXPECT_EQ(responses.back().answer, EventAnswer::Started);
  EXPECT_TRUE(responses.back().ok);
}

TEST_F(CsipausAdapterTest, StoresEachControlsEffectiveWindow) {
  rest->on("GET /derp/1/derc",
           fixtures::derControlList({fixtures::derControlElement(
               controlId, 1, 1790000000, 1800, 2000, 300, 120)}));
  registerNow();
  adapter->poll(*sink);

  const auto start = Timestamp(std::chrono::seconds(
      1790000000 + randomOffset(ourLfdi, controlId, 300)));
  const auto end = start + std::chrono::seconds(
                               1800 + randomOffset(ourLfdi, controlId, 120));
  const auto &detail = db->csipausEvents.at(control()->id);
  EXPECT_EQ(detail.randomizeStartSeconds, 300);
  EXPECT_EQ(detail.randomizeDurationSeconds, 120);
  EXPECT_EQ(detail.effectiveStartAt, start);
  EXPECT_EQ(detail.effectiveEndAt, end);

  const auto limits = db->csipausLimits.at(connection);
  ASSERT_FALSE(limits.empty());
  EXPECT_EQ(limits[0].source, CsipausEventKind::Control);
  EXPECT_EQ(limits[0].start, start);
  EXPECT_EQ(limits[0].end, end);

  const auto &standing = db->csipausEvents.at(standingCaps()->id);
  EXPECT_FALSE(standing.effectiveStartAt);
  EXPECT_FALSE(standing.effectiveEndAt);
}

// As after an upgrade, for controls stored before the worker wrote windows.
TEST_F(CsipausAdapterTest, WritesTheWindowOfAControlStoredWithoutOne) {
  registerNow();
  adapter->poll(*sink);
  const auto id = control()->id;
  const auto stored = db->csipausEvents.at(id);
  ASSERT_TRUE(stored.effectiveStartAt);
  db->csipausEvents.at(id).effectiveStartAt.reset();
  db->csipausEvents.at(id).effectiveEndAt.reset();

  build();
  ConnectionState state;
  state.registrationId = "/edev/3";
  ASSERT_TRUE(adapter->resume(state, *sink));
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  EXPECT_EQ(db->csipausEvents.at(id), stored);

  db->csipausEvents.at(id).effectiveStartAt.reset();
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  EXPECT_FALSE(db->csipausEvents.at(id).effectiveStartAt)
      << "unchanged since this worker wrote it, so not written again";
}

// Envoy marks supersession only within one DERProgram, so across programs
// the worker's own resolution says which controls were superseded.
TEST_F(CsipausAdapterTest, TellsTheServerAControlItsOwnResolutionSupersedes) {
  listTwoPrograms({});
  registerNow();
  adapter->poll(*sink);
  ASSERT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1"}));

  listTwoPrograms({outrankingControl()});
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Superseded);
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1", "7"}));
  EXPECT_EQ(statusesSentFor(outrankingId), (std::vector<std::string>{"1"}));
  EXPECT_EQ(db->responsesFor(control()->id).back().answer,
            EventAnswer::Superseded);
  const auto entry = db->activityEntry(connection, "This device superseded");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->category, ActivityCategory::Events);
  EXPECT_EQ(entry->eventId, control()->id);
  EXPECT_FALSE(limitsName(controlId));
  EXPECT_TRUE(limitsName(outrankingId));
}

TEST_F(CsipausAdapterTest, AControlItSupersededStaysOutOfTheCaps) {
  listTwoPrograms({});
  registerNow();
  adapter->poll(*sink);
  listTwoPrograms({outrankingControl()});
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  ASSERT_EQ(control()->status, ProgramStatus::Superseded);

  listTwoPrograms({outrankingControl(2)});
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  ASSERT_EQ(statusesSentFor(outrankingId),
            (std::vector<std::string>{"1", "6"}));
  EXPECT_EQ(control()->status, ProgramStatus::Superseded);
  EXPECT_FALSE(limitsName(controlId)) << "the server still lists it as active";
  EXPECT_FALSE(limitsName(outrankingId));

  build();
  ConnectionState state;
  state.registrationId = "/edev/3";
  ASSERT_TRUE(adapter->resume(state, *sink));
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  EXPECT_EQ(control()->status, ProgramStatus::Superseded);
  EXPECT_FALSE(limitsName(controlId)) << "nor after a restart";
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1", "7"}));
}

TEST_F(CsipausAdapterTest, ANewControlItSupersedesIsReceivedFirst) {
  listTwoPrograms({outrankingControl()});
  registerNow();
  adapter->poll(*sink);
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1"}))
      << "received was sent this second, so 7 waits for the next poll";
  EXPECT_EQ(control()->status, ProgramStatus::Active);

  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1"}))
      << "superseding is judged on polls";

  advance(std::chrono::seconds(60));
  adapter->poll(*sink);
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1", "7"}));
  EXPECT_EQ(control()->status, ProgramStatus::Superseded);
  const auto bodies = responseBodies();
  std::set<std::string> created;
  const std::regex at("<createdDateTime>(\\d+)</createdDateTime>");
  for (const auto &body : bodies) {
    std::smatch match;
    if (body.find("<subject>" + controlId + "</subject>") != std::string::npos &&
        std::regex_search(body, match, at)) {
      EXPECT_TRUE(created.insert(match[1].str()).second)
          << "two statuses in one second";
    }
  }
}

TEST_F(CsipausAdapterTest, SupersedesOnlyOnceTheServerTookItsReceived) {
  listTwoPrograms({});
  registerNow();
  rest->queue("POST /rsps/1/rsp", {503, "", {}, std::nullopt});
  EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
            AdapterError::Kind::Transient);

  listTwoPrograms({outrankingControl()});
  rest->queue("POST /rsps/1/rsp", {201, "", {}, std::nullopt});
  rest->queue("POST /rsps/1/rsp", {503, "", {}, std::nullopt});
  advance(std::chrono::seconds(61));
  EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
            AdapterError::Kind::Transient);
  ASSERT_EQ(statusesSentFor(outrankingId), (std::vector<std::string>{"1"}));
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1", "1"}))
      << "no 7 while the server has not taken its received";
  EXPECT_EQ(control()->status, ProgramStatus::Active);

  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  EXPECT_EQ(statusesSentFor(controlId),
            (std::vector<std::string>{"1", "1", "1"}));

  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  EXPECT_EQ(statusesSentFor(controlId),
            (std::vector<std::string>{"1", "1", "1", "7"}));
  EXPECT_EQ(control()->status, ProgramStatus::Superseded);
}

TEST_F(CsipausAdapterTest, SupersedesWithoutAResponseWhenNoneIsAsked) {
  listTwoPrograms({}, "01");
  registerNow();
  adapter->poll(*sink);
  listTwoPrograms({outrankingControl()}, "01");
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Superseded);
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1"}));
  EXPECT_TRUE(db->activityEntry(connection, "This device superseded"));
}

TEST_F(CsipausAdapterTest, AControlThatStillWinsACapIsNotSuperseded) {
  const std::vector<std::pair<std::string, int>> caps = {
      {"opModExpLimW", 2000}, {"opModGenLimW", 4000}};
  listTwoPrograms({}, "03", caps);
  registerNow();
  adapter->poll(*sink);
  listTwoPrograms({outrankingControl()}, "03", caps);
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);

  EXPECT_EQ(control()->status, ProgramStatus::Active);
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1"}));
  EXPECT_TRUE(limitsName(controlId)) << "it holds generation";
  EXPECT_FALSE(db->activityEntry(connection, "This device superseded"));
}

TEST_F(CsipausAdapterTest, NoStartedOrCompletedAfterItsOwnSupersession) {
  listTwoPrograms({});
  registerNow();
  adapter->poll(*sink);
  listTwoPrograms({outrankingControl()});
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);
  ASSERT_EQ(control()->status, ProgramStatus::Superseded);

  db->setLifecycle(control()->id, "control");
  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  db->setLifecycle(control()->id, "completed");
  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  advance(std::chrono::seconds(61));
  adapter->poll(*sink);

  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1", "7"}));
}

// ---------------------------------------------------------------------------
// The DER disconnecting (explainer 5.3.1): its controls are dropped for the
// default and the programs polled again, unless a cap switched it off.

TEST_F(CsipausAdapterTest, ADisconnectionAsksForAPollAndSaysSo) {
  reportState();
  disconnects();

  EXPECT_TRUE(adapter->pollWanted());
  const auto said =
      db->activityEntry(connection, "The DER disconnected from the grid");
  ASSERT_TRUE(said);
  EXPECT_EQ(said->category, ActivityCategory::Connection);
  EXPECT_NE(said->message.find("5.3.1"), std::string::npos) << said->message;
  EXPECT_TRUE(limitsName(controlId)) << "nothing is written before the poll";
}

TEST_F(CsipausAdapterTest, TheRepollFetchesTheProgramsAgainWhetherOrNotDue) {
  reportState();
  disconnects();

  EXPECT_EQ(pollAfter(std::chrono::seconds(1)),
            (std::vector<std::string>{"GET /edev/3/fsa", "GET /derp",
                                      "GET /derp/1/dderc", "GET /derp/1/derc"}))
      << "DeviceCapability, the EndDevice and Time only when due";
  EXPECT_FALSE(adapter->pollWanted());
  EXPECT_TRUE(pollAfter(std::chrono::seconds(1)).empty())
      << "then each when it is due";
}

TEST_F(CsipausAdapterTest, ASuccessfulRepollKeepsTheControlsTheServerStillLists) {
  reportState();
  disconnects();
  pollAfter(std::chrono::seconds(1));

  EXPECT_TRUE(limitsName(controlId));
  EXPECT_TRUE(limitsName(defaultId));
  EXPECT_FALSE(db->activityEntry(connection, "could not be polled again"));
}

TEST_F(CsipausAdapterTest, AFailedRepollLeavesTheDefaultsOnly) {
  reportState();
  disconnects();
  rest->queue("GET /derp/1/derc", {503, "", {}, std::nullopt});

  now += std::chrono::seconds(1);
  EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
            AdapterError::Kind::Transient);

  const auto limits = db->csipausLimits.at(connection);
  ASSERT_EQ(limits.size(), 1u);
  EXPECT_EQ(limits[0].programEventId, defaultId);
  EXPECT_EQ(limits[0].value, 1500);
  EXPECT_FALSE(limits[0].end.has_value());
  EXPECT_EQ(limits[0].setGradW, 27);
  const auto said = db->activityEntry(connection, "could not be polled again");
  ASSERT_TRUE(said);
  EXPECT_EQ(said->category, ActivityCategory::Connection);

  now += std::chrono::seconds(1);
  adapter->poll(*sink);
  EXPECT_TRUE(limitsName(controlId)) << "the next poll that succeeds";
}

TEST_F(CsipausAdapterTest, AFailedRepollIsNotRepeatedUntilTheNextPoll) {
  reportState();
  disconnects();
  rest->queue("GET /derp/1/derc", {503, "", {}, std::nullopt});
  now += std::chrono::seconds(1);
  failure([&] { adapter->poll(*sink); });

  EXPECT_FALSE(adapter->pollWanted());
  advance(std::chrono::seconds(15));
  inverterSays(0, 0);
  adapter->runDueWork(*sink);
  EXPECT_FALSE(adapter->pollWanted()) << "still disconnected: no new change";
}

TEST_F(CsipausAdapterTest, AFailedRepollBeforeAnyWalkLeavesTheCaps) {
  reportState();
  registerNow();
  inverterSays(1, 1);
  adapter->runDueWork(*sink);
  advance(std::chrono::seconds(15));
  inverterSays(0, 0);
  adapter->runDueWork(*sink);
  ASSERT_TRUE(adapter->pollWanted());
  rest->queue("GET /derp/1/derc", {503, "", {}, std::nullopt});

  failure([&] { adapter->poll(*sink); });

  EXPECT_EQ(db->csipausLimits.count(connection), 0u)
      << "no walk has said what the defaults are";
  EXPECT_TRUE(db->activityEntry(connection, "not been walked"));
}

TEST_F(CsipausAdapterTest, ADisconnectionUnderAConnectCapOfZeroKeepsTheControls) {
  listSwitchOff("opModConnect");
  reportState();
  disconnects();

  EXPECT_FALSE(adapter->pollWanted());
  EXPECT_TRUE(limitsName(controlId));
  EXPECT_TRUE(db->activityEntry(
      connection, "while a connect or energize cap of 0 is in force"));
}

TEST_F(CsipausAdapterTest, ADisconnectionUnderAnEnergizeCapOfZeroKeepsTheControls) {
  listSwitchOff("opModEnergize");
  reportState();
  disconnects();

  EXPECT_FALSE(adapter->pollWanted());
  EXPECT_TRUE(limitsName(controlId));
  EXPECT_TRUE(db->activityEntry(
      connection, "while a connect or energize cap of 0 is in force"));
}

// CSIP-AUS v1.2 explainer, section 8.2: "exponential backoff with jitter",
// two retries within the polling period, at least 10 s apart; then at the
// polling period, as the server set it.
TEST(CsipausRetryDelay, RetriesTwiceWithinThePollRateThenAtIt) {
  using namespace std::chrono_literals;
  std::mt19937 random(7);
  std::chrono::milliseconds shortest = 1h;
  std::chrono::milliseconds longest = 0ms;
  for (int draw = 0; draw < 1000; ++draw) {
    const auto first = retryDelay(1, 60s, random);
    const auto second = retryDelay(2, 60s, random);
    ASSERT_GE(first, 10s);
    ASSERT_LT(first, 15s);
    ASSERT_GE(second, 20s);
    ASSERT_LE(second, 30s);
    ASSERT_LT(first + second, 60s);
    for (const int failures : {3, 4, 12}) {
      ASSERT_EQ(retryDelay(failures, 60s, random), 60s);
    }
    shortest = std::min(shortest, first);
    longest = std::max(longest, first);
  }
  EXPECT_GT(longest - shortest, 4s) << "jittered";
}

TEST(CsipausRetryDelay, RetriesNeverCloserThanTenSeconds) {
  using namespace std::chrono_literals;
  std::mt19937 random(7);
  for (const std::chrono::milliseconds pollRate : {30s, 20s, 5s}) {
    const auto half = std::max<std::chrono::milliseconds>(pollRate / 2, 10s);
    for (int draw = 0; draw < 1000; ++draw) {
      for (const int failures : {1, 2}) {
        const auto delay = retryDelay(failures, pollRate, random);
        ASSERT_GE(delay, 10s) << pollRate.count();
        ASSERT_LE(delay, half) << pollRate.count();
      }
      ASSERT_EQ(retryDelay(3, pollRate, random),
                std::max<std::chrono::milliseconds>(pollRate, 10s))
          << pollRate.count();
    }
  }
}

TEST_F(CsipausAdapterTest, PollsAndRetriesAtTheServersRatesWhateverTheJitter) {
  using namespace std::chrono_literals;
  db->connections.at(connection).pollJitter = 0.5;
  build();
  EXPECT_FALSE(adapter->jittersPolls());
  for (int draw = 0; draw < 200; ++draw) {
    ASSERT_EQ(adapter->retryDelay(3, 60s), 60s);
    ASSERT_EQ(adapter->retryDelay(8, 60s), 60s);
    const auto first = adapter->retryDelay(1, 60s);
    ASSERT_TRUE(first);
    ASSERT_GE(*first, 10s);
    ASSERT_LT(*first, 15s);
  }
}

// CSIP-AUS v1.2 explainer, section 5.2.1: through a loss of contact the
// client keeps its schedule, then the default.
TEST_F(CsipausAdapterTest, AFailedPollLeavesTheScheduleThenTheDefault) {
  registerNow();
  adapter->poll(*sink);
  const auto stored = db->csipausLimits.at(connection);
  ASSERT_EQ(stored.size(), 2u);
  EXPECT_EQ(stored[0].programEventId, controlId);
  EXPECT_EQ(stored[1].programEventId, defaultId);
  EXPECT_FALSE(stored[1].end.has_value()) << "the default runs on";

  serverFails();
  advance(std::chrono::seconds(50));
  for (int attempt = 0; attempt < 3; ++attempt) {
    advance(std::chrono::seconds(15));
    EXPECT_EQ(failure([&] { adapter->poll(*sink); }).kind(),
              AdapterError::Kind::Transient);
    EXPECT_EQ(db->csipausLimits.at(connection), stored);
  }
}

TEST_F(CsipausAdapterTest, ResumingWhileTheServerFailsLeavesTheStoredCaps) {
  registerNow();
  adapter->poll(*sink);
  const auto stored = db->csipausLimits.at(connection);

  serverFails();
  EXPECT_EQ(failure([&] { restart(); }).kind(), AdapterError::Kind::Transient);
  EXPECT_EQ(db->csipausLimits.at(connection), stored);
}

// The control's lifecycle and the answers sent are in the database, so a
// restarted worker says started without saying received again.
TEST_F(CsipausAdapterTest, ARestartedAdapterAnswersStartedFromTheStore) {
  registerNow();
  adapter->poll(*sink);
  ASSERT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1"}));

  advance(std::chrono::seconds(30));
  ASSERT_TRUE(restart());
  db->setLifecycle(control()->id, "control");
  adapter->runDueWork(*sink);
  EXPECT_EQ(statusesSentFor(controlId).size(), 1u) << "nothing before a walk";

  adapter->poll(*sink);
  advance(std::chrono::seconds(1));
  adapter->runDueWork(*sink);
  EXPECT_EQ(statusesSentFor(controlId), (std::vector<std::string>{"1", "2"}));
  EXPECT_EQ(db->responsesFor(control()->id).back().answer,
            EventAnswer::Started);
}
