#include "datum/snws2.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>

#include <sodium.h>

namespace grid_services::datum {

namespace {

using Digest = std::array<unsigned char, crypto_hash_sha256_BYTES>;

Digest sha256(const std::string &data) {
  Digest digest;
  crypto_hash_sha256(digest.data(),
                     reinterpret_cast<const unsigned char *>(data.data()),
                     data.size());
  return digest;
}

Digest hmacSha256(const unsigned char *key, size_t keyLength,
                  const std::string &data) {
  Digest digest;
  crypto_auth_hmacsha256_state state;
  crypto_auth_hmacsha256_init(&state, key, keyLength);
  crypto_auth_hmacsha256_update(
      &state, reinterpret_cast<const unsigned char *>(data.data()),
      data.size());
  crypto_auth_hmacsha256_final(&state, digest.data());
  return digest;
}

Digest hmacSha256(const std::string &key, const std::string &data) {
  return hmacSha256(reinterpret_cast<const unsigned char *>(key.data()),
                    key.size(), data);
}

std::string hex(const Digest &digest) {
  static const char *digits = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 0x0f]);
  }
  return text;
}

std::tm utc(std::chrono::system_clock::time_point at) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
  std::tm parts{};
  gmtime_r(&seconds, &parts);
  return parts;
}

// "20260930" or "20260930T140506Z".
std::string compactDate(std::chrono::system_clock::time_point at,
                        bool withTime) {
  const auto parts = utc(at);
  char text[64];
  if (withTime) {
    std::snprintf(text, sizeof(text), "%04d%02d%02dT%02d%02d%02dZ",
                  parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                  parts.tm_hour, parts.tm_min, parts.tm_sec);
  } else {
    std::snprintf(text, sizeof(text), "%04d%02d%02d", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday);
  }
  return text;
}

} // namespace

std::string percentEncode(const std::string &value) {
  static const char *digits = "0123456789ABCDEF";
  std::string encoded;
  for (const unsigned char c : value) {
    const bool unreserved = std::isalnum(c) || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved) {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(digits[c >> 4]);
      encoded.push_back(digits[c & 0x0f]);
    }
  }
  return encoded;
}

std::string canonicalQuery(
    const std::vector<std::pair<std::string, std::string>> &query) {
  auto sorted = query;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  std::string text;
  for (const auto &[name, value] : sorted) {
    if (!text.empty()) {
      text.push_back('&');
    }
    text += percentEncode(name) + "=" + percentEncode(value);
  }
  return text;
}

std::string rfc1123(std::chrono::system_clock::time_point at) {
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const auto parts = utc(at);
  char text[64];
  std::snprintf(text, sizeof(text), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                days[parts.tm_wday], parts.tm_mday, months[parts.tm_mon],
                parts.tm_year + 1900, parts.tm_hour, parts.tm_min,
                parts.tm_sec);
  return text;
}

std::string canonicalRequest(const SignedRequest &request,
                             std::chrono::system_clock::time_point at) {
  return request.method + "\n" + request.path + "\n" +
         canonicalQuery(request.query) + "\n" + "host:" + request.host + "\n" +
         "x-sn-date:" + rfc1123(at) + "\n" + "host;x-sn-date\n" +
         hex(sha256(""));
}

Snws2Headers sign(const SignedRequest &request, const std::string &token,
                  const std::string &secret,
                  std::chrono::system_clock::time_point at) {
  const auto dayKey = hmacSha256("SNWS2" + secret, compactDate(at, false));
  const auto signingKey =
      hmacSha256(dayKey.data(), dayKey.size(), "snws2_request");
  const std::string toSign = "SNWS2-HMAC-SHA256\n" + compactDate(at, true) +
                             "\n" + hex(sha256(canonicalRequest(request, at)));
  const auto signature =
      hmacSha256(signingKey.data(), signingKey.size(), toSign);
  return {rfc1123(at), "SNWS2 Credential=" + token +
                           ",SignedHeaders=host;x-sn-date,Signature=" +
                           hex(signature)};
}

} // namespace grid_services::datum
