#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace grid_services::datum {

// A request to sign with SolarNetwork's SNWS2 scheme: an HMAC-SHA256
// signature over the method, path, sorted query and the host and date
// headers, keyed by a signing key derived from the token secret and the
// day.
struct SignedRequest {
  std::string method = "GET";
  // As sent in the Host header: "data.solarnetwork.net", with a port only
  // when it is not the scheme's default.
  std::string host;
  std::string path;
  std::vector<std::pair<std::string, std::string>> query;
};

struct Snws2Headers {
  // The X-SN-Date header, in RFC 1123 form.
  std::string date;
  std::string authorization;
};

// RFC 3986 percent-encoding of everything but the unreserved characters.
std::string percentEncode(const std::string &value);

// The query string as it goes in the URL and the canonical request:
// sorted by name, percent-encoded.
std::string canonicalQuery(
    const std::vector<std::pair<std::string, std::string>> &query);

// "Wed, 30 Sep 2026 14:05:06 GMT".
std::string rfc1123(std::chrono::system_clock::time_point at);

// Exposed for tests.
std::string canonicalRequest(const SignedRequest &request,
                             std::chrono::system_clock::time_point at);

Snws2Headers sign(const SignedRequest &request, const std::string &token,
                  const std::string &secret,
                  std::chrono::system_clock::time_point at);

} // namespace grid_services::datum
