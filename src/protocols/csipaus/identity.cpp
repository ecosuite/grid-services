#include "protocols/csipaus/identity.hpp"

#include <array>
#include <stdexcept>
#include <vector>

#include <sodium.h>

namespace grid_services::csipaus {

namespace {

constexpr const char *beginMarker = "-----BEGIN CERTIFICATE-----";
constexpr const char *endMarker = "-----END CERTIFICATE-----";

std::vector<unsigned char> firstCertificateDer(const std::string &pem) {
  const auto begin = pem.find(beginMarker);
  const auto end = pem.find(endMarker);
  if (begin == std::string::npos || end == std::string::npos || end < begin) {
    throw std::invalid_argument("not a PEM certificate");
  }
  const auto bodyStart = begin + std::char_traits<char>::length(beginMarker);
  const std::string body = pem.substr(bodyStart, end - bodyStart);

  std::vector<unsigned char> der(body.size());
  size_t length = 0;
  if (sodium_base642bin(der.data(), der.size(), body.data(), body.size(),
                        " \t\r\n", &length, nullptr,
                        sodium_base64_VARIANT_ORIGINAL) != 0 ||
      length == 0) {
    throw std::invalid_argument("the PEM certificate is not valid base64");
  }
  der.resize(length);
  return der;
}

} // namespace

DeviceIdentity identityFromCertificate(const std::string &certPem) {
  const auto der = firstCertificateDer(certPem);
  std::array<unsigned char, crypto_hash_sha256_BYTES> fingerprint{};
  crypto_hash_sha256(fingerprint.data(), der.data(), der.size());

  static const char digits[] = "0123456789ABCDEF";
  DeviceIdentity identity;
  for (size_t i = 0; i < 20; ++i) {
    identity.lfdi += digits[fingerprint[i] >> 4];
    identity.lfdi += digits[fingerprint[i] & 0x0f];
  }
  identity.sfdi = sfdiFromLfdi(identity.lfdi);
  return identity;
}

std::uint64_t sfdiFromLfdi(const std::string &lfdi) {
  // 36 bits are nine hex digits.
  if (lfdi.size() < 9) {
    throw std::invalid_argument("an LFDI has 40 hex digits");
  }
  std::uint64_t value = 0;
  for (size_t i = 0; i < 9; ++i) {
    const char c = lfdi[i];
    int digit = 0;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      throw std::invalid_argument("an LFDI is hex digits");
    }
    value = value * 16 + static_cast<std::uint64_t>(digit);
  }

  int sum = 0;
  for (auto rest = value; rest > 0; rest /= 10) {
    sum += static_cast<int>(rest % 10);
  }
  // Makes the sum of all the digits a multiple of ten.
  return value * 10 + static_cast<std::uint64_t>((10 - sum % 10) % 10);
}

} // namespace grid_services::csipaus
