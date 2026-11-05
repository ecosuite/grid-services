#include <gtest/gtest.h>

#include <sstream>

#include <csipaussd/v11/csipaus-ext.hxx>
#include <csipaussd/v11/sep.hxx>

#include "helper/documents.hpp"

using namespace csipaus_test;

TEST(Registration, WritesAnEndDeviceForInBandRegistration) {
  csipaus::sep::EndDevice device(167261211391ull, 1790000000);
  device.lFDI(hex({0x3E, 0x4F, 0x45, 0xAB, 0x31, 0xED, 0xFE, 0x5B, 0x67, 0xE3,
                   0x43, 0xE5, 0xE4, 0x56, 0x2E, 0x31, 0x98, 0x4E, 0x23,
                   0xE5}));

  std::ostringstream out;
  csipaus::sep::EndDevice_(out, device, namespaces(), "UTF-8",
                           serializeFlags);
  const std::string xml = out.str();
  EXPECT_NE(xml.find("<EndDevice xmlns=\"urn:ieee:std:2030.5:ns\""),
            std::string::npos);
  EXPECT_NE(xml.find("<lFDI>3E4F45AB31EDFE5B67E343E5E4562E31984E23E5</lFDI>"),
            std::string::npos);
  EXPECT_NE(xml.find("<sFDI>167261211391</sFDI>"), std::string::npos);

  std::istringstream in(xml);
  EXPECT_EQ(*csipaus::sep::EndDevice_(in, parseFlags), device);
}

TEST(Registration, WritesTheConnectionPoint) {
  csipaus::ext::ConnectionPointType point("4102345678");

  std::ostringstream out;
  csipaus::ext::ConnectionPoint(out, point, namespaces(), "UTF-8",
                                serializeFlags);
  const std::string xml = out.str();
  EXPECT_NE(xml.find("<csipaus:ConnectionPoint"), std::string::npos);
  EXPECT_NE(xml.find("xmlns:csipaus=\"https://csipaus.org/ns\""),
            std::string::npos);
  EXPECT_NE(xml.find("<csipaus:connectionPointId>4102345678"
                     "</csipaus:connectionPointId>"),
            std::string::npos);

  std::istringstream in(xml);
  const auto reparsed = csipaus::ext::ConnectionPoint(in, parseFlags);
  EXPECT_EQ(reparsed->connectionPointId(), "4102345678");
}
