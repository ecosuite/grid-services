// Local addition to liboadr's suite.

#include <gtest/gtest.h>

#include <sstream>

#include <oadr/request/report/UpdatedReport.h>

TEST(UpdatedReport, AcknowledgesTheVtnsReport) {
  UpdatedReport updated("ven-1", "update-1", "452", "Not requested");

  std::istringstream xml(updated.generateRequestXML());
  const auto payload = oadr2b::oadr::oadrPayload_(
      xml, xml_schema::flags::dont_validate | xml_schema::flags::dont_initialize);

  ASSERT_TRUE(payload->oadrSignedObject().oadrUpdatedReport().present());
  const auto &message = *payload->oadrSignedObject().oadrUpdatedReport();
  EXPECT_EQ(message.eiResponse().responseCode(), "452");
  EXPECT_EQ(*message.eiResponse().responseDescription(), "Not requested");
  EXPECT_EQ(message.eiResponse().requestID(), "update-1");
  ASSERT_TRUE(message.venID().present());
  EXPECT_EQ(*message.venID(), "ven-1");
  ASSERT_TRUE(message.schemaVersion().present());
  EXPECT_EQ(*message.schemaVersion(), "2.0b");
}
