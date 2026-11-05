#include <gtest/gtest.h>

#include <optional>
#include <sstream>

#include <oadrsd/2b/oadr_20b.hxx>

#include "protocols/openadr2b/normalize.hpp"
#include "support/oadr_fixtures.hpp"

using namespace grid_services;
using namespace grid_services::openadr2b;
namespace fixtures = grid_services::testing::fixtures;

namespace {

std::unique_ptr<oadr2b::oadr::oadrPayload> parse(const std::string &xml) {
  std::istringstream stream(xml);
  return oadr2b::oadr::oadrPayload_(
      stream, xml_schema::flags::keep_dom | xml_schema::flags::dont_validate |
                  xml_schema::flags::dont_initialize);
}

} // namespace

TEST(Normalize, ConvertsAnEvent) {
  fixtures::EventSpec spec;
  spec.id = "ev-42";
  spec.modification = 3;
  spec.status = "near";
  spec.start = "2030-07-14T21:00:00Z";
  spec.duration = "PT3H";
  spec.intervals = {{"PT1H", 1.0}, {"PT2H", 2.0}};
  spec.comment = "heat wave";
  spec.responseRequired = "never";

  const auto payload = parse(fixtures::distributeEvent("req-7", {spec}));
  const auto &event =
      payload->oadrSignedObject().oadrDistributeEvent()->oadrEvent().front();
  const auto normalized = normalizeEvent(event, "req-7");

  EXPECT_EQ(normalized.programEventId, "ev-42");
  EXPECT_EQ(normalized.modificationNumber, 3);
  EXPECT_EQ(normalized.status, ProgramStatus::Pending);
  EXPECT_EQ(normalized.startAt.time_since_epoch().count(), 1'910'293'200);
  EXPECT_EQ(normalized.durationSeconds, 3 * 3600);
  EXPECT_EQ(normalized.marketContext, "http://market.example.com");
  EXPECT_FALSE(normalized.testEvent);
  EXPECT_FALSE(normalized.responseRequired);
  EXPECT_EQ(normalized.comment, "heat wave");
  EXPECT_EQ(normalized.programRequestId, "req-7");

  ASSERT_EQ(normalized.signals.size(), 1u);
  const auto &signal = normalized.signals.front();
  EXPECT_EQ(signal.name, "simple");
  EXPECT_EQ(signal.type, "level");
  EXPECT_EQ(signal.id, "signal-1");
  ASSERT_EQ(signal.intervals.size(), 2u);
  EXPECT_EQ(signal.intervals[1].offsetSeconds, 3600);
  EXPECT_EQ(signal.intervals[1].durationSeconds, 7200);
  EXPECT_DOUBLE_EQ(signal.intervals[1].value, 2.0);
}

TEST(Normalize, MapsEventStatuses) {
  const std::vector<std::pair<std::string, ProgramStatus>> cases = {
      {"none", ProgramStatus::Pending},
      {"far", ProgramStatus::Pending},
      {"near", ProgramStatus::Pending},
      {"active", ProgramStatus::Active},
      {"completed", ProgramStatus::Completed},
      {"cancelled", ProgramStatus::Cancelled},
  };
  for (const auto &[status, expected] : cases) {
    fixtures::EventSpec spec;
    spec.status = status;
    const auto payload = parse(fixtures::distributeEvent("r", {spec}));
    const auto &event =
        payload->oadrSignedObject().oadrDistributeEvent()->oadrEvent().front();
    EXPECT_EQ(normalizeEvent(event, "r").status, expected) << status;
  }
}

TEST(Normalize, TestEventFlag) {
  fixtures::EventSpec spec;
  spec.testEvent = "true";
  const auto payload = parse(fixtures::distributeEvent("r", {spec}));
  const auto &event =
      payload->oadrSignedObject().oadrDistributeEvent()->oadrEvent().front();
  EXPECT_TRUE(normalizeEvent(event, "r").testEvent);
}

TEST(Normalize, Targeting) {
  fixtures::EventSpec mine;
  fixtures::EventSpec other;
  other.venIds = {"ven-2"};
  fixtures::EventSpec untargeted;
  untargeted.venIds = {};

  const auto payload =
      parse(fixtures::distributeEvent("r", {mine, other, untargeted}));
  const auto &events =
      payload->oadrSignedObject().oadrDistributeEvent()->oadrEvent();
  EXPECT_TRUE(targetsVen(events[0], "ven-1"));
  EXPECT_FALSE(targetsVen(events[1], "ven-1"));
  EXPECT_TRUE(targetsVen(events[2], "ven-1"));
}

// ---------------------------------------------------------------------------
// OpenADR 2.0b Profile Specification v1.1, section 11.2 conformance rules.

// Rule 6: "The presence of any string except 'false' in the
// oadrDistributeEvent testEvent element MUST be treated as a trigger for a
// test event."
// Rule 46: "Empty or omitted elements are considered equal unless a
// conformance rule explicitly states that element tags must be excluded from
// the payload." An omitted testEvent is not a test event, so neither is an
// empty one.
TEST(NormalizeConformance, Rule6_EmptyTestEventIsNotATestEvent) {
  fixtures::EventSpec empty;
  empty.id = "empty";
  empty.testEvent = "";
  fixtures::EventSpec other;
  other.id = "other";
  other.testEvent = "yes";

  const auto payload = parse(fixtures::distributeEvent("r", {empty, other}));
  const auto &events =
      payload->oadrSignedObject().oadrDistributeEvent()->oadrEvent();
  EXPECT_FALSE(normalizeEvent(events[0], "r").testEvent);
  EXPECT_TRUE(normalizeEvent(events[1], "r").testEvent);
}

namespace {

const oadr2b::oadr::oadrEvent &
firstEvent(const std::unique_ptr<oadr2b::oadr::oadrPayload> &payload) {
  return payload->oadrSignedObject().oadrDistributeEvent()->oadrEvent().front();
}

} // namespace

// Rule 30: "The VEN MUST randomize the dtstart time of the event if a value is
// present in the startafter element."
// Rule 31: "The VEN MUST recognize and act upon values specified in the
// subelements of activePeriod including: ... tolerance".
// The planner randomizes, so the normalized event carries the window.
TEST(NormalizeConformance, Rule30_StartAfterIsPassedThrough) {
  fixtures::EventSpec randomized;
  randomized.startAfter = "PT10M";
  const auto withWindow = parse(fixtures::distributeEvent("r", {randomized}));
  EXPECT_EQ(normalizeEvent(firstEvent(withWindow), "r").startAfterSeconds, 600);

  const auto without =
      parse(fixtures::distributeEvent("r", {fixtures::EventSpec{}}));
  EXPECT_FALSE(normalizeEvent(firstEvent(without), "r").startAfterSeconds);
}

// Rule 1: "Fractional seconds in date-time values are allowed by [ISO8601],
// however it is the responsibility of the receiving device to truncate the
// fractional seconds if necessary."
TEST(NormalizeConformance, Rule1_FractionalSecondsAreTruncated) {
  fixtures::EventSpec spec;
  spec.start = "2030-07-14T21:00:00.789Z";
  const auto payload = parse(fixtures::distributeEvent("r", {spec}));
  EXPECT_EQ(normalizeEvent(firstEvent(payload), "r")
                .startAt.time_since_epoch()
                .count(),
            1'910'293'200);
}

// Rule 47: "An event with an overall duration of 0 indicates an event with no
// defined end time and will remain active until explicitly canceled."
TEST(NormalizeConformance, Rule47_ZeroDurationIsOpenEnded) {
  fixtures::EventSpec spec;
  spec.duration = "PT0S";
  spec.intervals = {{"PT0S", 1.0}};
  const auto payload = parse(fixtures::distributeEvent("r", {spec}));
  EXPECT_EQ(normalizeEvent(firstEvent(payload), "r").durationSeconds, 0);
}

// Rule 512: "venID and vtnID MUST be case-sensitive (e.g., a vtnID of 'vtnID1'
// is different from 'vtnid1' when parsing an incoming message)."
TEST(NormalizeConformance, Rule512_VenIdsAreCaseSensitive) {
  const auto payload =
      parse(fixtures::distributeEvent("r", {fixtures::EventSpec{}}));
  EXPECT_TRUE(targetsVen(firstEvent(payload), "ven-1"));
  EXPECT_FALSE(targetsVen(firstEvent(payload), "VEN-1"));
}

TEST(Normalize, AcceptedSignals) {
  NormalizedEvent event;
  event.signals = {{"simple", "level", "s1", {}}};
  EXPECT_FALSE(unacceptedSignal(event, {"SIMPLE"}));

  event.signals.push_back({"ELECTRICITY_PRICE", "price", "s2", {}});
  const auto refusal = unacceptedSignal(event, {"SIMPLE"});
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->code, "460");
  EXPECT_NE(refusal->reason.find("ELECTRICITY_PRICE"), std::string::npos);

  EXPECT_FALSE(unacceptedSignal(event, {"SIMPLE", "ELECTRICITY_PRICE"}));
  EXPECT_TRUE(unacceptedSignal(event, {}));
}

TEST(NormalizeConformance, Rule9_SimpleLevelsAreZeroToThree) {
  NormalizedEvent event;
  event.signals = {{"SIMPLE", "level", "s1", {{0, 60, 0}, {60, 60, 3}}}};
  EXPECT_FALSE(outOfRangeSignal(event));

  event.signals[0].intervals.push_back({120, 60, 1.5});
  const auto refusal = outOfRangeSignal(event);
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->code, "454");
  EXPECT_NE(refusal->reason.find("1.5"), std::string::npos);

  // Only SIMPLE has fixed levels.
  event.signals = {{"ELECTRICITY_PRICE", "price", "s2", {{0, 60, 12.5}}}};
  EXPECT_FALSE(outOfRangeSignal(event));
}
