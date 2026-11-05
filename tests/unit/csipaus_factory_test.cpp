#include <gtest/gtest.h>

#include "core/errors.hpp"
#include "crypto/sealed_box.hpp"
#include "protocols/csipaus/csipaus_adapter.hpp"
#include "support/csipaus_fixtures.hpp"
#include "support/memory_store.hpp"

using namespace grid_services;
using namespace grid_services::csipaus;
using namespace grid_services::testing;
namespace fixtures = grid_services::testing::csipaus_fixtures;

namespace {

class CsipausFactoryTest : public ::testing::Test {
protected:
  void SetUp() override {
    ConnectionConfig config;
    config.protocol = "csipaus";
    connection = db->addConnection(config);
    CsipausSettings settings;
    settings.dcapUrl = "https://utility.example.com/dcap";
    db->csipaus[connection] = settings;
    store = std::make_unique<MemoryRunnerStore>(db, connection);
  }

  std::unique_ptr<Adapter> create(const SecretOpener *opener) {
    return makeCsipausFactory(opener)(
        {db->connections.at(connection), *store, abort});
  }

  void giveCredential(const std::string &certPem) {
    db->clientCredentials[connection] = {
        certPem, seal(keys.publicKeyBase64, fixtures::deviceKeyPem)};
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

} // namespace

TEST_F(CsipausFactoryTest, KnowsTheDeviceByItsCertificate) {
  giveCredential(fixtures::deviceCertPem);
  const auto adapter = create(&opener);
  ASSERT_TRUE(adapter);
  EXPECT_EQ(adapter->describeClient(fixtures::deviceLfdi),
            "LFDI C995-EBDD-FCFA-53DE-23DA-8EA2-BFFF-9183-4D51-D617");
}

TEST_F(CsipausFactoryTest, NeedsAClientCertificate) {
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);
}

TEST_F(CsipausFactoryTest, AnUnreadableCertificateIsPermanent) {
  giveCredential("CERT");
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);
}

TEST_F(CsipausFactoryTest, ASealedClientKeyNeedsTheWorkersKey) {
  giveCredential(fixtures::deviceCertPem);
  EXPECT_EQ(failure(nullptr), AdapterError::Kind::Permanent);
}

// IEEE 2030.5, "Cipher suites": "All devices SHALL support the
// TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 cipher suite", which OpenSSL calls
// ECDHE-ECDSA-AES128-CCM8.
TEST(CsipausTls, OffersTheCipherSuiteIeee2030_5RequiresFirst) {
  EXPECT_EQ(std::string(cipherList).rfind("ECDHE-ECDSA-AES128-CCM8:", 0), 0u);
}

TEST_F(CsipausFactoryTest, ReadingsNeedASolarNetworkCredential) {
  giveCredential(fixtures::deviceCertPem);
  TelemetryPoint power;
  power.pointId = "site-power";
  power.source.nodeId = 604;
  power.source.sourceId = "/M/1";
  power.source.property = "watts";
  db->telemetryPoints[connection] = {power};
  db->csipausReadings[connection] = {
      {Mirror::Site, ReadingKind::RealPower, Phase::None, "site-power", 1}};
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);

  db->dataCredentials[connection] = {"data.solarnetwork.net", "token",
                                     seal(keys.publicKeyBase64, "secret")};
  EXPECT_TRUE(create(&opener));
}

TEST_F(CsipausFactoryTest, APowerReadingWithoutAPowerOfTenIsPermanent) {
  giveCredential(fixtures::deviceCertPem);
  TelemetryPoint power;
  power.pointId = "site-power";
  power.source.nodeId = 604;
  power.source.sourceId = "/M/1";
  power.source.property = "watts";
  db->telemetryPoints[connection] = {power};
  db->dataCredentials[connection] = {"data.solarnetwork.net", "token",
                                     seal(keys.publicKeyBase64, "secret")};
  db->csipausReadings[connection] = {
      {Mirror::Site, ReadingKind::RealPower, Phase::None, "site-power"}};
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);

  db->csipausReadings[connection][0].powerOfTen = 1;
  EXPECT_TRUE(create(&opener));
}

TEST_F(CsipausFactoryTest, AReadingFromAPointOfAnotherKindIsPermanent) {
  giveCredential(fixtures::deviceCertPem);
  TelemetryPoint energy;
  energy.pointId = "site-energy";
  energy.item.kind = "energy_real";
  energy.item.units = "Wh";
  energy.source.nodeId = 604;
  energy.source.sourceId = "/M/1";
  energy.source.property = "wattHours";
  db->telemetryPoints[connection] = {energy};
  db->csipausReadings[connection] = {
      {Mirror::Site, ReadingKind::RealPower, Phase::None, "site-energy", 1}};
  db->dataCredentials[connection] = {"data.solarnetwork.net", "token",
                                     seal(keys.publicKeyBase64, "secret")};
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);
}

namespace {

TelemetryPoint statePoint(const std::string &pointId,
                          const std::string &property) {
  TelemetryPoint state;
  state.pointId = pointId;
  state.item.kind = "state";
  state.item.units = "none";
  state.source.nodeId = 604;
  state.source.sourceId = "/DER/INV";
  state.source.property = property;
  return state;
}

DerDescription boundDer(const std::string &connected,
                        const std::string &operating) {
  DerDescription der;
  der.type = 4;
  der.rtgMaxW = 5000;
  der.connectedPointId = connected;
  der.operatingPointId = operating;
  return der;
}

} // namespace

TEST_F(CsipausFactoryTest, BuildsWithTheDersStateBound) {
  giveCredential(fixtures::deviceCertPem);
  db->telemetryPoints[connection] = {statePoint("der-connected", "connected"),
                                     statePoint("der-operating", "operating")};
  db->csipausDer[connection] = boundDer("der-connected", "der-operating");
  db->dataCredentials[connection] = {"data.solarnetwork.net", "token",
                                     seal(keys.publicKeyBase64, "secret")};
  EXPECT_TRUE(create(&opener));
}

TEST_F(CsipausFactoryTest, ABoundStateNeedsASolarNetworkCredential) {
  giveCredential(fixtures::deviceCertPem);
  db->telemetryPoints[connection] = {statePoint("der-connected", "connected"),
                                     statePoint("der-operating", "operating")};
  db->csipausDer[connection] = boundDer("der-connected", "der-operating");
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);
}

TEST_F(CsipausFactoryTest, AStateBindingToAPointOfAnotherKindIsPermanent) {
  giveCredential(fixtures::deviceCertPem);
  TelemetryPoint power;
  power.pointId = "der-power";
  power.source.nodeId = 604;
  power.source.sourceId = "/DER/INV";
  power.source.property = "watts";
  db->telemetryPoints[connection] = {power,
                                     statePoint("der-operating", "operating")};
  db->csipausDer[connection] = boundDer("der-power", "der-operating");
  db->dataCredentials[connection] = {"data.solarnetwork.net", "token",
                                     seal(keys.publicKeyBase64, "secret")};
  EXPECT_EQ(failure(&opener), AdapterError::Kind::Permanent);
}
