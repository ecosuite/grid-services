#include <gtest/gtest.h>

#include <stdexcept>

#include "protocols/csipaus/identity.hpp"
#include "support/csipaus_fixtures.hpp"

using namespace grid_services::csipaus;
namespace fixtures = grid_services::testing::csipaus_fixtures;

// IEEE 2030.5's SFDI example: fingerprint 3E4F-45AB-3... left-truncated to
// 36 bits is 16726121139, and its check digit is 1.
TEST(CsipausIdentity, SfdiFollowsTheStandardsExample) {
  EXPECT_EQ(sfdiFromLfdi("3E4F45AB31EDFE5B67E343E5E4562E31984E23E5"),
            167261211391ull);
  EXPECT_EQ(sfdiFromLfdi("3e4f45ab31edfe5b67e343e5e4562e31984e23e5"),
            167261211391ull);
}

// The expected values come from `openssl x509 -outform der | sha256sum` on
// the fixture certificate.
TEST(CsipausIdentity, DerivesTheIdentityFromTheCertificate) {
  const auto identity = identityFromCertificate(fixtures::deviceCertPem);
  EXPECT_EQ(identity.lfdi, "C995EBDDFCFA53DE23DA8EA2BFFF91834D51D617");
  EXPECT_EQ(identity.sfdi, 541127305912ull);
}

TEST(CsipausIdentity, UsesTheFirstCertificateOfAChain) {
  const auto chain = std::string(fixtures::deviceCertPem) +
                     "-----BEGIN CERTIFICATE-----\nAAAA\n"
                     "-----END CERTIFICATE-----\n";
  EXPECT_EQ(identityFromCertificate(chain).lfdi,
            "C995EBDDFCFA53DE23DA8EA2BFFF91834D51D617");
}

TEST(CsipausIdentity, RefusesWhatIsNotACertificate) {
  EXPECT_THROW(identityFromCertificate("not a certificate"),
               std::invalid_argument);
  EXPECT_THROW(identityFromCertificate("-----BEGIN CERTIFICATE-----\n!!\n"
                                       "-----END CERTIFICATE-----\n"),
               std::invalid_argument);
  EXPECT_THROW(sfdiFromLfdi("3E4F"), std::invalid_argument);
  EXPECT_THROW(sfdiFromLfdi("3E4F45AZ31"), std::invalid_argument);
}
