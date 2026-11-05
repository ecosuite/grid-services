#include <gtest/gtest.h>

#include <algorithm>
#include <regex>

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
constexpr std::uint32_t pen = 0x0000ABCD;
// A five-minute boundary.
constexpr long long windowStartsAt = 1790000100;

TelemetryPoint point(const std::string &pointId, const std::string &kind,
                     const std::string &units, const std::string &property) {
  TelemetryPoint telemetry;
  telemetry.pointId = pointId;
  telemetry.item.kind = kind;
  telemetry.item.units = units;
  telemetry.source.nodeId = 604;
  telemetry.source.sourceId = "/S1/" + pointId;
  telemetry.source.property = property;
  return telemetry;
}

Timestamp seconds(long long value) {
  return Timestamp(std::chrono::seconds(value));
}

class CsipausReporterTest : public ::testing::Test {
protected:
  void SetUp() override {
    ConnectionConfig config;
    config.protocol = "csipaus";
    connection = db->addConnection(config);
    store = std::make_unique<MemoryRunnerStore>(db, connection);
    sink = std::make_unique<StoreEventSink>(*store, Opt::In, *log);
    settings.dcapUrl = "https://utility.example.com/dcap";

    der.type = 4;
    der.rtgMaxW = 5000;
    der.modesSupported = {"export", "import"};
    der.modesEnabled = {"export", "import"};
    der.updatedAt = seconds(1789000000);

    sitePower = {Mirror::Site, ReadingKind::RealPower, Phase::None, 0,
                 point("site-power", "power_real", "W", "watts")};
    siteVolts = {Mirror::Site, ReadingKind::Voltage, Phase::None, -1,
                 point("site-volts", "voltage", "V", "voltage")};
    derPower = {Mirror::Der, ReadingKind::RealPower, Phase::None, 0,
                point("der-power", "power_real", "W", "watts")};
    // The site meter's readings share its stream.
    sitePower.point.source.sourceId = "/S1/site-meter";
    siteVolts.point.source.sourceId = "/S1/site-meter";
    state.connected = statePoint("der-connected", "connected");
    state.operating = statePoint("der-operating", "operating");

    rest->on("GET /dcap", fixtures::deviceCapability());
    rest->on("GET /edev",
             fixtures::endDeviceList({fixtures::reportingEndDeviceElement(
                 "/edev/3", ourLfdi, fixtures::deviceSfdi, 300)}));
    rest->on("GET /edev/3/fsa", fixtures::functionSetAssignmentsList(3600));
    rest->on("GET /derp", fixtures::derProgramList(60));
    rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500));
    rest->on("GET /derp/1/derc", fixtures::derControlList({}));
    rest->on("GET /tm", fixtures::time(windowStartsAt + 10));
    rest->on("GET /edev/3/der", fixtures::derList(60));
    rest->on("PUT /edev/3/der/1/dercap", "", 204);
    rest->on("PUT /edev/3/der/1/derg", "", 204);
    rest->on("PUT /edev/3/der/1/ders", "", 204);
    // Empty until the worker creates its mirrors.
    rest->queue("GET /mup", {200, fixtures::mirrorUsagePointList({}), {},
                             std::nullopt});
    rest->on("GET /mup",
             fixtures::mirrorUsagePointList(
                 {fixtures::mirrorUsagePointElement("/mup/1", siteMrid(),
                                                    ourLfdi, 300),
                  fixtures::mirrorUsagePointElement("/mup/2", derMrid(),
                                                    ourLfdi, 300)}));
    rest->on("GET /mup/1", fixtures::mirrorUsagePoint("/mup/1", siteMrid(),
                                                      ourLfdi, 300));
    rest->on("GET /mup/2", fixtures::mirrorUsagePoint("/mup/2", derMrid(),
                                                      ourLfdi, 300));
    rest->on("POST /mup/1", "", 201);
    rest->on("POST /mup/2", "", 201);

    // Halfway through the first window.
    reader->add(604, "/S1/site-meter",
                {seconds(windowStartsAt + 150),
                 {{"watts", -1200.4}, {"voltage", 230.14}}});
    reader->add(604, "/S1/der-power",
                {seconds(windowStartsAt + 150), {{"watts", 2000}}});
    for (const auto *stream :
         {"/S1/site-meter", "/S1/der-power", "/S1/big-power"}) {
      uploadedThrough(stream, windowStartsAt + 7200);
    }
  }

  // SolarNetwork has the stream's datums up to `at`: a datum with none of
  // the readings' properties.
  void uploadedThrough(const std::string &sourceId, long long at) {
    reader->add(604, sourceId, {seconds(at), {{"uploaded", 1}}});
  }

  // The inverter's state, both on one stream.
  static TelemetryPoint statePoint(const std::string &pointId,
                                   const std::string &property) {
    auto telemetry = point(pointId, "state", "none", property);
    telemetry.source.sourceId = "/S1/der-inv";
    telemetry.source.reduce = datum::Reduce::Last;
    return telemetry;
  }

  void inverterSays(long long at, double isConnected, double isOperating) {
    reader->add(604, "/S1/der-inv",
                {seconds(at),
                 {{"connected", isConnected}, {"operating", isOperating}}});
  }

  std::string siteMrid() const { return mirrorMrid(ourLfdi, Mirror::Site, pen); }
  std::string derMrid() const { return mirrorMrid(ourLfdi, Mirror::Der, pen); }

  // The server's answers to the worker creating its mirrors, site first.
  void createsMirrors(const std::vector<BoundReading> &readings) {
    for (const auto mirror : {Mirror::Site, Mirror::Der}) {
      if (std::any_of(readings.begin(), readings.end(),
                      [mirror](const auto &r) { return r.mirror == mirror; })) {
        rest->queue("POST /mup",
                    {201, "",
                     {{"Location", mirror == Mirror::Site ? "/mup/1" : "/mup/2"}},
                     std::nullopt});
      }
    }
  }

  void build(std::vector<BoundReading> readings) {
    createsMirrors(readings);
    ReportingSetup setup;
    if (describesDer) {
      setup.der = der;
    }
    setup.state = state;
    setup.readings = std::move(readings);
    setup.reader = reader;
    setup.pen = pen;
    adapter = std::make_unique<CsipausAdapter>(
        db->connections.at(connection), settings,
        DeviceIdentity{ourLfdi, fixtures::deviceSfdi},
        std::make_unique<ScriptedRest>(rest), *store, [this] { return now; },
        [this] { return wallNow; }, std::move(setup));
    adapter->registerConnection(*sink);
  }

  void buildWithAllReadings() { build({sitePower, siteVolts, derPower}); }

  // Runs due work at `at`, by the worker's clock, which here is the
  // server's.
  void runAt(long long at) {
    moveTo(at);
    adapter->runDueWork(*sink);
  }

  // Polls at `at`, then runs due work, as the runner does.
  void pollAt(long long at) {
    moveTo(at);
    adapter->poll(*sink);
    adapter->runDueWork(*sink);
  }

  void moveTo(long long at) {
    wallNow = seconds(at);
    now += std::chrono::seconds(at - lastRun);
    lastRun = at;
  }

  // Each DERSettings sent, without the whitespace between its elements.
  std::vector<std::string> settingsSent() {
    std::vector<std::string> bodies;
    for (const auto &put : rest->requestsTo("PUT /edev/3/der/1/derg")) {
      bodies.push_back(std::regex_replace(put.body, std::regex(">\\s+<"), "><"));
    }
    return bodies;
  }

  std::vector<ReportDelivery> deliveries(const std::string &report) {
    std::vector<ReportDelivery> found;
    for (const auto &delivery : db->reportDeliveriesFor(connection)) {
      if (delivery.report == report) {
        found.push_back(delivery);
      }
    }
    return found;
  }

  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  ConnectionId connection = 0;
  std::unique_ptr<MemoryRunnerStore> store;
  std::shared_ptr<spdlog::logger> log = makeLogger("test");
  std::unique_ptr<StoreEventSink> sink;
  CsipausSettings settings;
  DerDescription der;
  bool describesDer = true;
  StatePoints state;
  BoundReading sitePower;
  BoundReading siteVolts;
  BoundReading derPower;
  std::shared_ptr<RestScript> rest = std::make_shared<RestScript>();
  std::shared_ptr<FakeDatumReader> reader = std::make_shared<FakeDatumReader>();
  Clock::time_point now = Clock::time_point(std::chrono::hours(1000));
  long long lastRun = windowStartsAt + 10;
  Timestamp wallNow = seconds(windowStartsAt + 10);
  std::unique_ptr<CsipausAdapter> adapter;
};

// Whether `part` is in `text`, ignoring the whitespace between elements.
bool contains(const std::string &text, const std::string &part) {
  return std::regex_replace(text, std::regex(">\\s+<"), "><").find(part) !=
         std::string::npos;
}

} // namespace

TEST_F(CsipausReporterTest, DescribesTheDerOnceRegistered) {
  build({});
  pollAt(windowStartsAt + 10);

  ASSERT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 1u);
  EXPECT_TRUE(contains(rest->requestsTo("PUT /edev/3/der/1/dercap")[0].body,
                       "<csipaus:doeModesSupported>03"));
  ASSERT_EQ(rest->count("PUT /edev/3/der/1/derg"), 1u);
  EXPECT_TRUE(contains(rest->requestsTo("PUT /edev/3/der/1/derg")[0].body,
                       "<updatedTime>1789000000</updatedTime>"));

  const auto capability = deliveries("der_capability");
  ASSERT_EQ(capability.size(), 1u);
  EXPECT_TRUE(capability[0].ok);
  EXPECT_EQ(capability[0].payload[1]["rid"], "rtg_max_w");
  EXPECT_EQ(capability[0].payload[1]["value"], 5000);
  ASSERT_TRUE(capability[0].exchange.is_array());
  EXPECT_FALSE(capability[0].exchange.empty())
      << "the PUT is kept with the delivery";
  EXPECT_EQ(deliveries("der_settings").size(), 1u);

  runAt(windowStartsAt + 100);
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 1u)
      << "described once per registration";
  EXPECT_EQ(rest->count("GET /mup"), 0u) << "no readings, no mirrors";
}

TEST_F(CsipausReporterTest, SettingsWaitForTheProgramsToBeWalked) {
  build({});
  runAt(windowStartsAt + 10);
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 1u)
      << "the capability does not wait";
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/derg"), 0u)
      << "whether a default sets setGradW is not known yet";
  EXPECT_FALSE(adapter->dueWorkIn().has_value() &&
               *adapter->dueWorkIn() < std::chrono::seconds(30))
      << "nothing to wake for until a poll";

  pollAt(windowStartsAt + 20);
  ASSERT_EQ(settingsSent().size(), 1u);
  EXPECT_TRUE(contains(settingsSent()[0], "<setGradW>27</setGradW>"));
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 1u);
}

TEST_F(CsipausReporterTest, SettingsCarryTheDefaultsSetGradW) {
  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, 100));
  build({});
  pollAt(windowStartsAt + 10);

  ASSERT_EQ(settingsSent().size(), 1u);
  EXPECT_TRUE(contains(settingsSent()[0], "<setGradW>100</setGradW>"));
  EXPECT_TRUE(contains(settingsSent()[0], "<updatedTime>" +
                                              std::to_string(windowStartsAt + 10) +
                                              "</updatedTime>"))
      << "the walk that saw it, by the server's clock: " << settingsSent()[0];
  const auto settings = deliveries("der_settings");
  ASSERT_EQ(settings.size(), 1u);
  const auto &payload = settings[0].payload;
  const auto gradW = std::find_if(payload.begin(), payload.end(), [](const auto &entry) {
    return entry["rid"] == "set_grad_w";
  });
  ASSERT_NE(gradW, payload.end());
  EXPECT_EQ((*gradW)["value"], 100);
}

TEST_F(CsipausReporterTest, SettingsCarryTheDersSetGradWWhenNoDefaultSetsOne) {
  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, std::nullopt));
  der.setGradW = 50;
  build({});
  pollAt(windowStartsAt + 10);

  ASSERT_EQ(settingsSent().size(), 1u);
  EXPECT_TRUE(contains(settingsSent()[0], "<setGradW>50</setGradW>"));
  EXPECT_TRUE(contains(settingsSent()[0], "<updatedTime>1789000000</updatedTime>"));
}

TEST_F(CsipausReporterTest, AChangedSetGradWSendsTheSettingsAloneAgain) {
  build({});
  pollAt(windowStartsAt + 10);
  ASSERT_EQ(settingsSent().size(), 1u);

  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, 100));
  pollAt(windowStartsAt + 70);
  ASSERT_EQ(settingsSent().size(), 2u);
  EXPECT_TRUE(contains(settingsSent()[1], "<setGradW>100</setGradW>"));
  EXPECT_TRUE(contains(settingsSent()[1], "<updatedTime>" +
                                              std::to_string(windowStartsAt + 70) +
                                              "</updatedTime>"));
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 1u)
      << "the capability has not changed";

  pollAt(windowStartsAt + 130);
  EXPECT_EQ(settingsSent().size(), 2u) << "unchanged, not sent again";

  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, 27));
  pollAt(windowStartsAt + 190);
  ASSERT_EQ(settingsSent().size(), 3u);
  EXPECT_TRUE(contains(settingsSent()[2], "<setGradW>27</setGradW>"));
  EXPECT_TRUE(contains(settingsSent()[2], "<updatedTime>" +
                                              std::to_string(windowStartsAt + 190) +
                                              "</updatedTime>"))
      << "a change back is a change";
}

TEST_F(CsipausReporterTest, ADefaultThatAgreesWithTheDerSendsNothingNew) {
  build({});
  pollAt(windowStartsAt + 10);
  pollAt(windowStartsAt + 70);
  ASSERT_EQ(settingsSent().size(), 1u);
  EXPECT_TRUE(contains(settingsSent()[0], "<setGradW>27</setGradW>"));
  EXPECT_TRUE(contains(settingsSent()[0], "<updatedTime>1789000000</updatedTime>"));
}

TEST_F(CsipausReporterTest, ARefusedSettingsChangeIsSentAgainLater) {
  build({});
  pollAt(windowStartsAt + 10);
  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500, 100));
  rest->queue("PUT /edev/3/der/1/derg", {500, "", {}, std::nullopt});
  EXPECT_THROW(pollAt(windowStartsAt + 70), AdapterError);
  ASSERT_EQ(settingsSent().size(), 2u);
  EXPECT_FALSE(deliveries("der_settings").back().ok);

  runAt(windowStartsAt + 70 + describeRetry.count() - 1);
  EXPECT_EQ(settingsSent().size(), 2u);
  runAt(windowStartsAt + 70 + describeRetry.count());
  ASSERT_EQ(settingsSent().size(), 3u);
  EXPECT_TRUE(contains(settingsSent()[2], "<setGradW>100</setGradW>"));
  EXPECT_TRUE(contains(settingsSent()[2], "<updatedTime>" +
                                              std::to_string(windowStartsAt + 70) +
                                              "</updatedTime>"))
      << "the time it changed, not the time it was sent";
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 1u);
}

TEST_F(CsipausReporterTest, RegisteringAgainWaitsForTheNextWalk) {
  build({});
  pollAt(windowStartsAt + 10);
  adapter->registerConnection(*sink);
  runAt(windowStartsAt + 20);
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 2u);
  EXPECT_EQ(settingsSent().size(), 1u);

  pollAt(windowStartsAt + 30);
  EXPECT_EQ(settingsSent().size(), 2u);
}

TEST_F(CsipausReporterTest, CreatesItsMirrorsAndPostsEachWindowOnceItEnds) {
  buildWithAllReadings();
  runAt(windowStartsAt + 10);

  const auto created = rest->requestsTo("POST /mup");
  ASSERT_EQ(created.size(), 2u);
  EXPECT_TRUE(contains(created[0].body, "<mRID>" + siteMrid() + "</mRID>"));
  EXPECT_TRUE(contains(created[0].body, "<roleFlags>03</roleFlags>"));
  EXPECT_TRUE(contains(created[1].body, "<roleFlags>49</roleFlags>"));
  EXPECT_EQ(rest->count("POST /mup/1"), 0u) << "the window is under way";

  runAt(windowStartsAt + 300 + 10);
  EXPECT_EQ(rest->count("POST /mup/1"), 0u) << "datums are given time to arrive";

  runAt(windowStartsAt + 300 + readingSettle.count());
  const auto site = rest->requestsTo("POST /mup/1");
  ASSERT_EQ(site.size(), 1u);
  EXPECT_TRUE(contains(site[0].body, "<value>-1200</value>"));
  EXPECT_TRUE(contains(site[0].body, "<value>2301</value>"));
  EXPECT_TRUE(contains(site[0].body, "<duration>300</duration><start>" +
                                         std::to_string(windowStartsAt) +
                                         "</start>"));
  EXPECT_EQ(rest->count("POST /mup/2"), 1u);

  const auto posted = deliveries("site_readings");
  ASSERT_EQ(posted.size(), 1u);
  EXPECT_TRUE(posted[0].ok);
  EXPECT_EQ(posted[0].intervalStart, seconds(windowStartsAt));
  EXPECT_EQ(posted[0].intervalEnd, seconds(windowStartsAt + 300));
  ASSERT_EQ(posted[0].payload.size(), 2u);
  EXPECT_EQ(posted[0].payload[1]["rid"], "voltage");
  EXPECT_DOUBLE_EQ(posted[0].payload[1]["value"].get<double>(), 230.1)
      << "what the server receives";
  EXPECT_EQ(posted[0].payload[1]["quality"], "good");
  EXPECT_EQ(posted[0].reads.size(), 2u);
}

TEST_F(CsipausReporterTest, ReportsTheDersStatusFromItsStatePoints) {
  inverterSays(windowStartsAt + 5, 1, 1);
  buildWithAllReadings();
  runAt(windowStartsAt + 10);

  const auto status = rest->requestsTo("PUT /edev/3/der/1/ders");
  ASSERT_EQ(status.size(), 1u) << "sent at once, with no window to wait for";
  EXPECT_TRUE(contains(status[0].body, "<genConnectStatus><dateTime>" +
                                           std::to_string(windowStartsAt + 10) +
                                           "</dateTime><value>07</value>"))
      << "connected, available and operating";
  EXPECT_TRUE(contains(status[0].body, "<operationalModeStatus><dateTime>" +
                                           std::to_string(windowStartsAt + 10) +
                                           "</dateTime><value>2</value>"));

  const auto sent = deliveries("der_status");
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_TRUE(sent[0].ok);
  const auto entry = [&](const std::string &rid) {
    for (const auto &item : sent[0].payload) {
      if (item["rid"] == rid) {
        return item;
      }
    }
    ADD_FAILURE() << "no " << rid << " entry";
    return nlohmann::json::object();
  };
  EXPECT_EQ(entry("gen_connect_status")["value"], 7);
  EXPECT_EQ(entry("operational_mode_status")["value"], 2);
  EXPECT_EQ(entry("connected")["point_id"], "der-connected");
  EXPECT_EQ(entry("connected")["value"], 1);
  EXPECT_EQ(entry("connected")["quality"], "good");
  EXPECT_EQ(entry("operating")["point_id"], "der-operating");
  EXPECT_EQ(entry("operating")["source"]["property"], "operating");
  ASSERT_EQ(sent[0].reads.size(), 1u) << "one stream, read once";
  EXPECT_EQ(sent[0].reads[0]["kind"], "most_recent");
  EXPECT_EQ(sent[0].reads[0]["source_id"], "/S1/der-inv");

  runAt(windowStartsAt + 40);
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/ders"), 1u) << "unchanged";
  runAt(windowStartsAt + 70);
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/ders"), 2u)
      << "sent again at the DERList's poll rate";

  inverterSays(windowStartsAt + 75, 1, 0);
  runAt(windowStartsAt + 85);
  const auto latest = rest->requestsTo("PUT /edev/3/der/1/ders");
  ASSERT_EQ(latest.size(), 3u) << "a change is sent at once";
  EXPECT_TRUE(contains(latest.back().body, "<value>03</value>"));
  EXPECT_TRUE(contains(latest.back().body,
                       "</dateTime><value>1</value></operationalModeStatus>"));
}

TEST_F(CsipausReporterTest, AConnectionChangeIsSentAtOnce) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);

  inverterSays(windowStartsAt + 15, 0, 1);
  runAt(windowStartsAt + 25);
  inverterSays(windowStartsAt + 30, 1, 1);
  runAt(windowStartsAt + 40);

  const auto status = rest->requestsTo("PUT /edev/3/der/1/ders");
  const std::vector<std::string> connectStatus = {"07", "04", "07"};
  ASSERT_EQ(status.size(), connectStatus.size());
  for (std::size_t i = 0; i < status.size(); ++i) {
    EXPECT_TRUE(contains(status[i].body, "<value>" + connectStatus[i] +
                                             "</value></genConnectStatus>"))
        << "PUT " << i;
    EXPECT_TRUE(contains(
        status[i].body, "</dateTime><value>2</value></operationalModeStatus>"))
        << "PUT " << i << ": isolated, it still operates";
  }
}

TEST_F(CsipausReporterTest, AnOperatingChangeIsSentAtOnceBothWays) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);

  inverterSays(windowStartsAt + 15, 1, 0);
  runAt(windowStartsAt + 25);
  inverterSays(windowStartsAt + 30, 1, 1);
  runAt(windowStartsAt + 40);

  const auto status = rest->requestsTo("PUT /edev/3/der/1/ders");
  const std::vector<std::string> connectStatus = {"07", "03", "07"};
  const std::vector<std::string> mode = {"2", "1", "2"};
  ASSERT_EQ(status.size(), connectStatus.size());
  for (std::size_t i = 0; i < status.size(); ++i) {
    EXPECT_TRUE(contains(status[i].body, "<value>" + connectStatus[i] +
                                             "</value></genConnectStatus>"))
        << "PUT " << i;
    EXPECT_TRUE(contains(status[i].body, "</dateTime><value>" + mode[i] +
                                             "</value></operationalModeStatus>"))
        << "PUT " << i;
  }
}

// Explainer 5.3.1: a DER that loses its connection to the grid drops its
// controls for the default and polls its programs again. Nothing caps it
// off here, so the adapter asks to be polled.
TEST_F(CsipausReporterTest, ReportsADisconnectionWhenConnectedGoesFromOneToZero) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);
  EXPECT_FALSE(adapter->pollWanted());

  inverterSays(windowStartsAt + 15, 0, 0);
  runAt(windowStartsAt + 25);
  EXPECT_TRUE(adapter->pollWanted());
  EXPECT_TRUE(db->activityEntry(connection, "The DER disconnected from the grid"));
}

TEST_F(CsipausReporterTest, ADisconnectionIsReportedOnce) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);
  inverterSays(windowStartsAt + 15, 0, 0);
  runAt(windowStartsAt + 25);
  ASSERT_TRUE(adapter->pollWanted());

  pollAt(windowStartsAt + 26);
  EXPECT_FALSE(adapter->pollWanted()) << "the poll took it";
  runAt(windowStartsAt + 40);
  runAt(windowStartsAt + 70);
  EXPECT_FALSE(adapter->pollWanted()) << "still disconnected: no new change";
}

TEST_F(CsipausReporterTest, AFirstReadOfZeroIsNoDisconnection) {
  inverterSays(windowStartsAt + 5, 0, 0);
  build({});
  runAt(windowStartsAt + 10);
  runAt(windowStartsAt + 25);

  EXPECT_FALSE(adapter->pollWanted()) << "no 1 before it";
  EXPECT_FALSE(db->activityEntry(connection, "disconnected"));
}

TEST_F(CsipausReporterTest, AFirstReadAfterRegisteringAgainIsNoDisconnection) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);

  adapter->registerConnection(*sink);
  inverterSays(windowStartsAt + 15, 0, 0);
  runAt(windowStartsAt + 25);
  EXPECT_FALSE(adapter->pollWanted()) << "registering again forgets the 1";
}

TEST_F(CsipausReporterTest, AnUnreadableReadBetweenOneAndZeroStillDisconnects) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);

  reader->failure = "SolarNetwork is down";
  runAt(windowStartsAt + 25);
  EXPECT_FALSE(adapter->pollWanted()) << "an unreadable value is no value";
  reader->failure.reset();
  inverterSays(windowStartsAt + 30, 0, 0);
  runAt(windowStartsAt + 40);
  EXPECT_TRUE(adapter->pollWanted()) << "the 1 before it still counts";
}

TEST_F(CsipausReporterTest, AStaleZeroAfterOneDisconnects) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);

  inverterSays(windowStartsAt + 15, 0, 0);
  runAt(windowStartsAt + 15 + 700);
  EXPECT_TRUE(adapter->pollWanted()) << "a stale value still sets its bits";
}

TEST_F(CsipausReporterTest, ZeroToOneIsNoDisconnection) {
  inverterSays(windowStartsAt + 5, 0, 0);
  build({});
  runAt(windowStartsAt + 10);
  inverterSays(windowStartsAt + 15, 1, 1);
  runAt(windowStartsAt + 25);

  EXPECT_FALSE(adapter->pollWanted());
}

TEST_F(CsipausReporterTest, AStaleStateIsSentAsAFault) {
  inverterSays(windowStartsAt + 10 - 700, 1, 1);
  build({});
  runAt(windowStartsAt + 10);

  const auto status = rest->requestsTo("PUT /edev/3/der/1/ders");
  ASSERT_EQ(status.size(), 1u);
  EXPECT_TRUE(contains(status[0].body, "<value>15</value>"))
      << "connected and operating by the last value, but not available";
  EXPECT_TRUE(contains(status[0].body,
                       "</dateTime><value>2</value></operationalModeStatus>"));
  EXPECT_EQ(deliveries("der_status")[0].payload[2]["quality"], "stale");
}

TEST_F(CsipausReporterTest, StatusIsNotInferredFromPower) {
  reader->add(604, "/S1/der-power",
              {seconds(windowStartsAt + 5), {{"watts", 2500}}});
  buildWithAllReadings();
  runAt(windowStartsAt + 10);
  runAt(windowStartsAt + 300 + readingSettle.count());

  const auto status = rest->requestsTo("PUT /edev/3/der/1/ders");
  ASSERT_FALSE(status.empty());
  for (const auto &request : status) {
    EXPECT_TRUE(contains(request.body, "<value>10</value>"))
        << "the inverter has said nothing of its state";
    EXPECT_FALSE(contains(request.body, "<operationalModeStatus>"));
  }
  EXPECT_EQ(deliveries("der_status")[0].payload[1]["quality"], "no_value");
}

TEST_F(CsipausReporterTest, AStateThatCannotBeReadIsSentAsAFault) {
  inverterSays(windowStartsAt + 5, 1, 1);
  reader->failure = "SolarNetwork is down";
  build({});
  runAt(windowStartsAt + 10);

  const auto status = rest->requestsTo("PUT /edev/3/der/1/ders");
  ASSERT_EQ(status.size(), 1u) << "not a connection error";
  EXPECT_TRUE(contains(status[0].body, "<value>10</value>"));
  EXPECT_FALSE(contains(status[0].body, "<operationalModeStatus>"));
  const auto sent = deliveries("der_status");
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_TRUE(sent[0].ok);
  EXPECT_EQ(sent[0].payload[1]["quality"], "unreadable");
  EXPECT_EQ(sent[0].payload[2]["quality"], "unreadable");
  ASSERT_EQ(sent[0].reads.size(), 1u);
  EXPECT_EQ(sent[0].reads[0]["error"], "SolarNetwork is down");
}

TEST_F(CsipausReporterTest, AnUnboundStateSendsNoStatus) {
  inverterSays(windowStartsAt + 5, 1, 1);
  state.operating.reset();
  build({});
  runAt(windowStartsAt + 10);

  EXPECT_EQ(rest->count("PUT /edev/3/der/1/ders"), 0u);
  const auto recorded = deliveries("der_status");
  ASSERT_EQ(recorded.size(), 1u);
  EXPECT_FALSE(recorded[0].ok);
  EXPECT_TRUE(contains(recorded[0].message, "operating state is not bound"))
      << recorded[0].message;
  EXPECT_FALSE(contains(recorded[0].message, "connected"));

  runAt(windowStartsAt + 40);
  EXPECT_EQ(deliveries("der_status").size(), 1u);
  runAt(windowStartsAt + 70);
  EXPECT_EQ(deliveries("der_status").size(), 2u)
      << "said again at the DERList's poll rate";
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/ders"), 0u);
}

TEST_F(CsipausReporterTest, ADerNotDescribedSendsNoStatus) {
  describesDer = false;
  state = {};
  build({sitePower});
  runAt(windowStartsAt + 10);

  EXPECT_EQ(rest->count("PUT /edev/3/der/1/dercap"), 0u);
  EXPECT_EQ(rest->count("PUT /edev/3/der/1/ders"), 0u);
  const auto recorded = deliveries("der_status");
  ASSERT_EQ(recorded.size(), 1u);
  EXPECT_FALSE(recorded[0].ok);
  EXPECT_TRUE(contains(recorded[0].message, "the DER is not described"))
      << recorded[0].message;
}

TEST_F(CsipausReporterTest, AWindowWithoutValuesIsRecordedButNotPosted) {
  build({siteVolts});
  runAt(windowStartsAt + 10);
  runAt(windowStartsAt + 300 + readingSettle.count());
  runAt(windowStartsAt + 600 + readingSettle.count());

  EXPECT_EQ(rest->count("POST /mup/1"), 1u) << "only the first window had data";
  const auto posted = deliveries("site_readings");
  ASSERT_EQ(posted.size(), 2u);
  EXPECT_FALSE(posted[1].ok);
  EXPECT_TRUE(contains(posted[1].message, "Nothing was posted"));
  EXPECT_EQ(posted[1].payload[0]["quality"], "no_value");
}

TEST_F(CsipausReporterTest, AWindowTheServerRefusedIsPostedAgainLater) {
  reader->add(604, "/S1/site-meter",
              {seconds(windowStartsAt + 450), {{"watts", -900}}});
  build({sitePower});
  runAt(windowStartsAt + 10);
  rest->queue("POST /mup/1", {500, "", {}, std::nullopt});

  try {
    runAt(windowStartsAt + 300 + readingSettle.count());
    ADD_FAILURE() << "expected the failure to be raised";
  } catch (const AdapterError &error) {
    EXPECT_EQ(error.kind(), AdapterError::Kind::Transient);
    EXPECT_TRUE(contains(error.what(), "Site readings was not taken"));
  }
  ASSERT_EQ(deliveries("site_readings").size(), 1u);
  EXPECT_FALSE(deliveries("site_readings")[0].ok);

  runAt(windowStartsAt + 300 + readingSettle.count() + 30);
  EXPECT_EQ(rest->count("POST /mup/1"), 1u) << "waits a post rate";

  // By then the next window has ended too: both are posted, oldest first.
  runAt(windowStartsAt + 600 + readingSettle.count());
  const auto posts = rest->requestsTo("POST /mup/1");
  ASSERT_EQ(posts.size(), 3u);
  EXPECT_TRUE(contains(posts[1].body, "<start>" + std::to_string(windowStartsAt) +
                                          "</start>"));
  EXPECT_TRUE(contains(posts[2].body,
                       "<start>" + std::to_string(windowStartsAt + 300) +
                           "</start>"));
}

TEST_F(CsipausReporterTest, FindsItsMirrorsAgainAndFollowsTheirPostRate) {
  rest->queued["GET /mup"].clear();
  rest->on("GET /mup",
           fixtures::mirrorUsagePointList({fixtures::mirrorUsagePointElement(
               "/mup/7", siteMrid(), ourLfdi, 60)}));
  rest->queue("POST /mup", {201, "", {{"Location", "/mup/7"}}, std::nullopt});
  rest->on("GET /mup/7",
           fixtures::mirrorUsagePoint("/mup/7", siteMrid(), ourLfdi, 60));
  rest->on("POST /mup/7", "", 201);
  reader->add(604, "/S1/site-meter",
              {seconds(windowStartsAt + 30), {{"watts", -800}}});
  build({sitePower});
  rest->queued["POST /mup"].clear();
  rest->queue("POST /mup", {201, "", {{"Location", "/mup/7"}}, std::nullopt});

  runAt(windowStartsAt + 10);
  EXPECT_EQ(rest->count("POST /mup"), 1u)
      << "posted once after starting, defining its readings afresh";
  runAt(windowStartsAt + 60 + readingSettle.count());
  const auto posts = rest->requestsTo("POST /mup/7");
  ASSERT_EQ(posts.size(), 1u);
  EXPECT_TRUE(contains(posts[0].body, "<duration>60</duration>"));

  runAt(windowStartsAt + 120 + readingSettle.count());
  EXPECT_EQ(rest->count("POST /mup"), 1u) << "found, not created again";
}

TEST_F(CsipausReporterTest, CreatesAMirrorAgainWhenTheServerHasRemovedIt) {
  reader->add(604, "/S1/site-meter",
              {seconds(windowStartsAt + 450), {{"watts", -900}}});
  build({sitePower});
  runAt(windowStartsAt + 10);
  rest->queue("POST /mup/1", {404, "", {}, std::nullopt});
  EXPECT_THROW(runAt(windowStartsAt + 300 + readingSettle.count()),
               AdapterError);

  rest->queue("POST /mup", {201, "", {{"Location", "/mup/1"}}, std::nullopt});
  runAt(windowStartsAt + 600 + readingSettle.count());
  EXPECT_EQ(rest->count("POST /mup"), 2u);
  EXPECT_EQ(rest->count("POST /mup/1"), 3u)
      << "the refused window and the next";
}

TEST_F(CsipausReporterTest, AValueOutOfRangeIsRecordedButNotPosted) {
  auto bigPower = sitePower;
  bigPower.point = point("big-power", "power_real", "W", "bigWatts");
  bigPower.point.source.sourceId = "/S1/site-meter";
  reader->add(604, "/S1/site-meter",
              {seconds(windowStartsAt + 150), {{"bigWatts", 40000}}});
  build({bigPower, siteVolts});
  runAt(windowStartsAt + 10);
  runAt(windowStartsAt + 300 + readingSettle.count());

  const auto posts = rest->requestsTo("POST /mup/1");
  ASSERT_EQ(posts.size(), 1u);
  EXPECT_FALSE(contains(posts[0].body, readingMrid(ourLfdi, bigPower, pen)))
      << "40 kW is past an Int16 of watts";
  EXPECT_TRUE(contains(posts[0].body, readingMrid(ourLfdi, siteVolts, pen)));
  const auto posted = deliveries("site_readings");
  ASSERT_EQ(posted.size(), 1u);
  EXPECT_TRUE(posted[0].ok);
  EXPECT_EQ(posted[0].payload[0]["rid"], "real_power");
  EXPECT_EQ(posted[0].payload[0]["quality"], "out_of_range");
  EXPECT_DOUBLE_EQ(posted[0].payload[0]["value"].get<double>(), 40000)
      << "the window's value, in watts";
  EXPECT_EQ(posted[0].payload[1]["quality"], "good");
}

TEST_F(CsipausReporterTest, AWindowWithOnlyValuesOutOfRangeIsNotPosted) {
  auto bigPower = sitePower;
  bigPower.point = point("big-power", "power_real", "W", "watts");
  reader->add(604, "/S1/big-power",
              {seconds(windowStartsAt + 150), {{"watts", 40000}}});
  build({bigPower});
  runAt(windowStartsAt + 10);
  runAt(windowStartsAt + 300 + readingSettle.count());

  EXPECT_EQ(rest->count("POST /mup/1"), 0u);
  const auto posted = deliveries("site_readings");
  ASSERT_EQ(posted.size(), 1u);
  EXPECT_FALSE(posted[0].ok);
  EXPECT_TRUE(contains(posted[0].message, "Nothing was posted"))
      << posted[0].message;
  EXPECT_EQ(posted[0].payload[0]["quality"], "out_of_range");
}

// The CSIP-AUS v1.2 explainer's section 8.1: within half the post rate.
TEST_F(CsipausReporterTest, PostsEachWindowWithinHalfItsPostRate) {
  // Nothing else falls due before the window.
  rest->on("GET /edev/3/der", fixtures::derList(900));
  rest->queued["GET /mup"].clear();
  rest->queue("GET /mup", {200, fixtures::mirrorUsagePointList({}, 900), {},
                           std::nullopt});
  rest->on("GET /mup/1",
           fixtures::mirrorUsagePoint("/mup/1", siteMrid(), ourLfdi, 60));
  reader->add(604, "/S1/site-meter",
              {seconds(windowStartsAt + 30), {{"watts", -800}}});
  inverterSays(windowStartsAt + 5, 1, 1);
  build({sitePower});
  runAt(windowStartsAt + 10);
  EXPECT_EQ(adapter->dueWorkIn(), std::chrono::milliseconds(65'000))
      << "the window ends 60 s in, and is posted 15 s later";

  runAt(windowStartsAt + 74);
  EXPECT_EQ(rest->count("POST /mup/1"), 0u);
  EXPECT_EQ(adapter->dueWorkIn(), std::chrono::milliseconds(1'000));
  runAt(windowStartsAt + 75);
  const auto posts = rest->requestsTo("POST /mup/1");
  ASSERT_EQ(posts.size(), 1u);
  EXPECT_TRUE(contains(posts[0].body, "<duration>60</duration><start>" +
                                          std::to_string(windowStartsAt) +
                                          "</start>"));

  EXPECT_EQ(windowPostDelay(std::chrono::seconds(60)), std::chrono::seconds(15));
  EXPECT_EQ(windowPostDelay(std::chrono::seconds(300)), readingSettle);
  EXPECT_EQ(windowPostDelay(std::chrono::seconds(4)), std::chrono::seconds(1));
}

TEST_F(CsipausReporterTest, ALaterRunIsDueForAListButNotAtOnceForOneThatFailed) {
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);
  EXPECT_EQ(adapter->dueWorkIn(), std::chrono::milliseconds(60'000))
      << "the DERList and DERStatus, at the DERList's rate";

  rest->queue("GET /edev/3/der", {503, "", {}, std::nullopt});
  EXPECT_THROW(runAt(windowStartsAt + 70), AdapterError);
  EXPECT_EQ(adapter->dueWorkIn(), std::chrono::milliseconds(60'000))
      << "the next DERStatus; the DERList waits for the runner's heartbeat";
}

TEST_F(CsipausReporterTest, WakesNoSoonerThanASecond) {
  rest->on("GET /edev/3/der", fixtures::derList(0));
  inverterSays(windowStartsAt + 5, 1, 1);
  build({});
  runAt(windowStartsAt + 10);
  EXPECT_EQ(adapter->dueWorkIn(), std::chrono::milliseconds(1'000))
      << "a DERList and DERStatus due again at once";
}

TEST_F(CsipausReporterTest, TakesTheMirrorListFromTheDeviceCapability) {
  buildWithAllReadings();
  runAt(windowStartsAt + 10);

  EXPECT_EQ(rest->count("POST /mup"), 2u);
  EXPECT_EQ(rest->count("GET /dcap"), 1u)
      << "registering's, which polling and reporting share";
}

TEST_F(CsipausReporterTest, FollowsTheLatestDeviceCapabilitysMirrorList) {
  rest->on("GET /dcap", fixtures::deviceCapability(60));
  rest->on("GET /derp/1/dderc", fixtures::defaultDerControl(1500));
  rest->on("GET /derp/1/derc", fixtures::derControlList({}));
  rest->on("GET /tm", fixtures::time(windowStartsAt + 71));
  build({sitePower});
  runAt(windowStartsAt + 10);
  ASSERT_EQ(rest->count("POST /mup"), 1u);

  rest->on("GET /dcap", fixtures::deviceCapability(60, "/tm", "/mup2"));
  rest->on("GET /mup2", fixtures::mirrorUsagePointList({}));
  rest->queue("POST /mup2", {201, "", {{"Location", "/mup2/1"}}, std::nullopt});
  rest->on("GET /mup2/1",
           fixtures::mirrorUsagePoint("/mup2/1", siteMrid(), ourLfdi, 300));
  now += std::chrono::seconds(61);
  wallNow = seconds(windowStartsAt + 71);
  lastRun = windowStartsAt + 71;
  adapter->poll(*sink);
  adapter->runDueWork(*sink);

  EXPECT_EQ(rest->count("GET /dcap"), 2u);
  EXPECT_EQ(rest->count("POST /mup2"), 1u) << "created in the new list";
  EXPECT_EQ(rest->count("POST /mup"), 1u);
}

TEST_F(CsipausReporterTest, AWindowWaitsForItsDatumsAndIsPostedLate) {
  const BoundReading latePower{Mirror::Site, ReadingKind::RealPower,
                               Phase::None, 0,
                               point("late-power", "power_real", "W", "watts")};
  reader->add(604, "/S1/late-power",
              {seconds(windowStartsAt + 100), {{"watts", -1000}}});
  build({latePower});
  runAt(windowStartsAt + 10);

  runAt(windowStartsAt + 300 + readingSettle.count());
  EXPECT_EQ(rest->count("POST /mup/1"), 0u)
      << "SolarNetwork has nothing past the window's end";
  EXPECT_TRUE(deliveries("site_readings").empty()) << "waiting is not a delivery";
  EXPECT_EQ(adapter->dueWorkIn(),
            std::chrono::milliseconds(readingRecheck.count() * 1000));

  // The rest of the window arrives a minute and a half late.
  reader->add(604, "/S1/late-power",
              {seconds(windowStartsAt + 250), {{"watts", -2000}}});
  reader->add(604, "/S1/late-power",
              {seconds(windowStartsAt + 305), {{"watts", -2000}}});
  runAt(windowStartsAt + 390);
  const auto posts = rest->requestsTo("POST /mup/1");
  ASSERT_EQ(posts.size(), 1u);
  EXPECT_TRUE(contains(posts[0].body, "<value>-1500</value>"))
      << "the whole window, not the part there was when it ended";
  EXPECT_TRUE(contains(posts[0].body, "<duration>300</duration><start>" +
                                          std::to_string(windowStartsAt) +
                                          "</start>"));
  const auto posted = deliveries("site_readings");
  ASSERT_EQ(posted.size(), 1u);
  EXPECT_TRUE(posted[0].ok);
  ASSERT_EQ(posted[0].reads.size(), 2u);
  EXPECT_EQ(posted[0].reads[0]["kind"], "most_recent");
  EXPECT_EQ(posted[0].reads[1]["kind"], "list");
}

TEST_F(CsipausReporterTest, ALateStreamDoesNotHoldBackAnother) {
  const BoundReading latePower{Mirror::Site, ReadingKind::RealPower,
                               Phase::None, 0,
                               point("late-power", "power_real", "W", "watts")};
  reader->add(604, "/S1/late-power",
              {seconds(windowStartsAt + 100), {{"watts", -1000}}});
  build({siteVolts, latePower});
  runAt(windowStartsAt + 10);
  runAt(windowStartsAt + 300 + readingSettle.count());

  const auto posts = rest->requestsTo("POST /mup/1");
  ASSERT_EQ(posts.size(), 1u);
  EXPECT_TRUE(contains(posts[0].body, readingMrid(ourLfdi, siteVolts, pen)));
  EXPECT_FALSE(contains(posts[0].body, readingMrid(ourLfdi, latePower, pen)));
  ASSERT_EQ(deliveries("site_readings").size(), 1u);
  EXPECT_EQ(deliveries("site_readings")[0].payload.size(), 1u)
      << "one stream's readings";
}

TEST_F(CsipausReporterTest, AWindowWhoseDatumsNeverArriveIsRecordedOnItsLastChance) {
  const BoundReading latePower{Mirror::Site, ReadingKind::RealPower,
                               Phase::None, 0,
                               point("late-power", "power_real", "W", "watts")};
  reader->add(604, "/S1/late-power",
              {seconds(windowStartsAt + 100), {{"watts", -1000}}});
  build({latePower});
  runAt(windowStartsAt + 10);
  runAt(windowStartsAt + 300 + readingSettle.count());
  runAt(windowStartsAt + 300 * readingBacklog - 10);
  EXPECT_TRUE(deliveries("site_readings").empty());

  runAt(windowStartsAt + 300 * readingBacklog + 10);
  EXPECT_EQ(rest->count("POST /mup/1"), 0u)
      << "never from part of the window";
  const auto recorded = deliveries("site_readings");
  ASSERT_EQ(recorded.size(), 1u) << "the oldest window, and only it";
  EXPECT_FALSE(recorded[0].ok);
  EXPECT_EQ(recorded[0].intervalStart, seconds(windowStartsAt));
  EXPECT_TRUE(contains(recorded[0].message, "had not all arrived"))
      << recorded[0].message;
  EXPECT_EQ(recorded[0].payload[0]["quality"], "incomplete");
  EXPECT_TRUE(recorded[0].payload[0]["value"].is_null());
}
