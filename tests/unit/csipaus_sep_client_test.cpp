#include <gtest/gtest.h>

#include <algorithm>
#include <functional>

#include "protocols/csipaus/sep_client.hpp"
#include "support/csipaus_fixtures.hpp"
#include "support/scripted_rest.hpp"

using namespace grid_services;
using namespace grid_services::csipaus;
using namespace grid_services::testing;
namespace fixtures = grid_services::testing::csipaus_fixtures;

namespace {

class SepClientTest : public ::testing::Test {
protected:
  std::shared_ptr<RestScript> rest = std::make_shared<RestScript>();
  SepClient client{"https://utility.example.com/sep2/dcap",
                   std::make_unique<ScriptedRest>(rest)};

  AdapterError failure(const std::function<void()> &operation) {
    try {
      operation();
    } catch (const AdapterError &error) {
      return error;
    }
    ADD_FAILURE() << "expected an AdapterError";
    return AdapterError::stopped();
  }
};

} // namespace

TEST_F(SepClientTest, ResolvesHrefsAgainstTheServer) {
  rest->on("GET /sep2/dcap", fixtures::deviceCapability());

  EXPECT_EQ(client.dcapHref(), "/sep2/dcap");
  const auto dcap = client.get<sep::DeviceCapability>(client.dcapHref());

  EXPECT_EQ(dcap->EndDeviceListLink()->href(), "/edev");
  ASSERT_EQ(rest->requests.size(), 1u);
  const auto &headers = rest->requests[0].headers;
  EXPECT_NE(std::find(headers.begin(), headers.end(),
                      "Accept: application/sep+xml"),
            headers.end());
}

TEST_F(SepClientTest, ReadsEveryPageOfAList) {
  // The server returns fewer than asked for, as it may.
  rest->queue("GET /edev",
              {200,
               fixtures::endDeviceList(
                   {fixtures::endDeviceElement("/edev/1", "11", 1),
                    fixtures::endDeviceElement("/edev/2", "22", 2)},
                   3),
               {},
               std::nullopt});
  rest->queue("GET /edev",
              {200,
               fixtures::endDeviceList(
                   {fixtures::endDeviceElement("/edev/3", "33", 3)}, 3),
               {},
               std::nullopt});

  const auto list = client.getList<sep::EndDeviceList>("/edev");

  ASSERT_EQ(list->EndDevice().size(), 3u);
  EXPECT_EQ(*list->EndDevice()[2].href(), "/edev/3");
  ASSERT_EQ(rest->requests.size(), 2u);
  EXPECT_EQ(rest->requests[0].target, "/edev?s=0&l=100");
  EXPECT_EQ(rest->requests[1].target, "/edev?s=2&l=100");
}

TEST_F(SepClientTest, StopsAtAnEmptyPage) {
  rest->queue("GET /edev",
              {200,
               fixtures::endDeviceList(
                   {fixtures::endDeviceElement("/edev/1", "11", 1)}, 5),
               {},
               std::nullopt});
  rest->queue("GET /edev", {200, fixtures::endDeviceList({}, 5), {}, {}});

  EXPECT_EQ(client.getList<sep::EndDeviceList>("/edev")->EndDevice().size(),
            1u);
  EXPECT_EQ(rest->requests.size(), 2u);
}

TEST_F(SepClientTest, ANotFoundIsTransientAndFindMakesItNull) {
  EXPECT_EQ(client.find<sep::EndDevice>("/edev/9"), nullptr);
  EXPECT_THROW(client.get<sep::EndDevice>("/edev/9"), NotFound);
  EXPECT_EQ(failure([&] { client.get<sep::EndDevice>("/edev/9"); }).kind(),
            AdapterError::Kind::Transient);
}

TEST_F(SepClientTest, ARefusalIsPermanentAndSaysWhy) {
  rest->on("GET /edev/9",
           "<Error xmlns=\"urn:ieee:std:2030.5:ns\">"
           "<reasonCode>1</reasonCode></Error>",
           403);

  const auto error = failure([&] { client.get<sep::EndDevice>("/edev/9"); });
  EXPECT_EQ(error.kind(), AdapterError::Kind::Permanent);
  EXPECT_STREQ(error.what(), "GET /edev/9 gave 403, reason code 1");
}

TEST_F(SepClientTest, AServerErrorIsTransient) {
  rest->on("GET /edev/9", "", 503);
  EXPECT_EQ(failure([&] { client.get<sep::EndDevice>("/edev/9"); }).kind(),
            AdapterError::Kind::Transient);
}

TEST_F(SepClientTest, RefusesADocumentWithElementsItDoesNotRead) {
  rest->on("GET /derp/1/dderc",
           "<DefaultDERControl xmlns=\"urn:ieee:std:2030.5:ns\" "
           "xmlns:csipaus=\"https://csipaus.org/ns\" "
           "xmlns:vendor=\"urn:example:vendor\">"
           "<mRID>D1000000000000000000000000000002</mRID>"
           "<DERControlBase>"
           "<opModEnergize>true</opModEnergize>"
           "<vendor:opModCurtail>1</vendor:opModCurtail>"
           "<csipaus:opModExpLimW><multiplier>0</multiplier>"
           "<value>1500</value></csipaus:opModExpLimW>"
           "</DERControlBase>"
           "</DefaultDERControl>");

  const auto error = failure(
      [&] { client.get<sep::DefaultDERControl>("/derp/1/dderc"); });
  EXPECT_EQ(error.kind(), AdapterError::Kind::Transient);
  EXPECT_STREQ(error.what(),
               "the server's DefaultDERControl at /derp/1/dderc has elements "
               "this worker does not read: "
               "DefaultDERControl/DERControlBase/vendor:opModCurtail "
               "(urn:example:vendor), "
               "DefaultDERControl/DERControlBase/csipaus:opModExpLimW "
               "(https://csipaus.org/ns)");
}

TEST_F(SepClientTest, AnswersThatAreNotXmlAreTransient) {
  rest->on("GET /edev/3", "<html>maintenance</h1>");
  EXPECT_EQ(failure([&] { client.get<sep::EndDevice>("/edev/3"); }).kind(),
            AdapterError::Kind::Transient);
}

TEST_F(SepClientTest, PostReturnsTheCreatedResourcesHref) {
  rest->queue("POST /edev", {201,
                             "",
                             {{"Location", "https://utility.example.com/edev/7"}},
                             std::nullopt});
  const sep::EndDevice device(167261211391ull, 1790000000);

  EXPECT_EQ(client.post("/edev", device), "/edev/7");
  const auto &request = rest->requests.at(0);
  EXPECT_NE(std::find(request.headers.begin(), request.headers.end(),
                      "Content-Type: application/sep+xml"),
            request.headers.end());
  EXPECT_NE(request.body.find("<sFDI>167261211391</sFDI>"), std::string::npos);
  EXPECT_EQ(request.body.find("<?xml"), std::string::npos);
}

TEST_F(SepClientTest, APostWithoutALocationIsTransient) {
  rest->on("POST /edev", "", 201);
  const sep::EndDevice device(167261211391ull, 1790000000);
  EXPECT_EQ(failure([&] { client.post("/edev", device); }).kind(),
            AdapterError::Kind::Transient);
}

TEST_F(SepClientTest, RecordsEachExchange) {
  rest->on("GET /sep2/dcap", fixtures::deviceCapability());
  client.get<sep::DeviceCapability>("/sep2/dcap");

  const auto exchange = client.takeExchange();
  ASSERT_EQ(exchange.size(), 2u);
  EXPECT_EQ(exchange[0]["direction"], "to_program");
  EXPECT_EQ(exchange[0]["method"], "GET");
  EXPECT_EQ(exchange[0]["url"], "https://utility.example.com/sep2/dcap");
  EXPECT_EQ(exchange[1]["direction"], "from_program");
  EXPECT_EQ(exchange[1]["status"], 200);
  EXPECT_EQ(exchange[1]["format"], "xml");
  EXPECT_EQ(exchange[1]["body"], fixtures::deviceCapability());
  EXPECT_TRUE(client.takeExchange().is_null());
}
