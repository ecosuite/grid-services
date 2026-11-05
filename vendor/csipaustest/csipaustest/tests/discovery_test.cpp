#include <gtest/gtest.h>

#include <csipaussd/v11/sep.hxx>

#include "helper/documents.hpp"

using namespace csipaus::sep;
using namespace csipaus_test;

TEST(Discovery, ReadsDeviceCapabilityLinks) {
  auto stream = fixture("device_capability.xml");
  const auto dcap = DeviceCapability_(stream, parseFlags);

  EXPECT_EQ(*dcap->href(), "/dcap");
  EXPECT_EQ(dcap->pollRate(), 86400u);
  ASSERT_TRUE(dcap->TimeLink());
  EXPECT_EQ(dcap->TimeLink()->href(), "/tm");
  ASSERT_TRUE(dcap->EndDeviceListLink());
  EXPECT_EQ(dcap->EndDeviceListLink()->href(), "/edev");
  EXPECT_EQ(*dcap->EndDeviceListLink()->all(), 1u);
  ASSERT_TRUE(dcap->MirrorUsagePointListLink());
  EXPECT_EQ(dcap->MirrorUsagePointListLink()->href(), "/mup");
  EXPECT_FALSE(dcap->DERProgramListLink());
}

TEST(Discovery, ReadsTime) {
  auto stream = fixture("time.xml");
  const auto time = Time_(stream, parseFlags);

  EXPECT_EQ(time->currentTime(), 1790000000);
  EXPECT_EQ(time->tzOffset(), 36000);
  EXPECT_EQ(time->dstOffset(), 3600);
  EXPECT_EQ(time->quality(), 7);
  EXPECT_EQ(time->pollRate(), 900u);
}

TEST(Discovery, ReadsOurEndDeviceAndItsConnectionPointLink) {
  auto stream = fixture("end_device_list.xml");
  const auto list = EndDeviceList_(stream, parseFlags);

  EXPECT_EQ(list->all(), 1u);
  EXPECT_EQ(list->pollRate(), 300u);
  ASSERT_EQ(list->EndDevice().size(), 1u);
  const auto &device = list->EndDevice().front();
  EXPECT_EQ(*device.href(), "/edev/3");
  ASSERT_TRUE(device.lFDI());
  EXPECT_EQ(hexString(*device.lFDI()),
            "3E4F45AB31EDFE5B67E343E5E4562E31984E23E5");
  EXPECT_EQ(device.sFDI(), 167261211391ull);
  EXPECT_EQ(device.changedTime(), 1790000000);
  ASSERT_TRUE(device.enabled());
  EXPECT_TRUE(*device.enabled());
  EXPECT_EQ(*device.postRate(), 300u);
  ASSERT_TRUE(device.FunctionSetAssignmentsListLink());
  EXPECT_EQ(device.FunctionSetAssignmentsListLink()->href(), "/edev/3/fsa");
  ASSERT_TRUE(device.RegistrationLink());
  EXPECT_EQ(device.RegistrationLink()->href(), "/edev/3/rg");
  ASSERT_TRUE(device.ConnectionPointLink());
  EXPECT_EQ(device.ConnectionPointLink()->href(), "/edev/3/cp");
}

TEST(Discovery, ReadsTheRegistrationPin) {
  auto stream = fixture("registration.xml");
  const auto registration = Registration_(stream, parseFlags);

  EXPECT_EQ(registration->pIN(), 111115u);
  EXPECT_EQ(registration->dateTimeRegistered(), 1789900000);
}

TEST(Discovery, ReadsAServerError) {
  auto stream = fixture("error.xml");
  const auto error = Error_(stream, parseFlags);

  EXPECT_EQ(error->reasonCode(), 1);
  ASSERT_TRUE(error->maxRetryDuration());
  EXPECT_EQ(*error->maxRetryDuration(), 600);
}
