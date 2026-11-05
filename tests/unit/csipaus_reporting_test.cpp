#include <gtest/gtest.h>

#include <regex>
#include <sstream>

#include "protocols/csipaus/reporting.hpp"
#include "support/csipaus_fixtures.hpp"

using namespace grid_services;
using namespace grid_services::csipaus;
namespace fixtures = grid_services::testing::csipaus_fixtures;

namespace {

const std::string lfdi = fixtures::deviceLfdi;
const Timestamp at = Timestamp(std::chrono::seconds(1790000100));

TelemetryPoint point(const std::string &pointId, const std::string &kind,
                     const std::string &units) {
  TelemetryPoint telemetry;
  telemetry.pointId = pointId;
  telemetry.item.kind = kind;
  telemetry.item.units = units;
  telemetry.source.nodeId = 604;
  telemetry.source.sourceId = "/S1/" + pointId;
  telemetry.source.property = "watts";
  return telemetry;
}

BoundReading reading(Mirror mirror, ReadingKind kind, Phase phase,
                     int powerOfTen, const TelemetryPoint &telemetry) {
  return {mirror, kind, phase, powerOfTen, telemetry};
}

// The document without the whitespace between its elements.
template <typename T> std::string xml(const T &document) {
  std::ostringstream out;
  Document<T>::write(out, document);
  return std::regex_replace(out.str(), std::regex(">\\s+<"), "><");
}

bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

DerDescription der() {
  DerDescription description;
  description.type = 4;
  description.rtgMaxW = 50000;
  description.rtgMaxVa = 52000;
  description.rtgVNom = 230;
  description.setMaxW = 30000;
  description.modesSupported = {"export", "import", "max_output", "connect"};
  description.modesEnabled = {"export", "max_output"};
  description.updatedAt = at;
  return description;
}

} // namespace

TEST(CsipausReportingTest, BindsReadingsToPointsOfTheirKind) {
  const std::vector<TelemetryPoint> points = {
      point("site-power", "power_real", "W"), point("site-volts", "voltage", "V")};
  const auto bound = bindReadings(
      {{Mirror::Site, ReadingKind::RealPower, Phase::None, "site-power", 0},
       {Mirror::Site, ReadingKind::Voltage, Phase::A, "site-volts"}},
      points);

  ASSERT_EQ(bound.size(), 2u);
  EXPECT_EQ(bound[1].point.pointId, "site-volts");
  EXPECT_EQ(readingId(bound[1]), "voltage_a");
  EXPECT_EQ(describeReading(bound[1]), "Site voltage phase A");
}

TEST(CsipausReportingTest, RefusesReadingsWithoutAPointOfTheirKind) {
  const std::vector<TelemetryPoint> points = {
      point("site-power", "power_real", "W"),
      point("site-kw", "power_reactive", "kvar")};

  EXPECT_THROW(bindReadings({{Mirror::Site, ReadingKind::RealPower,
                              Phase::None, "missing", 0}},
                            points),
               std::invalid_argument);
  EXPECT_THROW(bindReadings({{Mirror::Der, ReadingKind::Voltage, Phase::None,
                              "site-power"}},
                            points),
               std::invalid_argument);
  auto kilovars = points[1];
  kilovars.item.units = "var";
  kilovars.item.scale = "k";
  EXPECT_THROW(bindReadings({{Mirror::Site, ReadingKind::ReactivePower,
                              Phase::None, "site-kw", 0}},
                            {kilovars}),
               std::invalid_argument)
      << "values are sent in var, so the point must be too";
}

// The CSIP-AUS v1.2 explainer's section 4.1.7: the PEN in decimal, as
// CACTUS reads it (int(mrid[-8:])).
TEST(CsipausReportingTest, MridsEndInThePenInDecimal) {
  const auto site = mirrorMrid(lfdi, Mirror::Site, 12345);

  EXPECT_EQ(site.size(), 32u);
  EXPECT_EQ(site.substr(24), "00012345");
  EXPECT_EQ(site.find_first_not_of("0123456789ABCDEF"), std::string::npos);
  EXPECT_EQ(site, mirrorMrid(lfdi, Mirror::Site, 12345));
  EXPECT_NE(site, mirrorMrid(lfdi, Mirror::Der, 12345));
  EXPECT_EQ(mirrorMrid(lfdi, Mirror::Site, maxPen).substr(24), "99999999");

  const auto power = point("p", "power_real", "W");
  const auto sitePower = readingMrid(
      lfdi, reading(Mirror::Site, ReadingKind::RealPower, Phase::None, 1, power),
      12345);
  EXPECT_EQ(sitePower.size(), 32u);
  EXPECT_EQ(sitePower.substr(24), "00012345");
  EXPECT_NE(sitePower,
            readingMrid(lfdi,
                        reading(Mirror::Der, ReadingKind::RealPower,
                                Phase::None, 1, power),
                        12345));
}

TEST(CsipausReportingTest, PowerReadingsAreSentAtTheirRowsPowerOfTen) {
  const auto bound = bindReadings(
      {{Mirror::Site, ReadingKind::RealPower, Phase::None, "p", 1},
       {Mirror::Site, ReadingKind::Voltage, Phase::None, "v"},
       {Mirror::Der, ReadingKind::Frequency, Phase::None, "f"}},
      {point("p", "power_real", "W"), point("v", "voltage", "V"),
       point("f", "frequency", "Hz")});
  ASSERT_EQ(bound.size(), 3u);
  EXPECT_EQ(bound[0].powerOfTen, 1);
  EXPECT_EQ(bound[1].powerOfTen, -1);
  EXPECT_EQ(bound[2].powerOfTen, -2);

  EXPECT_EQ(sentValue(1, -1200.4), -120);
  EXPECT_DOUBLE_EQ(receivedValue(1, -120), -1200);
  EXPECT_EQ(sentValue(0, -1499.6), -1500);
  EXPECT_EQ(sentValue(-1, 230.14), 2301);
  EXPECT_DOUBLE_EQ(receivedValue(-1, 2301), 230.1);
  EXPECT_EQ(sentValue(-2, 50.012), 5001);
}

// Readings are Int16 with a multiplier (CSIP-AUS v1.2 explainer, section
// 4.2.4).
TEST(CsipausReportingTest, AValueOutsideInt16AtItsPowerOfTenIsNotSent) {
  EXPECT_FALSE(sentValue(0, 40000).has_value());
  EXPECT_EQ(sentValue(1, 40000), 4000);
  EXPECT_EQ(sentValue(0, 32767), 32767);
  EXPECT_EQ(sentValue(0, -32768), -32768);
  EXPECT_FALSE(sentValue(0, 32767.6).has_value());
  EXPECT_FALSE(sentValue(0, -32769).has_value());
}

TEST(CsipausReportingTest,
     RefusesAPowerReadingWithoutAPowerOfTenAndAVoltageWithOne) {
  const std::vector<TelemetryPoint> points = {
      point("p", "power_real", "W"), point("q", "power_reactive", "var"),
      point("v", "voltage", "V"), point("f", "frequency", "Hz")};

  EXPECT_THROW(
      bindReadings({{Mirror::Site, ReadingKind::RealPower, Phase::None, "p"}},
                   points),
      std::invalid_argument)
      << "the worker does not choose one";
  EXPECT_THROW(bindReadings({{Mirror::Site, ReadingKind::ReactivePower,
                              Phase::None, "q"}},
                            points),
               std::invalid_argument);
  EXPECT_THROW(bindReadings({{Mirror::Site, ReadingKind::Voltage, Phase::None,
                              "v", -1}},
                            points),
               std::invalid_argument)
      << "voltage's is fixed";
  EXPECT_THROW(bindReadings({{Mirror::Der, ReadingKind::Frequency, Phase::None,
                              "f", -2}},
                            points),
               std::invalid_argument);
  EXPECT_THROW(bindReadings({{Mirror::Site, ReadingKind::RealPower,
                              Phase::None, "p", 10}},
                            points),
               std::invalid_argument)
      << "IEEE 2030.5's PowerOfTenMultiplierType is -9 to 9";
  EXPECT_THROW(bindReadings({{Mirror::Site, ReadingKind::RealPower,
                              Phase::None, "p", -10}},
                            points),
               std::invalid_argument);
  EXPECT_EQ(bindReadings({{Mirror::Site, ReadingKind::RealPower, Phase::None,
                           "p", 9}},
                         points)[0]
                .powerOfTen,
            9);
}

TEST(CsipausReportingTest, AlignsWindowsToThePostRate) {
  EXPECT_EQ(windowStart(Timestamp(std::chrono::seconds(1790000399)),
                        std::chrono::seconds(300)),
            at);
  EXPECT_EQ(windowStart(at, std::chrono::seconds(300)), at);
  EXPECT_EQ(windowStart(Timestamp(std::chrono::seconds(1790000159)),
                        std::chrono::seconds(60)),
            Timestamp(std::chrono::seconds(1790000100)));
}

TEST(CsipausReportingTest, MirrorUsagePointsDescribeTheirReadings) {
  const auto document = xml(mirrorUsagePoint(
      Mirror::Site,
      {reading(Mirror::Site, ReadingKind::RealPower, Phase::None, 1,
               point("p", "power_real", "W")),
       reading(Mirror::Site, ReadingKind::ReactivePower, Phase::None, 2,
               point("q", "power_reactive", "var")),
       reading(Mirror::Site, ReadingKind::Voltage, Phase::B, -1,
               point("vb", "voltage", "V")),
       reading(Mirror::Site, ReadingKind::Voltage, Phase::None, -1,
               point("v", "voltage", "V"))},
      lfdi, 7));

  EXPECT_TRUE(contains(document, "<mRID>" + mirrorMrid(lfdi, Mirror::Site, 7) +
                                     "</mRID>"));
  EXPECT_TRUE(contains(document, "<roleFlags>03</roleFlags>"));
  EXPECT_TRUE(contains(document, "<deviceLFDI>" + lfdi + "</deviceLFDI>"));
  EXPECT_TRUE(contains(document, "<phase>0</phase><powerOfTenMultiplier>1"
                                 "</powerOfTenMultiplier><uom>38</uom>"))
      << "W at the row's power of ten";
  EXPECT_TRUE(contains(document, "<phase>0</phase><powerOfTenMultiplier>2"
                                 "</powerOfTenMultiplier><uom>63</uom>"));
  EXPECT_TRUE(contains(document, "<phase>65</phase><powerOfTenMultiplier>-1"
                                 "</powerOfTenMultiplier><uom>29</uom>"))
      << "phase BN";
  EXPECT_TRUE(contains(document, "<phase>129</phase><powerOfTenMultiplier>-1"
                                 "</powerOfTenMultiplier><uom>29</uom>"))
      << "a single phase's voltage is AN (CSIP-AUS v1.1a, Annex A)";
  EXPECT_TRUE(contains(document, "<dataQualifier>2</dataQualifier>"));
  EXPECT_TRUE(contains(document, "<kind>37</kind>"));

  const auto der = xml(mirrorUsagePoint(Mirror::Der, {}, lfdi, 7));
  EXPECT_TRUE(contains(der, "<roleFlags>49</roleFlags>"));
}

TEST(CsipausReportingTest, ReadingsLeaveOutValuesTheSiteDidNotHave) {
  const auto power = reading(Mirror::Site, ReadingKind::RealPower, Phase::None,
                             0, point("p", "power_real", "W"));
  const auto volts = reading(Mirror::Site, ReadingKind::Voltage, Phase::None,
                             -1, point("v", "voltage", "V"));
  const auto document = xml(readingsDocument(
      {{power, {-1200.4, datum::Quality::Good}},
       {volts, {std::nullopt, datum::Quality::NoValue}}},
      lfdi, 0, at, std::chrono::seconds(300)));

  EXPECT_TRUE(contains(document, "all=\"1\""));
  EXPECT_TRUE(contains(document, readingMrid(lfdi, power, 0)));
  EXPECT_FALSE(contains(document, readingMrid(lfdi, volts, 0)));
  EXPECT_TRUE(contains(document, "<value>-1200</value>"));
  EXPECT_TRUE(contains(document, "<duration>300</duration><start>1790000100</start>"));
  EXPECT_TRUE(contains(document, "<qualityFlags>0001</qualityFlags>"));
}

TEST(CsipausReportingTest, ReadingsLeaveOutValuesOutOfRange) {
  const auto site = reading(Mirror::Site, ReadingKind::RealPower, Phase::None,
                            0, point("p", "power_real", "W"));
  const auto der = reading(Mirror::Der, ReadingKind::RealPower, Phase::None, 1,
                           point("d", "power_real", "W"));
  const auto document = xml(readingsDocument(
      {{site, {40000, datum::Quality::Good}},
       {der, {40000, datum::Quality::Good}}},
      lfdi, 0, at, std::chrono::seconds(300)));

  EXPECT_TRUE(contains(document, "all=\"1\""));
  EXPECT_FALSE(contains(document, readingMrid(lfdi, site, 0)))
      << "40 kW is past an Int16 of watts";
  EXPECT_TRUE(contains(document, readingMrid(lfdi, der, 0)));
  EXPECT_TRUE(contains(document, "<value>4000</value>"));
}

TEST(CsipausReportingTest, CapabilityCarriesRatingsAndSupportedModes) {
  const auto document = xml(capabilityDocument(der()));

  // opModConnect and opModMaxLimW.
  EXPECT_TRUE(contains(document, "<modesSupported>00100004</modesSupported>"));
  EXPECT_TRUE(contains(
      document, "<rtgMaxW><multiplier>1</multiplier><value>5000</value></rtgMaxW>"))
      << "50 kW does not fit an Int16 of watts";
  EXPECT_TRUE(contains(document, "<rtgVNom><multiplier>0</multiplier><value>230</value></rtgVNom>"));
  EXPECT_TRUE(contains(document, "<type>4</type>"));
  EXPECT_TRUE(contains(document, "<csipaus:doeModesSupported>03</csipaus:doeModesSupported>"));
}

TEST(CsipausReportingTest, SettingsCarryWhatIsEnabled) {
  const auto document = xml(settingsDocument(der(), 27, at));

  EXPECT_TRUE(contains(document, "<modesEnabled>00100000</modesEnabled>"));
  EXPECT_TRUE(contains(document, "<setGradW>27</setGradW>"));
  EXPECT_TRUE(contains(
      document, "<setMaxW><multiplier>0</multiplier><value>30000</value></setMaxW>"));
  EXPECT_TRUE(contains(document, "<setMaxVA><multiplier>0</multiplier><value>52000</value></setMaxVA>"))
      << "a setting left out is its rating";
  EXPECT_TRUE(contains(document, "<updatedTime>1790000100</updatedTime>"));
  EXPECT_TRUE(contains(document, "<csipaus:doeModesEnabled>01</csipaus:doeModesEnabled>"));
}

TEST(CsipausReportingTest, SettingsCarryTheSetGradWAndUpdatedTimeGiven) {
  auto description = der();
  description.setGradW = 27;
  const Timestamp changed(std::chrono::seconds(1790000500));
  const auto document = xml(settingsDocument(description, 100, changed));
  EXPECT_TRUE(contains(document, "<setGradW>100</setGradW>"));
  EXPECT_TRUE(contains(document, "<updatedTime>1790000500</updatedTime>"));

  const auto payload = settingsPayload(description, 100, changed, at);
  const auto value = [&payload](const std::string &rid) {
    for (const auto &entry : payload) {
      if (entry["rid"] == rid) {
        return entry["value"];
      }
    }
    return nlohmann::json();
  };
  EXPECT_EQ(value("set_grad_w"), 100) << "what was sent";
  EXPECT_EQ(value("updated_time"), 1790000500);
  EXPECT_EQ(payload[0]["dtstart"], isoTime(at)) << "when it was sent";
}

TEST(CsipausReportingTest, StatusComesFromTheConnectedAndOperatingState) {
  const datum::Sample on{1, datum::Quality::Good};
  const datum::Sample off{0, datum::Quality::Good};

  const auto running = derState(on, on);
  EXPECT_EQ(running.connectStatus, connected | available | operating);
  EXPECT_EQ(running.operationalMode, 2);

  const auto idle = derState(on, off);
  EXPECT_EQ(idle.connectStatus, connected | available);
  EXPECT_EQ(idle.operationalMode, 1);

  const auto isolated = derState(off, off);
  EXPECT_EQ(isolated.connectStatus, 0u) << "off the grid, not available to it";
  EXPECT_EQ(isolated.operationalMode, 1);

  const auto document = xml(statusDocument(running, at));
  EXPECT_TRUE(contains(document, "<genConnectStatus><dateTime>1790000100</dateTime><value>07</value></genConnectStatus>"));
  EXPECT_TRUE(contains(document, "<operationalModeStatus><dateTime>1790000100</dateTime><value>2</value></operationalModeStatus>"));
  EXPECT_TRUE(contains(document, "<readingTime>1790000100</readingTime>"));
}

TEST(CsipausReportingTest, AStateOtherThanZeroOrOneIsUnreadable) {
  const datum::Sample on{1, datum::Quality::Good};

  EXPECT_EQ(stateSample({0.5, datum::Quality::Good}).quality,
            datum::Quality::Unreadable);
  EXPECT_FALSE(stateSample({2, datum::Quality::Stale}).value.has_value());
  EXPECT_EQ(stateSample({1, datum::Quality::Stale}).quality,
            datum::Quality::Stale);

  const auto halfConnected = derState({0.5, datum::Quality::Good}, on);
  EXPECT_EQ(halfConnected.connectStatus, operating | faulted);
  EXPECT_EQ(halfConnected.operationalMode, 2);

  const auto unknownOperating = derState(on, {2, datum::Quality::Good});
  EXPECT_EQ(unknownOperating.connectStatus, connected | faulted);
  EXPECT_FALSE(unknownOperating.operationalMode.has_value());
}

TEST(CsipausReportingTest, AStateWithoutAValueIsUnreadable) {
  const datum::Sample on{1, datum::Quality::Good};

  const auto noOperating = derState(on, {std::nullopt, datum::Quality::NoValue});
  EXPECT_EQ(noOperating.connectStatus, connected | faulted);
  EXPECT_FALSE(noOperating.operationalMode.has_value());

  const auto unreadConnected =
      derState({std::nullopt, datum::Quality::Unreadable}, on);
  EXPECT_EQ(unreadConnected.connectStatus, operating | faulted);
  EXPECT_EQ(unreadConnected.operationalMode, 2);

  const auto neither = derState({std::nullopt, datum::Quality::NoValue},
                                {std::nullopt, datum::Quality::Unreadable});
  EXPECT_EQ(neither.connectStatus, faulted);
  EXPECT_FALSE(neither.operationalMode.has_value());
  EXPECT_TRUE(contains(xml(statusDocument(neither, at)), "<value>10</value>"));
}

TEST(CsipausReportingTest, AStaleStateStillSetsItsBits) {
  const datum::Sample stale{1, datum::Quality::Stale};
  const auto state = derState(stale, stale);
  EXPECT_EQ(state.connectStatus, 0x15u);
  EXPECT_EQ(state.operationalMode, 2);
}

TEST(CsipausReportingTest, BindsTheDersStateToStatePoints) {
  auto description = der();
  EXPECT_FALSE(bindState(description, {}).connected.has_value())
      << "nothing bound";

  description.connectedPointId = "der-connected";
  description.operatingPointId = "der-operating";
  const auto bound = bindState(
      description, {point("der-power", "power_real", "W"),
                    point("der-connected", "state", "none"),
                    point("der-operating", "state", "none")});
  ASSERT_TRUE(bound.connected.has_value());
  ASSERT_TRUE(bound.operating.has_value());
  EXPECT_EQ(bound.connected->pointId, "der-connected");
  EXPECT_EQ(bound.operating->pointId, "der-operating");

  description.operatingPointId.reset();
  const auto half = bindState(description,
                              {point("der-connected", "state", "none")});
  EXPECT_TRUE(half.connected.has_value());
  EXPECT_FALSE(half.operating.has_value());
}

TEST(CsipausReportingTest, RefusesAStateBindingToAMissingPointOrAPointOfAnotherKind) {
  auto description = der();
  description.connectedPointId = "der-connected";
  EXPECT_THROW(bindState(description, {point("der-operating", "state", "none")}),
               std::invalid_argument);

  description.connectedPointId = "der-power";
  EXPECT_THROW(bindState(description, {point("der-power", "power_real", "W")}),
               std::invalid_argument);
}
