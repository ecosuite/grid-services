#include <gtest/gtest.h>

#include <sstream>

#include <csipaussd/v11/sep.hxx>

#include "helper/documents.hpp"

using namespace csipaus::sep;
using namespace csipaus_test;

namespace {

const auto lfdi = hex({0x3E, 0x4F, 0x45, 0xAB, 0x31, 0xED, 0xFE, 0x5B, 0x67,
                       0xE3, 0x43, 0xE5, 0xE4, 0x56, 0x2E, 0x31, 0x98, 0x4E,
                       0x23, 0xE5});

// A generated mRID ending in the PEN, as CSIP-AUS asks.
xml_schema::hex_binary mrid(unsigned char id) {
  return hex({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
              id, 0x00, 0x00, 0x9C, 0x41});
}

// Site real power: average watts.
ReadingType siteRealPower() {
  ReadingType type;
  type.commodity(1);
  type.dataQualifier(2);
  type.flowDirection(1);
  type.intervalLength(300);
  type.kind(37);
  type.powerOfTenMultiplier(PowerOfTenMultiplierType(0));
  type.uom(38);
  return type;
}

} // namespace

TEST(Telemetry, WritesASiteMirrorUsagePoint) {
  // isMirror and isPremisesAggregationPoint.
  MirrorUsagePoint point(mrid(0x01), hex({0x00, 0x03}), 0, 1, lfdi);
  point.description("Site");
  MirrorMeterReading reading(mrid(0x02));
  reading.description("Site real power");
  reading.ReadingType(siteRealPower());
  point.MirrorMeterReading().push_back(reading);

  std::ostringstream out;
  MirrorUsagePoint_(out, point, namespaces(), "UTF-8", serializeFlags);
  const std::string xml = out.str();
  EXPECT_NE(xml.find("<roleFlags>0003</roleFlags>"), std::string::npos);
  EXPECT_NE(xml.find("<mRID>FFFFFFFFFFFFFFFFFFFFFF0100009C41</mRID>"),
            std::string::npos);

  std::istringstream in(xml);
  const auto reparsed = MirrorUsagePoint_(in, parseFlags);
  EXPECT_EQ(*reparsed, point);
  ASSERT_EQ(reparsed->MirrorMeterReading().size(), 1u);
  EXPECT_EQ(*reparsed->MirrorMeterReading().front().ReadingType()->uom(), 38);
}

TEST(Telemetry, WritesAReadingSetForAPostPeriod) {
  MirrorReadingSet set(mrid(0x03), DateTimeInterval(300, 1790000000));
  // Exporting 2.5 kW: site readings are positive for import.
  Reading exporting;
  exporting.value(-2500);
  set.Reading().push_back(exporting);

  MirrorMeterReading reading(mrid(0x02));
  reading.MirrorReadingSet().push_back(set);

  std::ostringstream out;
  MirrorMeterReading_(out, reading, namespaces(), "UTF-8", serializeFlags);
  EXPECT_NE(out.str().find("<value>-2500</value>"), std::string::npos);

  std::istringstream in(out.str());
  const auto reparsed = MirrorMeterReading_(in, parseFlags);
  EXPECT_EQ(*reparsed, reading);
  const auto &readingSet = reparsed->MirrorReadingSet().front();
  EXPECT_EQ(readingSet.timePeriod().start(), 1790000000);
  EXPECT_EQ(*readingSet.Reading().front().value(), -2500);
}
