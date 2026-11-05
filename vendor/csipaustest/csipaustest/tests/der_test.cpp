#include <gtest/gtest.h>

#include <sstream>

#include <csipaussd/v11/sep.hxx>

#include "helper/documents.hpp"

using namespace csipaus::sep;
using namespace csipaus_test;

namespace {

// Export, import, generation and load limits: bits 0 to 3.
const auto allDoeModes = hex({0x0F});

} // namespace

TEST(Der, WritesCapabilityWithTheEnvelopeModesItSupports) {
  // Bits 0 (charge), 1 (discharge) and 23 (fixed W) of DERControlType.
  DERCapability capability(hex({0x00, 0x80, 0x00, 0x03}),
                           ActivePower(3, 10), DERType(4), allDoeModes);
  capability.rtgMaxVA(ApparentPower(3, 10));

  std::ostringstream out;
  DERCapability_(out, capability, namespaces(), "UTF-8", serializeFlags);
  const std::string xml = out.str();
  EXPECT_NE(xml.find("<csipaus:doeModesSupported>0F"
                     "</csipaus:doeModesSupported>"),
            std::string::npos);
  EXPECT_NE(xml.find("<modesSupported>00800003</modesSupported>"),
            std::string::npos);

  std::istringstream in(xml);
  const auto reparsed = DERCapability_(in, parseFlags);
  EXPECT_EQ(*reparsed, capability);
  EXPECT_EQ(hexString(reparsed->doeModesSupported()), "0F");
}

TEST(Der, WritesSettingsWithTheEnvelopeModesEnabled) {
  DERSettings settings(27, ActivePower(3, 10), 1790000000);
  settings.doeModesEnabled(allDoeModes);
  settings.setSoftGradW(16);

  std::ostringstream out;
  DERSettings_(out, settings, namespaces(), "UTF-8", serializeFlags);
  EXPECT_NE(out.str().find("<csipaus:doeModesEnabled>0F"
                           "</csipaus:doeModesEnabled>"),
            std::string::npos);

  std::istringstream in(out.str());
  EXPECT_EQ(*DERSettings_(in, parseFlags), settings);
}

TEST(Der, WritesStatus) {
  DERStatus status(1790000300);
  // Connected, available and operating.
  status.genConnectStatus(ConnectStatusType(1790000000, hex({0x07})));
  status.operationalModeStatus(OperationalModeStatusType(1790000000, 2));

  std::ostringstream out;
  DERStatus_(out, status, namespaces(), "UTF-8", serializeFlags);

  std::istringstream in(out.str());
  const auto reparsed = DERStatus_(in, parseFlags);
  EXPECT_EQ(*reparsed, status);
  EXPECT_EQ(hexString(reparsed->genConnectStatus()->value()), "07");
  EXPECT_EQ(reparsed->operationalModeStatus()->value(), 2);
}

TEST(Der, WritesAResponseToAControl) {
  DERControlResponse response(
      hex({0x3E, 0x4F, 0x45, 0xAB, 0x31, 0xED, 0xFE, 0x5B, 0x67, 0xE3, 0x43,
           0xE5, 0xE4, 0x56, 0x2E, 0x31, 0x98, 0x4E, 0x23, 0xE5}),
      hex({0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x00, 0x00, 0x07}));
  response.createdDateTime(1790000005);
  // Event received.
  response.status(1);

  std::ostringstream out;
  DERControlResponse_(out, response, namespaces(), "UTF-8", serializeFlags);
  const std::string xml = out.str();
  EXPECT_NE(xml.find("<subject>C1000000000000000000000000000007</subject>"),
            std::string::npos);
  EXPECT_NE(xml.find("<status>1</status>"), std::string::npos);

  std::istringstream in(xml);
  EXPECT_EQ(*DERControlResponse_(in, parseFlags), response);
}
