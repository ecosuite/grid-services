#include <gtest/gtest.h>

#include "datum/snws2.hpp"

using namespace grid_services::datum;

namespace {

// The vector was produced with solarnetwork-api-core's
// AuthorizationV2Builder for the same request, token and secret.
const auto at = std::chrono::sys_seconds(std::chrono::seconds(1790777106));

SignedRequest listRequest() {
  SignedRequest request;
  request.host = "data.solarnetwork.net";
  request.path = "/solarquery/api/v1/sec/datum/list";
  request.query = {{"nodeId", "604"},
                   {"sourceId", "/LN/RC/S1/MET/1"},
                   {"startDate", "2026-09-30T13:00"},
                   {"endDate", "2026-09-30T14:00"},
                   {"aggregation", "FiveMinute"}};
  return request;
}

} // namespace

TEST(Snws2, FormatsTheDateHeader) {
  EXPECT_EQ(rfc1123(at), "Wed, 30 Sep 2026 14:05:06 GMT");
}

TEST(Snws2, EncodesQueryParametersSortedAndEscaped) {
  EXPECT_EQ(canonicalQuery(listRequest().query),
            "aggregation=FiveMinute&endDate=2026-09-30T14%3A00&nodeId=604&"
            "sourceId=%2FLN%2FRC%2FS1%2FMET%2F1&startDate=2026-09-30T13%3A00");
  EXPECT_EQ(percentEncode("a b!*'()~"), "a%20b%21%2A%27%28%29~");
}

TEST(Snws2, BuildsTheCanonicalRequest) {
  EXPECT_EQ(
      canonicalRequest(listRequest(), at),
      "GET\n/solarquery/api/v1/sec/datum/list\n"
      "aggregation=FiveMinute&endDate=2026-09-30T14%3A00&nodeId=604&"
      "sourceId=%2FLN%2FRC%2FS1%2FMET%2F1&startDate=2026-09-30T13%3A00\n"
      "host:data.solarnetwork.net\n"
      "x-sn-date:Wed, 30 Sep 2026 14:05:06 GMT\n"
      "host;x-sn-date\n"
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Snws2, SignsLikeTheReferenceLibrary) {
  const auto headers = sign(listRequest(), "test-token", "test-secret", at);
  EXPECT_EQ(headers.date, "Wed, 30 Sep 2026 14:05:06 GMT");
  EXPECT_EQ(headers.authorization,
            "SNWS2 Credential=test-token,SignedHeaders=host;x-sn-date,"
            "Signature="
            "8571ce23bffcba729f2e1244bdc771e44a22e9b0d9856f71f65ad2aeb4a3afb5");
}
