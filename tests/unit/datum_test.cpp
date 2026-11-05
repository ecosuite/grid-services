#include <gtest/gtest.h>

#include "datum/datum.hpp"
#include "datum/solarnetwork_client.hpp"

using namespace grid_services::datum;
using namespace std::chrono_literals;

namespace {

Timestamp at(long seconds) { return Timestamp(std::chrono::seconds(seconds)); }

Datum datum(long seconds, double watts, double wattHours) {
  return {at(seconds), {{"watts", watts}, {"wattHours", wattHours}}};
}

const auto start = at(1000);
const auto end = at(1300);

DatumSource wattsSource() {
  DatumSource source;
  source.nodeId = 604;
  source.sourceId = "/LN/RC/S1/MET/1";
  source.property = "watts";
  return source;
}

DatumSource wattHoursSource() {
  auto source = wattsSource();
  source.property = "wattHours";
  source.propertyClass = PropertyClass::Accumulating;
  source.reduce = Reduce::Difference;
  return source;
}

const std::vector<Datum> series = {
    datum(940, 100, 1000), datum(1000, 200, 1010), datum(1060, 400, 1030),
    datum(1120, 300, 1040), datum(1300, 900, 1100),
};

} // namespace

TEST(Reduce, AveragesInstantaneousValuesInTheWindow) {
  const auto sample = reduceWindow(series, wattsSource(), start, end);
  ASSERT_TRUE(sample.value.has_value());
  EXPECT_DOUBLE_EQ(*sample.value, 300);
  EXPECT_EQ(sample.quality, Quality::Good);
}

TEST(Reduce, TakesMinMaxAndLast) {
  auto source = wattsSource();
  source.reduce = Reduce::Min;
  EXPECT_DOUBLE_EQ(*reduceWindow(series, source, start, end).value, 200);
  source.reduce = Reduce::Max;
  EXPECT_DOUBLE_EQ(*reduceWindow(series, source, start, end).value, 400);
  source.reduce = Reduce::Last;
  EXPECT_DOUBLE_EQ(*reduceWindow(series, source, start, end).value, 300);
}

TEST(Reduce, DifferencesTheReadingsAtTheWindowBoundaries) {
  const auto sample = reduceWindow(series, wattHoursSource(), start, end);
  EXPECT_DOUBLE_EQ(*sample.value, 90);
  const auto shifted =
      reduceWindow(series, wattHoursSource(), at(1030), at(1200));
  EXPECT_DOUBLE_EQ(*shifted.value, 30);
}

TEST(Reduce, DifferencesWithinTheWindowWithoutAnEarlierReading) {
  const std::vector<Datum> inWindowOnly(series.begin() + 2, series.end());
  const auto sample = reduceWindow(inWindowOnly, wattHoursSource(), start, end);
  EXPECT_DOUBLE_EQ(*sample.value, 70);
}

TEST(Reduce, AppliesTheMultiplier) {
  auto source = wattsSource();
  source.multiplier = -0.001;
  EXPECT_DOUBLE_EQ(*reduceWindow(series, source, start, end).value, -0.3);
}

TEST(Reduce, ReportsNoValueForAnEmptyWindowOrMissingProperty) {
  EXPECT_EQ(reduceWindow(series, wattsSource(), at(2000), at(2300)).quality,
            Quality::NoValue);
  auto source = wattsSource();
  source.property = "voltage";
  const auto sample = reduceWindow(series, source, start, end);
  EXPECT_FALSE(sample.value.has_value());
  EXPECT_EQ(sample.quality, Quality::NoValue);
}

TEST(Reduce, LatestSampleIsStaleBeyondTheAllowance) {
  auto source = wattsSource();
  source.staleAfter = 120s;
  const auto fresh = latestSample(datum(1000, 50, 0), source, at(1100));
  EXPECT_EQ(fresh.quality, Quality::Good);
  EXPECT_DOUBLE_EQ(*fresh.value, 50);
  const auto stale = latestSample(datum(1000, 50, 0), source, at(1200));
  EXPECT_EQ(stale.quality, Quality::Stale);
  EXPECT_DOUBLE_EQ(*stale.value, 50);
  EXPECT_EQ(latestSample(std::nullopt, source, at(1200)).quality,
            Quality::NoValue);
}

TEST(Reduce, ParsesAndDefaults) {
  EXPECT_EQ(parseReduce("difference"), Reduce::Difference);
  EXPECT_EQ(parsePropertyClass("accumulating"), PropertyClass::Accumulating);
  EXPECT_EQ(defaultReduce(PropertyClass::Accumulating), Reduce::Difference);
  EXPECT_EQ(defaultReduce(PropertyClass::Instantaneous), Reduce::Average);
  EXPECT_THROW(parseReduce("median"), std::invalid_argument);
}

TEST(Quality, IsRecordedInTheWorkersOwnTerms) {
  EXPECT_EQ(toString(Quality::Good), "good");
  EXPECT_EQ(toString(Quality::NoValue), "no_value");
  EXPECT_EQ(toString(Quality::Stale), "stale");
  EXPECT_EQ(toString(Quality::Unreadable), "unreadable");
}

TEST(SolarNetworkParsing, ReadsDatumTimes) {
  EXPECT_EQ(parseSolarNetworkTime("2026-09-30 13:00:00.000Z"),
            at(1790773200));
  EXPECT_EQ(parseSolarNetworkTime("2026-09-30T13:00:00Z"), at(1790773200));
  EXPECT_THROW(parseSolarNetworkTime("yesterday"), DatumError);
  EXPECT_EQ(formatQueryTime(at(1790773230)), "2026-09-30T13:00");
}

TEST(SolarNetworkParsing, ReadsAListPage) {
  const auto page = parseListResponse(R"({
    "success": true,
    "data": {
      "totalResults": 2, "startingOffset": 0, "returnedResultCount": 2,
      "results": [
        {"created": "2026-09-30 13:00:00.000Z", "nodeId": 604,
         "sourceId": "/LN/RC/S1/MET/1", "localDate": "2026-09-30",
         "localTime": "13:00", "watts": 1234.5, "wattHours": 99},
        {"created": "2026-09-30 13:01:00.000Z", "nodeId": 604,
         "sourceId": "/LN/RC/S1/MET/1",
         "i": {"watts": 1300}, "a": {"wattHours": 100}, "s": {"mode": "on"}}
      ]
    }
  })");
  ASSERT_EQ(page.datums.size(), 2u);
  EXPECT_EQ(page.totalResults, 2);
  EXPECT_EQ(page.returnedResultCount, 2);
  EXPECT_EQ(page.datums[0].at, at(1790773200));
  EXPECT_DOUBLE_EQ(page.datums[0].values.at("watts"), 1234.5);
  EXPECT_EQ(page.datums[0].values.count("nodeId"), 0u);
  EXPECT_DOUBLE_EQ(page.datums[1].values.at("watts"), 1300);
  EXPECT_DOUBLE_EQ(page.datums[1].values.at("wattHours"), 100);
  EXPECT_EQ(page.datums[1].values.count("mode"), 0u);
}

TEST(SolarNetworkParsing, ReadsMostRecentAndFailures) {
  const auto latest = parseMostRecentResponse(
      R"({"success": true, "data": {"results": [
            {"created": "2026-09-30 13:05:00.000Z", "watts": 7}]}})");
  ASSERT_TRUE(latest.has_value());
  EXPECT_DOUBLE_EQ(latest->values.at("watts"), 7);
  EXPECT_FALSE(parseMostRecentResponse(
                   R"({"success": true, "data": {"results": []}})")
                   .has_value());
  EXPECT_THROW(parseMostRecentResponse(
                   R"({"success": false, "message": "Forbidden"})"),
               DatumError);
  EXPECT_THROW(parseListResponse("<html>"), DatumError);
  EXPECT_THROW(parseListResponse(R"({"success": true})"), DatumError);
}
