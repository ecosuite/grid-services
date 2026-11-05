#include <gtest/gtest.h>

#include "protocols/openadr2b/curl_transport.hpp"
#include "support/loopback_server.hpp"

using namespace grid_services::openadr2b;
using grid_services::testing::LoopbackServer;

// ---------------------------------------------------------------------------
// OpenADR 2.0b Profile Specification v1.1, section 11.2 conformance rules.

// Rule 67: "VTN and VEN MUST support TLS 1.2. ... The VEN client MUST offer at
// least one of the default cipher suites listed below. ...
// TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, TLS_RSA_WITH_AES_128_CBC_SHA256".
// OpenSSL names them ECDHE-ECDSA-AES128-SHA256 and AES128-SHA256.
TEST(CurlTransportConformance, Rule67_DefaultsToTls12WithTheProfileCiphers) {
  const auto tls = profileTls();
  EXPECT_EQ(tls.sslVersion, CURL_SSLVERSION_TLSv1_2);
  EXPECT_NE(tls.cipherList.find("ECDHE-ECDSA-AES128-SHA256"),
            std::string::npos);
  EXPECT_NE(tls.cipherList.find("AES128-SHA256"), std::string::npos);
}

// Section 9.1.9.1: "Since responses are always 'application/xml', the Accept
// header may be omitted. However, if it is included, the value of the Accept
// request header must always be 'application/xml'."
// Rule 53 and sections 9.1.9.4 to 9.1.9.6: requests carry Host,
// Content-Length and a Content-Type of application/xml, and are not chunked.
TEST(CurlTransportConformance, Section9_1_9_RequestHeaders) {
  LoopbackServer server;
  CurlTransport transport({}, std::chrono::seconds(5), std::chrono::seconds(5),
                          nullptr);
  const std::string body = "<oadr:oadrPayload/>";
  ASSERT_TRUE(
      transport.post(server.url("/OpenADR2/Simple/2.0b/OadrPoll"), body));
  EXPECT_EQ(transport.getResponseCode(), "200");

  const auto &headers = server.received().headers;
  for (auto [it, end] = headers.equal_range("accept"); it != end; ++it) {
    EXPECT_EQ(it->second, "application/xml");
  }
  EXPECT_EQ(headers.count("host"), 1u);
  ASSERT_EQ(headers.count("content-length"), 1u);
  EXPECT_EQ(headers.find("content-length")->second,
            std::to_string(body.size()));
  ASSERT_EQ(headers.count("content-type"), 1u);
  EXPECT_EQ(headers.find("content-type")->second, "application/xml");
  EXPECT_EQ(headers.count("transfer-encoding"), 0u);
}
