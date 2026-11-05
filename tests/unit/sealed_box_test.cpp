#include <gtest/gtest.h>

#include "crypto/sealed_box.hpp"

using namespace grid_services;

TEST(SealedBox, RoundTrip) {
  const auto pair = generateKeyPair();
  const std::string key = "-----BEGIN PRIVATE KEY-----\nabc\n-----END PRIVATE KEY-----\n";

  const auto sealed = seal(pair.publicKeyBase64, key);
  EXPECT_NE(sealed.find('='), std::string::npos) << "standard base64 with padding";

  const SecretOpener opener(pair.secretKeyBase64);
  EXPECT_EQ(opener.open(sealed), key);
  EXPECT_EQ(opener.publicKeyBase64(), pair.publicKeyBase64);
}

TEST(SealedBox, OtherKeyCannotOpen) {
  const auto sealed = seal(generateKeyPair().publicKeyBase64, "secret");
  const SecretOpener opener(generateKeyPair().secretKeyBase64);
  EXPECT_THROW(opener.open(sealed), std::runtime_error);
}

TEST(SealedBox, RejectsMalformedInput) {
  EXPECT_THROW(SecretOpener("not base64!"), std::invalid_argument);
  EXPECT_THROW(SecretOpener("c2hvcnQ="), std::invalid_argument);
  const SecretOpener opener(generateKeyPair().secretKeyBase64);
  EXPECT_THROW(opener.open("c2hvcnQ="), std::runtime_error);
}
