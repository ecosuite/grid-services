#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <regex>
#include <set>

#include "core/event_sink.hpp"
#include "core/logging.hpp"
#include "protocols/csipaus/csipaus_adapter.hpp"
#include "support/cactus_fixture.hpp"
#include "support/fake_datum_reader.hpp"
#include "support/memory_store.hpp"
#include "support/scripted_rest.hpp"

using namespace grid_services;
using namespace grid_services::csipaus;
using namespace grid_services::testing;

namespace {

// test-servers/cactus/certs/device.pem, the certificate the runs use.
const std::string runLfdi = "09331B384E1ECB7C0E7939B4A15AF73481623851";
constexpr std::uint64_t runSfdi = 24695079729ull;

// Envoy's documents and answers from CACTUS runs, replayed to the adapter
// with the settings and certificate the runs gave the worker.
class CsipausCactusTest : public ::testing::Test {
protected:
  void load(const std::string &test, ReportingSetup reporting = {}) {
    ConnectionConfig config;
    config.protocol = "csipaus";
    connection = db->addConnection(config);
    store = std::make_unique<MemoryRunnerStore>(db, connection);
    sink = std::make_unique<StoreEventSink>(*store, Opt::In, *log);

    run = loadCactusFixture(test, *rest);
    wallNow = std::chrono::floor<std::chrono::seconds>(
        run.exchanges.front().requestedAt);
    reportingSetup = std::move(reporting);
    build();
  }

  void build() {
    CsipausSettings settings;
    settings.dcapUrl = "https://cactus-proxy/envoy/dcap";
    settings.registrationPin = 111115;
    settings.connectionPointId = "44070000003";
    adapter = std::make_unique<CsipausAdapter>(
        db->connections.at(connection), settings,
        DeviceIdentity{runLfdi, runSfdi}, std::make_unique<ScriptedRest>(rest),
        *store, [this] { return now; }, [this] { return wallNow; },
        reportingSetup);
  }

  // The worker restarted: a new adapter, store and sink on the same
  // database, as the runner makes them.
  void restart() {
    adapter.reset();
    store = std::make_unique<MemoryRunnerStore>(db, connection);
    sink = std::make_unique<StoreEventSink>(*store, Opt::In, *log);
    build();
  }

  // What site-sim provisions: a 5 kW PV inverter whose connected and
  // operating state are on /DER/INV, and the site's and DER's readings, with
  // run.py's PEN.
  ReportingSetup siteSimReporting() {
    DerDescription der;
    der.type = 4;
    der.rtgMaxW = 5000;
    der.rtgMaxVa = 5000;
    der.rtgMaxVar = 3000;
    der.rtgMaxVarNeg = 3000;
    der.rtgVNom = 230;
    der.modesSupported = {"export", "import", "generation", "load", "energize",
                          "connect"};
    der.modesEnabled = der.modesSupported;
    der.connectedPointId = "der-connected";
    der.operatingPointId = "der-operating";
    ReportingSetup reporting;
    reporting.der = der;
    reporting.state.connected =
        point("der-connected", "state", "none", "/DER/INV", "connected");
    reporting.state.operating =
        point("der-operating", "state", "none", "/DER/INV", "operating");
    // Power at the power of ten a reading written without one gets.
    reporting.readings = {
        {Mirror::Site, ReadingKind::RealPower, Phase::None, 1,
         point("site-real-power", "power_real", "W", "/SITE/METER", "watts")},
        {Mirror::Site, ReadingKind::ReactivePower, Phase::None, 1,
         point("site-reactive-power", "power_reactive", "var", "/SITE/METER",
               "reactivePower")},
        {Mirror::Site, ReadingKind::Voltage, Phase::None, -1,
         point("site-voltage", "voltage", "V", "/SITE/METER", "voltage")},
        {Mirror::Der, ReadingKind::RealPower, Phase::None, 1,
         point("der-real-power", "power_real", "W", "/DER/INV", "watts")},
        {Mirror::Der, ReadingKind::ReactivePower, Phase::None, 1,
         point("der-reactive-power", "power_reactive", "var", "/DER/INV",
               "reactivePower")}};
    reporting.reader = reader;
    reporting.pen = 12345;
    return reporting;
  }

  static TelemetryPoint point(const std::string &pointId,
                              const std::string &kind, const std::string &units,
                              const std::string &sourceId,
                              const std::string &property) {
    TelemetryPoint telemetry;
    telemetry.pointId = pointId;
    telemetry.item.kind = kind;
    telemetry.item.units = units;
    telemetry.source.nodeId = 1;
    telemetry.source.sourceId = sourceId;
    telemetry.source.property = property;
    return telemetry;
  }

  // The inverter's and the site meter's latest datums, 5 s ago.
  void inverterSays(double isConnected, double isOperating) {
    const auto producing = isConnected == 1 && isOperating == 1;
    reader->add(1, "/DER/INV",
                {wallNow - std::chrono::seconds(5),
                 {{"watts", producing ? 3000 : 0},
                  {"reactivePower", 0},
                  {"connected", isConnected},
                  {"operating", isOperating}}});
    reader->add(1, "/SITE/METER",
                {wallNow - std::chrono::seconds(5),
                 {{"watts", producing ? -2000 : 1000},
                  {"reactivePower", 0},
                  {"voltage", 230}}});
  }

  // Registers, polls and reports, as the worker does when it is asked to
  // register.
  void start() {
    adapter->registerConnection(*sink);
    adapter->poll(*sink);
    adapter->runDueWork(*sink);
  }

  // The operator's act shows in the inverter's next datum, and the worker
  // reads it on its next due-work run, when the run sent its next DERStatus.
  void inverterChangesTo(double isConnected, double isOperating) {
    const auto sent = statusesSent().size();
    std::size_t seen = 0;
    const auto next = std::find_if(
        run.exchanges.begin(), run.exchanges.end(), [&](const auto &exchange) {
          return exchange.key == "PUT /envoy/edev/1/der/1/ders" &&
                 seen++ == sent;
        });
    ASSERT_NE(next, run.exchanges.end()) << "the run sent no more statuses";
    const auto at = std::chrono::floor<std::chrono::seconds>(next->requestedAt);
    now += at - wallNow;
    wallNow = at;
    inverterSays(isConnected, isOperating);
    adapter->runDueWork(*sink);
  }

  // Each DERStatus sent, without the whitespace between its elements.
  std::vector<std::string> statusesSent() const {
    std::vector<std::string> bodies;
    for (const auto &put : rest->requestsTo("PUT /envoy/edev/1/der/1/ders")) {
      bodies.push_back(
          std::regex_replace(put.body, std::regex(">\\s+<"), "><"));
    }
    return bodies;
  }

  std::string statusSent() const {
    const auto bodies = statusesSent();
    EXPECT_EQ(bodies.size(), 1u);
    return bodies.empty() ? "" : bodies[0];
  }

  // The run's requests up to and including its first DERStatus.
  std::vector<std::string> keysToFirstStatus() const {
    auto keys = run.keys();
    const auto first =
        std::find(keys.begin(), keys.end(), "PUT /envoy/edev/1/der/1/ders");
    EXPECT_NE(first, keys.end());
    keys.erase(first == keys.end() ? first : first + 1, keys.end());
    return keys;
  }

  std::vector<std::string> requestKeys() const {
    std::vector<std::string> keys;
    for (const auto &request : rest->requests) {
      keys.push_back(request.key);
    }
    return keys;
  }

  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  ConnectionId connection = 0;
  std::unique_ptr<MemoryRunnerStore> store;
  std::shared_ptr<spdlog::logger> log = makeLogger("test");
  std::unique_ptr<StoreEventSink> sink;
  std::shared_ptr<RestScript> rest = std::make_shared<RestScript>();
  std::shared_ptr<FakeDatumReader> reader = std::make_shared<FakeDatumReader>();
  CactusFixture run;
  ReportingSetup reportingSetup;
  Clock::time_point now = Clock::time_point(std::chrono::hours(1000));
  Timestamp wallNow;
  std::unique_ptr<CsipausAdapter> adapter;
};

class CsipausCactusAll01 : public CsipausCactusTest {
protected:
  void SetUp() override { load("ALL-01"); }
};

class CsipausCactusAll02 : public CsipausCactusTest {
protected:
  void SetUp() override { load("ALL-02", siteSimReporting()); }
};

class CsipausCactusAll05 : public CsipausCactusTest {
protected:
  void SetUp() override { load("ALL-05", siteSimReporting()); }
};

class CsipausCactusAll06 : public CsipausCactusTest {
protected:
  void SetUp() override { load("ALL-06", siteSimReporting()); }
};

class CsipausCactusAll07 : public CsipausCactusTest {
protected:
  void SetUp() override { load("ALL-07", siteSimReporting()); }
};

class CsipausCactusAll08 : public CsipausCactusTest {
protected:
  void SetUp() override { load("ALL-08", siteSimReporting()); }
};

class CsipausCactusAll09 : public CsipausCactusTest {
protected:
  void SetUp() override { load("ALL-09", siteSimReporting()); }
};

// Envoy's controls and answers from a run, replayed at the run's times,
// with site-sim's planner played by the started rule. Reporting is left out,
// as its requests have keys of their own, but for the DER's description when
// it is asked for: then due work is also done when the run read the DERList
// or sent DERCapability or DERSettings, and the DERList is there to read
// after the run's last.
class CsipausCactusReplay : public CsipausCactusTest {
protected:
  void loadReplay(const std::string &test, bool describesDer = false) {
    load(test, describesDer ? describeOnly() : ReportingSetup{});
    if (describesDer) {
      dueWorkKeys = {derListPath, capabilityPath, settingsPath};
      for (const auto &exchange : run.exchanges) {
        if (exchange.key == derListPath) {
          rest->on(derListPath, exchange.body);
        }
      }
    }
    origin = now;
  }

  // site-sim's DER, without its state or readings.
  ReportingSetup describeOnly() {
    auto reporting = siteSimReporting();
    reporting.der->updatedAt = describedAt;
    reporting.der->connectedPointId.reset();
    reporting.der->operatingPointId.reset();
    reporting.state = {};
    reporting.readings.clear();
    reporting.reader.reset();
    return reporting;
  }

  // Each DERSettings sent, without the whitespace between its elements.
  std::vector<std::string> settingsSent() const {
    std::vector<std::string> bodies;
    for (const auto &put : rest->requestsTo(settingsPath)) {
      bodies.push_back(std::regex_replace(put.body, std::regex(">\\s+<"), "><"));
    }
    return bodies;
  }

  // The value of `element` in each DERSettings sent.
  std::vector<long long> settingsSentWith(const std::string &element) const {
    const std::regex value("<" + element + ">(\\d+)</" + element + ">");
    std::vector<long long> values;
    for (const auto &body : settingsSent()) {
      std::smatch match;
      EXPECT_TRUE(std::regex_search(body, match, value)) << body;
      values.push_back(match.empty() ? -1 : std::stoll(match[1].str()));
    }
    return values;
  }

  // For each DERSettings the run sent, when the last walk from the
  // EndDeviceList before it was done, by the server's clock.
  std::vector<long long> walksBeforeSettings() const {
    const auto offset = *db->stateExtras(connection).clockOffsetSeconds;
    std::vector<long long> walks;
    long long walk = 0;
    for (std::size_t i = 0; i < run.exchanges.size(); ++i) {
      if (run.exchanges[i].key == "GET /envoy/edev") {
        walk = std::chrono::floor<std::chrono::seconds>(
                   run.exchanges[walkEnd(i)].requestedAt)
                   .time_since_epoch()
                   .count() +
               offset;
      } else if (run.exchanges[i].key == settingsPath) {
        walks.push_back(walk);
      }
    }
    return walks;
  }

  // Those of `keys` among `wanted`, in order.
  static std::vector<std::string> only(const std::vector<std::string> &keys,
                                       const std::set<std::string> &wanted) {
    std::vector<std::string> kept;
    std::copy_if(keys.begin(), keys.end(), std::back_inserter(kept),
                 [&wanted](const auto &key) { return wanted.count(key) != 0; });
    return kept;
  }

  struct Replayed {
    // When the run sent each answer, by our clock.
    std::vector<Timestamp> runAnswers;
    // The answers sent on due work between polls.
    std::size_t betweenPolls = 0;
    // The due-work runs after which the adapter wanted a poll.
    std::size_t pollsWanted = 0;
    // The polls and resumes that failed, where the run's did.
    std::size_t failedAttempts = 0;
    // The due-work runs the server answered with errors, as in the run.
    std::size_t failedDueWork = 0;
    // The requests of the first poll that succeeded after one failed.
    std::vector<std::string> firstPollRestored;
  };

  // Polls when the run polled (a walk starts at the EndDeviceList, or at
  // Time when only Time is due, or fails at DeviceCapability), and does due
  // work then and when the run sent an answer; the planner moves each
  // control by its window just before. From exchange `restartAt` the worker
  // has restarted: it resumes when the run fetched its EndDevice, until
  // that succeeds. What failed in the run fails here, transiently, and a
  // failed poll leaves the caps stored as they were.
  Replayed replay(std::optional<std::size_t> restartAt = std::nullopt) {
    moveTo(run.exchanges.front().requestedAt);
    const auto info = adapter->registerConnection(*sink);
    store->setRegistered(info);
    const auto keys = run.keys();
    const auto firstPoll = static_cast<std::size_t>(
        std::find(keys.begin(), keys.end(), "GET /envoy/tm") - keys.begin());
    EXPECT_LT(firstPoll, keys.size());

    Replayed replayed;
    bool resuming = false;
    for (auto i = firstPoll; i < run.exchanges.size(); ++i) {
      const auto &exchange = run.exchanges[i];
      if (restartAt && i == *restartAt) {
        restart();
        resuming = true;
      }
      const bool resumes =
          resuming && exchange.key == "GET " + info.registrationId;
      const bool polls =
          !resuming &&
          (exchange.key == "GET /envoy/edev" || exchange.key == "GET /envoy/tm" ||
           (exchange.key == "GET /envoy/dcap" && exchange.status >= 500));
      if (exchange.key == responsePath) {
        replayed.runAnswers.push_back(
            std::chrono::floor<std::chrono::seconds>(exchange.requestedAt));
      } else if (!polls && !resumes && dueWorkKeys.count(exchange.key) == 0) {
        continue;
      }
      const auto end = polls ? walkEnd(i) : i;
      moveTo(run.exchanges[end].requestedAt);
      planAsTheSimulatorDoes();
      if (resumes) {
        try {
          adapter->resume(store->loadState(), *sink);
          resuming = false;
        } catch (const AdapterError &error) {
          EXPECT_EQ(error.kind(), AdapterError::Kind::Transient);
          EXPECT_TRUE(runFailed(i, end)) << error.what();
          ++replayed.failedAttempts;
        }
      }
      if (polls) {
        const auto before = limitsNow();
        const auto sent = rest->requests.size();
        try {
          adapter->poll(*sink);
          if (replayed.failedAttempts > 0 && replayed.firstPollRestored.empty()) {
            const auto requests = requestKeys();
            replayed.firstPollRestored.assign(
                requests.begin() + static_cast<std::ptrdiff_t>(sent),
                requests.end());
          }
        } catch (const AdapterError &error) {
          EXPECT_EQ(error.kind(), AdapterError::Kind::Transient);
          EXPECT_TRUE(runFailed(i, end)) << error.what();
          EXPECT_EQ(limitsNow(), before) << "a failed poll leaves the caps";
          ++replayed.failedAttempts;
        }
        keepLimits();
      }
      if (playsPlant) {
        playPlant();
      }
      const auto before = rest->count(responsePath);
      try {
        adapter->runDueWork(*sink);
      } catch (const AdapterError &error) {
        EXPECT_EQ(error.kind(), AdapterError::Kind::Transient);
        EXPECT_TRUE(runFailed(i, i)) << error.what();
        ++replayed.failedDueWork;
      }
      if (!polls) {
        replayed.betweenPolls += rest->count(responsePath) - before;
      }
      replayed.pollsWanted += adapter->pollWanted() ? 1 : 0;
    }
    return replayed;
  }

  // Whether the server answered any of the run's exchanges `from` to `to`
  // with a server error, as CACTUS does while comms are disabled.
  bool runFailed(std::size_t from, std::size_t to) const {
    return std::any_of(
        run.exchanges.begin() + static_cast<std::ptrdiff_t>(from),
        run.exchanges.begin() + static_cast<std::ptrdiff_t>(to) + 1,
        [](const auto &exchange) { return exchange.status >= 500; });
  }

  std::vector<LimitSegment> limitsNow() const {
    const auto stored = db->csipausLimits.find(connection);
    return stored == db->csipausLimits.end() ? std::vector<LimitSegment>{}
                                             : stored->second;
  }

  // The last GET of the walk that starts at exchange `start`: the worker
  // answers what it found once it has walked, so a walk that runs into the
  // next second answers in it.
  std::size_t walkEnd(std::size_t start) const {
    auto end = start;
    while (end + 1 < run.exchanges.size() &&
           run.exchanges[end + 1].key.rfind("GET ", 0) == 0 &&
           run.exchanges[end + 1].key != derListPath &&
           run.exchanges[end + 1].key != "GET /envoy/mup") {
      ++end;
    }
    return end;
  }

  // Adds the limits stored now to every one stored in the replay.
  void keepLimits() {
    const auto stored = db->csipausLimits.find(connection);
    if (stored == db->csipausLimits.end()) {
      return;
    }
    for (const auto &segment : stored->second) {
      if (std::find(limitsStored.begin(), limitsStored.end(), segment) ==
          limitsStored.end()) {
        limitsStored.push_back(segment);
      }
    }
  }

  // Both clocks at a moment of the run.
  void moveTo(std::chrono::sys_time<std::chrono::milliseconds> at) {
    now = origin + (at - run.exchanges.front().requestedAt);
    wallNow = std::chrono::floor<std::chrono::seconds>(at);
  }

  // The planner: each control to control while its effective window holds,
  // and to completed once it has ended, by the server's clock. Nothing until
  // the worker has read the server's clock.
  void planAsTheSimulatorDoes() {
    const auto offset = db->stateExtras(connection).clockOffsetSeconds;
    if (!offset) {
      return;
    }
    const auto at = wallNow + std::chrono::seconds(*offset);
    for (const auto &[id, detail] : db->csipausEvents) {
      if (detail.kind != CsipausEventKind::Control) {
        continue;
      }
      ASSERT_TRUE(detail.effectiveStartAt && detail.effectiveEndAt);
      const auto lifecycle = *detail.effectiveEndAt <= at ? "completed"
                             : *detail.effectiveStartAt <= at ? "control"
                                                              : "received";
      db->setLifecycle(id, lifecycle);
    }
  }

  // site-sim's plant, for a replay whose run switched the DER off: a
  // connect or energize cap of 0 stored and in force by the server's clock
  // turns it off, and it returns 60 s after the cap is lifted; the
  // inverter's datum, 5 s old, says so.
  void playPlant() {
    const auto offset = db->stateExtras(connection).clockOffsetSeconds;
    const auto stored = db->csipausLimits.find(connection);
    const auto isOff = [&](const std::string &name) {
      if (!offset || stored == db->csipausLimits.end()) {
        return false;
      }
      const auto at = wallNow + std::chrono::seconds(*offset);
      return std::any_of(
          stored->second.begin(), stored->second.end(), [&](const auto &row) {
            return row.limit == name && row.value == 0 && row.start <= at &&
                   (!row.end || at < *row.end);
          });
    };
    const auto on = [&](const std::string &name) {
      auto &lifted = liftedAt[name];
      if (isOff(name)) {
        lifted = std::nullopt;
        switchedOffOnce.insert(name);
        return false;
      }
      if (switchedOffOnce.count(name) != 0 && !lifted) {
        lifted = wallNow;
      }
      return !lifted || wallNow >= *lifted + std::chrono::seconds(60);
    };
    const bool connected = on("connect");
    const bool operating = connected && on("energize");
    reader->add(1, "/DER/INV",
                {wallNow - std::chrono::seconds(5),
                 {{"connected", connected ? 1 : 0},
                  {"operating", operating ? 1 : 0}}});
  }

  // site-sim's DER and its state, without its readings.
  ReportingSetup describeAndState() {
    auto reporting = siteSimReporting();
    reporting.der->updatedAt = describedAt;
    reporting.readings.clear();
    return reporting;
  }

  void loadPlantReplay(const std::string &test) {
    load(test, describeAndState());
    dueWorkKeys = {derListPath, capabilityPath, settingsPath, statusPath};
    for (const auto &exchange : run.exchanges) {
      if (exchange.key == derListPath) {
        rest->on(derListPath, exchange.body);
      }
    }
    rest->on(statusPath, "", 204);
    playsPlant = true;
    origin = now;
  }

  // genConnectStatus bit `bit` of each DERStatus sent, with repeats left
  // out.
  std::vector<int> statusBitChanges(int bit) const {
    const std::regex value(
        "<genConnectStatus>[\\s\\S]*?<value>([0-9A-Fa-f]+)</value>");
    std::vector<int> changes;
    for (const auto &put : rest->requestsTo(statusPath)) {
      std::smatch match;
      EXPECT_TRUE(std::regex_search(put.body, match, value)) << put.body;
      const int set = match.empty()
                          ? -1
                          : (std::stoi(match[1].str(), nullptr, 16) >> bit) & 1;
      if (changes.empty() || changes.back() != set) {
        changes.push_back(set);
      }
    }
    return changes;
  }

  struct Answer {
    std::string subject;
    int status = 0;
    long long created = 0;
  };

  std::vector<Answer> answersSent() const {
    const std::regex fields(
        "<createdDateTime>(\\d+)</createdDateTime>[\\s\\S]*"
        "<status>(\\d+)</status>[\\s\\S]*<subject>([0-9A-F]+)</subject>");
    std::vector<Answer> answers;
    for (const auto &post : rest->requestsTo(responsePath)) {
      std::smatch match;
      EXPECT_TRUE(std::regex_search(post.body, match, fields)) << post.body;
      answers.push_back({match[3].str(), std::stoi(match[2].str()),
                         std::stoll(match[1].str())});
    }
    return answers;
  }

  // The statuses sent for each control, in the order the controls were
  // first answered; each answer is checked to have been sent when the run
  // sent it, and never in the second of the control's last.
  std::vector<std::vector<int>> statusesByControl(const Replayed &replayed) {
    const auto offset =
        std::chrono::seconds(*db->stateExtras(connection).clockOffsetSeconds);
    const auto answers = answersSent();
    EXPECT_EQ(answers.size(), replayed.runAnswers.size());
    std::vector<std::string> subjects;
    std::map<std::string, std::vector<Answer>> bySubject;
    for (std::size_t i = 0; i < answers.size(); ++i) {
      if (i < replayed.runAnswers.size()) {
        EXPECT_EQ(answers[i].created,
                  (replayed.runAnswers[i] + offset).time_since_epoch().count())
            << "answer " << i << " is sent when the run sent it";
      }
      auto &sent = bySubject[answers[i].subject];
      if (sent.empty()) {
        subjects.push_back(answers[i].subject);
      } else {
        EXPECT_GT(answers[i].created, sent.back().created)
            << answers[i].subject << ": two statuses in one second";
      }
      sent.push_back(answers[i]);
    }
    std::vector<std::vector<int>> statuses;
    for (const auto &subject : subjects) {
      std::vector<int> sent;
      for (const auto &answer : bySubject[subject]) {
        sent.push_back(answer.status);
      }
      statuses.push_back(std::move(sent));
    }
    return statuses;
  }

  const std::string responsePath = "POST /envoy/edev/1/rsps/site_control/rsp";
  const std::string derListPath = "GET /envoy/edev/1/der";
  const std::string capabilityPath = "PUT /envoy/edev/1/der/1/dercap";
  const std::string settingsPath = "PUT /envoy/edev/1/der/1/derg";
  const std::string defaultPath = "GET /envoy/edev/1/derp/1/dderc";
  const std::string statusPath = "PUT /envoy/edev/1/der/1/ders";
  // Requests the run made that due work is also done at.
  std::set<std::string> dueWorkKeys;
  // Every limit segment stored in the replay.
  std::vector<LimitSegment> limitsStored;
  // Whether due work reads the inverter's state as playPlant gives it.
  bool playsPlant = false;
  // For playPlant: the switches a cap has turned off, and when each was
  // lifted, while it has not returned.
  std::set<std::string> switchedOffOnce;
  std::map<std::string, std::optional<Timestamp>> liftedAt;
  // csipaus_der.updated_at: when site-sim described the DER, before the run.
  const Timestamp describedAt = Timestamp(std::chrono::seconds(1791700000));
  Clock::time_point origin;
};

class CsipausCactusGen11 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadReplay("GEN-11"); }
};

class CsipausCactusAll13 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadReplay("ALL-13", true); }
};

class CsipausCactusAll28 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadReplay("ALL-28", true); }
};

class CsipausCactusAll18 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadReplay("ALL-18"); }
};

class CsipausCactusAll11 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadPlantReplay("ALL-11"); }
};

class CsipausCactusAll12 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadPlantReplay("ALL-12"); }
};

class CsipausCactusAll26 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadPlantReplay("ALL-26"); }
};

class CsipausCactusAll27 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadPlantReplay("ALL-27"); }
};

class CsipausCactusGen10 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadPlantReplay("GEN-10"); }
};

class CsipausCactusAll23 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadReplay("ALL-23"); }

  const std::string controlsPath = "GET /envoy/edev/1/derp/1/derc";
};

class CsipausCactusAll30 : public CsipausCactusReplay {
protected:
  void SetUp() override { loadReplay("ALL-30"); }

  // The run's first request after the operator restarted the worker: its
  // resume, fetching the EndDevice while comms were down.
  std::size_t firstAfterRestart() const {
    const auto found = std::find_if(
        run.exchanges.begin(), run.exchanges.end(), [](const auto &exchange) {
          return exchange.key == "GET /envoy/edev/1" && exchange.status >= 500;
        });
    EXPECT_NE(found, run.exchanges.end());
    return static_cast<std::size_t>(found - run.exchanges.begin());
  }
};

} // namespace

TEST_F(CsipausCactusAll01, RegistersAsInTheRun) {
  const auto info = adapter->registerConnection(*sink);

  const std::vector<std::string> registration = {
      "GET /envoy/dcap",          "GET /envoy/edev",
      "GET /envoy/edev/1/rg",     "PUT /envoy/edev/1/cp",
      "GET /envoy/edev/1/fsa",    "GET /envoy/edev/1/fsa/1/derp"};
  const auto recorded = run.keys();
  ASSERT_GE(recorded.size(), registration.size());
  EXPECT_EQ(std::vector<std::string>(recorded.begin(),
                                     recorded.begin() + registration.size()),
            registration);
  EXPECT_EQ(requestKeys(), registration);

  const auto puts = rest->requestsTo("PUT /envoy/edev/1/cp");
  ASSERT_EQ(puts.size(), 1u);
  EXPECT_NE(puts[0].body.find("<csipaus:connectionPointId>44070000003"
                              "</csipaus:connectionPointId>"),
            std::string::npos);

  EXPECT_EQ(info.clientId, runLfdi);
  EXPECT_EQ(info.registrationId, "/envoy/edev/1");
  EXPECT_EQ(info.pollIntervalSeconds, 60) << "the DERProgramList's poll rate";
}

// Envoy's EndDevice has no RegistrationLink, so the PIN goes unchecked.
TEST_F(CsipausCactusAll05, RegistersAsInTheRun) {
  const auto info = adapter->registerConnection(*sink);

  const std::vector<std::string> registration = {
      "GET /envoy/dcap",      "GET /envoy/edev",
      "POST /envoy/edev",     "GET /envoy/edev/1",
      "PUT /envoy/edev/1/cp", "GET /envoy/edev/1/fsa",
      "GET /envoy/edev/1/fsa/1/derp"};
  const auto recorded = run.keys();
  ASSERT_GE(recorded.size(), registration.size());
  EXPECT_EQ(std::vector<std::string>(recorded.begin(),
                                     recorded.begin() + registration.size()),
            registration);
  EXPECT_EQ(requestKeys(), registration);
  EXPECT_EQ(rest->count("GET /envoy/edev/1/rg"), 0u);

  EXPECT_EQ(info.registrationId, "/envoy/edev/1");
  EXPECT_TRUE(db->activityEntry(connection,
                                "no Registration to check the PIN against"));
}

TEST_F(CsipausCactusAll05, ReportsTheDersStateOnceItFindsTheDer) {
  inverterSays(1, 1);
  start();

  EXPECT_EQ(requestKeys(), run.keys())
      << "the status follows the DER's description and its mirrors";
  const auto status = statusSent();
  EXPECT_NE(status.find("<value>07</value></genConnectStatus>"),
            std::string::npos)
      << status;
  EXPECT_NE(status.find("<value>2</value></operationalModeStatus>"),
            std::string::npos);
}

TEST_F(CsipausCactusAll02, RegistersInBandAndDescribesTheDer) {
  inverterSays(1, 1);
  start();

  EXPECT_EQ(requestKeys(), run.keys());
  const auto status = statusSent();
  EXPECT_NE(status.find("<value>07</value></genConnectStatus>"),
            std::string::npos)
      << status;
  EXPECT_NE(status.find("<value>2</value></operationalModeStatus>"),
            std::string::npos);
}

TEST_F(CsipausCactusAll09, SendsTheDersCapabilitySettingsAndStatus) {
  inverterSays(1, 0);
  start();

  EXPECT_EQ(requestKeys(), run.keys());
  const auto capability = rest->requestsTo("PUT /envoy/edev/1/der/1/dercap");
  ASSERT_EQ(capability.size(), 1u);
  EXPECT_NE(capability[0].body.find("<type>4</type>"), std::string::npos);
  const auto settings = rest->requestsTo("PUT /envoy/edev/1/der/1/derg");
  ASSERT_EQ(settings.size(), 1u);
  EXPECT_NE(settings[0].body.find("<setGradW>27</setGradW>"),
            std::string::npos);
  const auto status = statusSent();
  EXPECT_NE(status.find("<value>03</value></genConnectStatus>"),
            std::string::npos)
      << "connected and available, not operating: " << status;
  EXPECT_NE(status.find("<value>1</value></operationalModeStatus>"),
            std::string::npos);
}

// The run before step 2 created mirrors with the PEN in hex, power at a
// power of ten of 0 and the site's voltage at phase 0, and failed every
// readings check.
TEST_F(CsipausCactusAll06, CreatesItsMirrorsAsCsipAusAsks) {
  inverterSays(1, 1);
  start();

  const auto created = rest->requestsTo("POST /envoy/mup");
  ASSERT_EQ(created.size(), 2u);
  const auto flat = [](const std::string &body) {
    return std::regex_replace(body, std::regex(">\\s+<"), "><");
  };
  const std::regex mrid("<mRID>([0-9A-F]*)</mRID>");
  const std::vector<std::size_t> mrids = {4, 3};
  for (std::size_t i = 0; i < created.size(); ++i) {
    const auto body = flat(created[i].body);
    std::size_t count = 0;
    for (std::sregex_iterator it(body.begin(), body.end(), mrid), end;
         it != end; ++it, ++count) {
      const auto value = (*it)[1].str();
      ASSERT_EQ(value.size(), 32u) << value;
      EXPECT_EQ(value.substr(24), "00012345")
          << "run.py's PEN, in decimal: " << value;
    }
    EXPECT_EQ(count, mrids[i]) << "the mirror's and each reading's";
  }

  const auto site = flat(created[0].body);
  EXPECT_NE(site.find("<roleFlags>03</roleFlags>"), std::string::npos);
  EXPECT_NE(site.find("<phase>129</phase><powerOfTenMultiplier>-1"
                      "</powerOfTenMultiplier><uom>29</uom>"),
            std::string::npos)
      << "single-phase voltage is AN: " << site;
  EXPECT_NE(site.find("<phase>0</phase><powerOfTenMultiplier>1"
                      "</powerOfTenMultiplier><uom>38</uom>"),
            std::string::npos);
  EXPECT_NE(site.find("<phase>0</phase><powerOfTenMultiplier>1"
                      "</powerOfTenMultiplier><uom>63</uom>"),
            std::string::npos);
  const auto der = flat(created[1].body);
  EXPECT_NE(der.find("<roleFlags>49</roleFlags>"), std::string::npos);
  EXPECT_NE(der.find("<powerOfTenMultiplier>1</powerOfTenMultiplier>"
                     "<uom>38</uom>"),
            std::string::npos);
  EXPECT_NE(der.find("<powerOfTenMultiplier>1</powerOfTenMultiplier>"
                     "<uom>63</uom>"),
            std::string::npos);
  EXPECT_EQ(rest->count("GET /envoy/dcap"), 1u)
      << "polled at its own rate, and read once for the readings";
}

// The operator isolates the DER, then reconnects it.
TEST_F(CsipausCactusAll07, SendsTheDisconnectAndReconnect) {
  inverterSays(1, 1);
  start();
  EXPECT_EQ(requestKeys(), keysToFirstStatus());

  // Isolated, it stops operating too. With only a default in force,
  // the worker asks to poll its programs again (explainer 5.3.1).
  inverterChangesTo(0, 0);
  EXPECT_TRUE(adapter->pollWanted());
  inverterChangesTo(1, 1);

  EXPECT_EQ(requestKeys(), run.keys());
  const auto statuses = statusesSent();
  const std::vector<std::string> connectStatus = {"07", "00", "07"};
  const std::vector<std::string> mode = {"2", "1", "2"};
  ASSERT_EQ(statuses.size(), connectStatus.size());
  for (std::size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_NE(statuses[i].find("<value>" + connectStatus[i] +
                               "</value></genConnectStatus>"),
              std::string::npos)
        << statuses[i];
    EXPECT_NE(statuses[i].find("<value>" + mode[i] +
                               "</value></operationalModeStatus>"),
              std::string::npos)
        << statuses[i];
  }
}

// The operator stops the DER's power, then resumes it.
TEST_F(CsipausCactusAll08, SendsTheStopAndResume) {
  inverterSays(1, 1);
  start();
  EXPECT_EQ(requestKeys(), keysToFirstStatus());

  inverterChangesTo(1, 0);
  inverterChangesTo(1, 1);
  EXPECT_FALSE(adapter->pollWanted()) << "it stayed connected";

  EXPECT_EQ(requestKeys(), run.keys());
  const auto statuses = statusesSent();
  const std::vector<std::string> connectStatus = {"07", "03", "07"};
  const std::vector<std::string> mode = {"2", "1", "2"};
  ASSERT_EQ(statuses.size(), connectStatus.size());
  for (std::size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_NE(statuses[i].find("<value>" + connectStatus[i] +
                               "</value></genConnectStatus>"),
              std::string::npos)
        << statuses[i];
    EXPECT_NE(statuses[i].find("<value>" + mode[i] +
                               "</value></operationalModeStatus>"),
              std::string::npos)
        << statuses[i];
  }
}

TEST_F(CsipausCactusGen11, AnswersEachControlReceivedStartedCompleted) {
  const auto replayed = replay();

  const std::vector<int> each = {1, 2, 3};
  EXPECT_EQ(statusesByControl(replayed),
            (std::vector<std::vector<int>>{each, each, each, each, each}))
      << "DERC1 to DERC5";
  EXPECT_GT(replayed.betweenPolls, 0u)
      << "started and completed are sent between polls";
}

// Envoy marks DERC4 superseded when the fifth control, newer and in its
// program, overlaps it; the worker's own resolution supersedes nothing,
// since DERC4 holds its caps for most of its window.
TEST_F(CsipausCactusAll18, AnswersEachControlAsTheRunDid) {
  const auto replayed = replay();

  EXPECT_EQ(statusesByControl(replayed),
            (std::vector<std::vector<int>>{
                {1, 2, 3}, {1, 2, 6}, {1, 6}, {1, 7}, {1, 2, 3}}))
      << "DERC1 to DERC4, and the fifth control";
  EXPECT_FALSE(db->activityEntry(connection, "This device superseded"));
}

// Envoy sets the default's setGradW to 100, then 27, as the worker reads it;
// each change goes in a DERSettings of its own, after the walk that saw it.
TEST_F(CsipausCactusAll13, SendsEachSetGradWTheDefaultSets) {
  replay();

  const std::set<std::string> settings = {defaultPath, capabilityPath,
                                          settingsPath};
  EXPECT_EQ(only(requestKeys(), settings), only(run.keys(), settings));
  EXPECT_EQ(settingsSentWith("setGradW"), (std::vector<long long>{27, 100, 27}))
      << "csipaus_der's while the program has no default, then the default's";
  const auto walks = walksBeforeSettings();
  ASSERT_EQ(walks.size(), 3u);
  EXPECT_EQ(settingsSentWith("updatedTime"),
            (std::vector<long long>{describedAt.time_since_epoch().count(),
                                    walks[1], walks[2]}))
      << "csipaus_der.updated_at, then the walk that saw each change";
  EXPECT_EQ(rest->count(capabilityPath), 1u);
}

// DERC3 ramps over its own rampTms. Once the default sets setGradW 100, it
// goes in DERSettings and on DERC4's rows, which have no rampTms; before,
// controls without rampTms have no ramp.
TEST_F(CsipausCactusAll28, CarriesEachRowsRampAndTheDefaultsSetGradW) {
  const auto replayed = replay();

  EXPECT_EQ(statusesByControl(replayed),
            (std::vector<std::vector<int>>{
                {1, 2, 7}, {1, 2, 7}, {1, 2, 7}, {1, 2, 6}, {1, 2, 3}}))
      << "PRECONDITION and DERC1 to DERC4";

  std::map<std::string, std::set<std::pair<std::optional<int>, std::optional<int>>>>
      rampsByControl;
  std::vector<std::string> controls;
  for (const auto &segment : limitsStored) {
    if (segment.source != CsipausEventKind::Control) {
      continue;
    }
    if (rampsByControl.count(segment.programEventId) == 0) {
      controls.push_back(segment.programEventId);
    }
    rampsByControl[segment.programEventId].insert(
        {segment.rampTms, segment.setGradW});
  }
  using Ramps = std::set<std::pair<std::optional<int>, std::optional<int>>>;
  const Ramps none = {{std::nullopt, std::nullopt}};
  ASSERT_EQ(controls.size(), 5u);
  EXPECT_EQ(rampsByControl[controls[0]], none) << "PRECONDITION";
  EXPECT_EQ(rampsByControl[controls[1]], none) << "DERC1";
  EXPECT_EQ(rampsByControl[controls[2]], none) << "DERC2";
  EXPECT_EQ(rampsByControl[controls[3]], (Ramps{{6000, std::nullopt}}))
      << "DERC3: 60 s";
  EXPECT_EQ(rampsByControl[controls[4]], (Ramps{{std::nullopt, 100}}))
      << "DERC4: the default's setGradW";

  const std::set<std::string> settings = {defaultPath, capabilityPath,
                                          settingsPath};
  EXPECT_EQ(only(requestKeys(), settings), only(run.keys(), settings));
  EXPECT_EQ(settingsSentWith("setGradW"), (std::vector<long long>{27, 100}));
  const auto walks = walksBeforeSettings();
  ASSERT_EQ(walks.size(), 2u);
  EXPECT_EQ(settingsSentWith("updatedTime"),
            (std::vector<long long>{describedAt.time_since_epoch().count(),
                                    walks[1]}));
}

// The second control's energize 0 stops the DER, which stays connected;
// once it ends the DER operates again 60 s on.
TEST_F(CsipausCactusAll11, SendsTheStopAndRestartItsCapsCommand) {
  const auto replayed = replay();

  EXPECT_EQ(statusBitChanges(2), (std::vector<int>{1, 0, 1}));
  EXPECT_EQ(statusBitChanges(0), (std::vector<int>{1}));
  EXPECT_EQ(replayed.pollsWanted, 0u);
}

// The control's connect 0 disconnects the DER for 10 minutes; the worker
// keeps its controls and asks for no poll, and the DER returns 60 s after
// the control ends.
TEST_F(CsipausCactusAll12, KeepsItsControlsThroughACommandedDisconnection) {
  const auto replayed = replay();

  EXPECT_EQ(statusBitChanges(0), (std::vector<int>{1, 0, 1}));
  EXPECT_EQ(replayed.pollsWanted, 0u);
  EXPECT_TRUE(db->activityEntry(
      connection, "while a connect or energize cap of 0 is in force"));
  EXPECT_FALSE(db->activityEntry(connection, "disconnected from the grid"));
}

// DEENERGIZE's energize 0 stops the DER, which stays connected, so nothing
// is dropped; ENERGIZE lifts it, and the DER operates again 60 s on.
TEST_F(CsipausCactusAll26, SendsTheStopAndRestartItsCapsCommand) {
  const auto replayed = replay();

  EXPECT_EQ(statusBitChanges(2), (std::vector<int>{1, 0, 1}));
  EXPECT_EQ(statusBitChanges(0), (std::vector<int>{1}));
  EXPECT_EQ(replayed.pollsWanted, 0u);
  EXPECT_FALSE(db->activityEntry(connection, "The DER disconnected"));
}

// DISCONNECT's connect 0 disconnects the DER; the worker keeps its controls
// and asks for no poll, since a connect cap of 0 is in force. CONNECT lifts
// it, and the DER returns 60 s on.
TEST_F(CsipausCactusAll27, KeepsItsControlsThroughACommandedDisconnection) {
  const auto replayed = replay();

  EXPECT_EQ(statusBitChanges(0), (std::vector<int>{1, 0, 1}));
  EXPECT_EQ(replayed.pollsWanted, 0u);
  EXPECT_TRUE(db->activityEntry(
      connection, "while a connect or energize cap of 0 is in force"));
  EXPECT_FALSE(db->activityEntry(connection, "disconnected from the grid"));
}

// Fifteen programs in two FSAs. DERC2, in a primacy 1 program, holds export
// 0 past DERC1's end in a primacy 2 program, so the worker sends DERC1's 7
// itself; envoy marks DERC5's when DERC6 joins its program. DERC4's connect
// 0 disconnects the DER, and the worker keeps its controls. DERC9 and
// DERC10 are cancelled.
TEST_F(CsipausCactusGen10, AnswersEachControlAsTheRunDid) {
  const auto replayed = replay();

  const std::vector<int> completed = {1, 2, 3};
  const std::vector<int> superseded = {1, 2, 7};
  const std::vector<int> cancelled = {1, 2, 6};
  EXPECT_EQ(statusesByControl(replayed),
            (std::vector<std::vector<int>>{superseded, completed, completed,
                                           completed, superseded, superseded,
                                           completed, completed, cancelled,
                                           cancelled}))
      << "DERC1 to DERC10";
  EXPECT_TRUE(db->activityEntry(connection, "This device superseded"));
  EXPECT_EQ(statusBitChanges(0), (std::vector<int>{1, 0, 1}));
  EXPECT_EQ(replayed.pollsWanted, 0u);
  EXPECT_TRUE(db->activityEntry(
      connection, "while a connect or energize cap of 0 is in force"));
}

// Comms are down for 420 s. Polls fail and change no caps, so the stored
// controls and then the default run on; once comms return, the next walk
// fetches the controls (GET-DERC-AFTER-COMMS-RESTORE).
TEST_F(CsipausCactusAll23, KeepsItsCapsThroughTheLossAndPollsTheControlsWhenItReturns) {
  const auto replayed = replay();

  EXPECT_GT(replayed.failedAttempts, 2u);
  EXPECT_GT(replayed.failedDueWork, 0u);
  const std::set<std::string> walk = {
      "GET /envoy/dcap", "GET /envoy/edev", "GET /envoy/edev/1",
      "GET /envoy/tm", "GET /envoy/edev/1/fsa", "GET /envoy/edev/1/fsa/1/derp",
      defaultPath, controlsPath};
  EXPECT_EQ(only(requestKeys(), walk), only(run.keys(), walk));
  EXPECT_EQ(std::count(replayed.firstPollRestored.begin(),
                       replayed.firstPollRestored.end(), controlsPath),
            1)
      << "the first walk after comms return fetches the controls";
}

// The operator restarts the worker while comms are down. The restarted
// worker resumes once they return, from what the database holds: DERC1 is
// received then started, never two in one second, and PRECONDITION, told
// received and started before the restart, is not told received again.
TEST_F(CsipausCactusAll30, AnswersStartedAfterARestart) {
  const auto replayed = replay(firstAfterRestart());

  EXPECT_EQ(replayed.failedAttempts, 5u)
      << "the poll the outage cut short, then four resumes";
  EXPECT_EQ(statusesByControl(replayed),
            (std::vector<std::vector<int>>{{1, 2, 3}, {1, 2, 3}}))
      << "PRECONDITION and DERC1";
}
