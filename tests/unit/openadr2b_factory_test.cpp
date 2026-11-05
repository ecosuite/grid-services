#include <gtest/gtest.h>

#include "core/errors.hpp"
#include "crypto/sealed_box.hpp"
#include "protocols/openadr2b/openadr2b_adapter.hpp"
#include "protocols/openadr2b/reports.hpp"
#include "support/memory_store.hpp"

using namespace grid_services;
using namespace grid_services::openadr2b;
using namespace grid_services::testing;

namespace {

TelemetryPoint point(const std::string &pointId,
                     const std::string &property = "watts") {
  TelemetryPoint point;
  point.pointId = pointId;
  point.source.nodeId = 604;
  point.source.sourceId = "/S/1";
  point.source.property = property;
  return point;
}

ReportOfferRow offerRow(nlohmann::json dataPoints) {
  ReportOfferRow row;
  row.id = 7;
  row.reportName = "SITE_LOAD";
  row.specifierId = "load-1";
  row.dataPoints = std::move(dataPoints);
  return row;
}

} // namespace

TEST(BindOffer, CopiesEachPointsItemAndSource) {
  auto energy = point("site-energy", "wattHours");
  energy.item.kind = "energy_real";
  energy.item.units = "Wh";
  const auto offer = bindOffer(
      offerRow({{{"rid", "power"}, {"point_id", "site-power"},
                 {"resource_id", "SITE"}, {"report_type", "demand"}},
                {{"point_id", "site-energy"}, {"resource_id", "SITE"}}}),
      {point("site-power"), energy});

  EXPECT_EQ(offer.id, 7);
  EXPECT_EQ(offer.specifierId, "load-1");
  ASSERT_EQ(offer.dataPoints.size(), 2u);
  EXPECT_EQ(offer.dataPoints[0].rid, "power");
  EXPECT_EQ(offer.dataPoints[0].reportType, "demand");
  EXPECT_EQ(offer.dataPoints[0].source.property, "watts");
  EXPECT_EQ(offer.dataPoints[1].rid, "site-energy")
      << "the rID defaults to the point's id";
  EXPECT_EQ(offer.dataPoints[1].readingType, "Direct Read");
  EXPECT_EQ(offer.dataPoints[1].item.units, "Wh");
  EXPECT_EQ(offer.dataPoints[1].source.property, "wattHours");
}

TEST(BindOffer, RefusesWhatCannotBeReported) {
  const std::vector<TelemetryPoint> points = {point("site-power")};
  EXPECT_THROW(bindOffer(offerRow({{{"point_id", "missing"},
                                    {"resource_id", "SITE"}}}),
                         points),
               std::invalid_argument);
  EXPECT_THROW(bindOffer(offerRow({{{"rid", "power"}, {"resource_id", "SITE"}}}),
                         points),
               std::invalid_argument)
      << "a data point names its telemetry point";
  EXPECT_THROW(bindOffer(offerRow({{{"point_id", "site-power"}}}), points),
               std::invalid_argument)
      << "and its resource";
  EXPECT_THROW(
      bindOffer(offerRow({{{"point_id", "site-power"}, {"resource_id", "A"}},
                          {{"point_id", "site-power"}, {"resource_id", "B"}}}),
                points),
      std::invalid_argument)
      << "an rID appears once";
  EXPECT_THROW(bindOffer(offerRow(nlohmann::json::object()), points),
               std::invalid_argument);
}

TEST(BindOffer, RefusesStatePoints) {
  auto connected = point("der-connected", "connected");
  connected.item.kind = "state";
  connected.item.units = "none";
  EXPECT_THROW(bindOffer(offerRow({{{"point_id", "der-connected"},
                                    {"resource_id", "SITE"}}}),
                         {connected}),
               std::invalid_argument)
      << "a state is CSIP-AUS's, not a value OpenADR reports";
}

class Openadr2bFactoryTest : public ::testing::Test {
protected:
  void SetUp() override {
    ConnectionConfig config;
    config.protocol = "openadr2b";
    connection = db->addConnection(config);
    Openadr2bSettings settings;
    settings.vtnUrl = "https://vtn.example.com";
    settings.venName = "site-ven";
    db->openadr2b[connection] = settings;
    store = std::make_unique<MemoryRunnerStore>(db, connection);
  }

  std::unique_ptr<Adapter> create(const SecretOpener *opener) {
    return makeOpenadr2bFactory(opener)(
        {db->connections.at(connection), *store, abort});
  }

  void offerPower() {
    db->telemetryPoints[connection] = {point("site-power")};
    db->reportOffers[connection] = {
        offerRow({{{"point_id", "site-power"}, {"resource_id", "SITE"}}})};
  }

  AdapterError::Kind failure(const SecretOpener *opener) {
    try {
      create(opener);
    } catch (const AdapterError &error) {
      return error.kind();
    }
    ADD_FAILURE() << "expected an AdapterError";
    return AdapterError::Kind::Stopped;
  }

  KeyPair keys = generateKeyPair();
  SecretOpener opener{keys.secretKeyBase64};
  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  ConnectionId connection = 0;
  std::unique_ptr<MemoryRunnerStore> store;
  std::atomic<bool> abort{false};
};

TEST_F(Openadr2bFactoryTest, BuildsFromTheConnectionsTables) {
  offerPower();
  db->dataCredentials[connection] = {"data.solarnetwork.net", "tok",
                                     seal(keys.publicKeyBase64, "secret")};
  db->clientCredentials[connection] = {"CERT",
                                       seal(keys.publicKeyBase64, "KEY")};
  const auto adapter = create(&opener);
  ASSERT_TRUE(adapter);
  EXPECT_EQ(adapter->describeClient("ven-1"), "VEN ven-1");
}

TEST_F(Openadr2bFactoryTest, AnOfferForAMissingPointIsInvalid) {
  db->reportOffers[connection] = {
      offerRow({{{"point_id", "site-power"}, {"resource_id", "SITE"}}})};
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);
}

TEST_F(Openadr2bFactoryTest, ReportedPointsNeedADataCredential) {
  offerPower();
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);
}

TEST_F(Openadr2bFactoryTest, ASealedClientKeyNeedsTheWorkersKey) {
  db->clientCredentials[connection] = {"CERT",
                                       seal(keys.publicKeyBase64, "KEY")};
  EXPECT_EQ(failure(nullptr), AdapterError::Kind::Permanent);
  const SecretOpener other(generateKeyPair().secretKeyBase64);
  EXPECT_EQ(failure(&other), AdapterError::Kind::Permanent)
      << "sealed for another worker";
  EXPECT_TRUE(create(&opener));
}
