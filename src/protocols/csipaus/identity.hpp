#pragma once

#include <cstdint>
#include <string>

namespace grid_services::csipaus {

// A device's IEEE 2030.5 identifiers, derived from its certificate
// (IEEE 2030.5, "Device credentials").
struct DeviceIdentity {
  // Long-form: the certificate fingerprint left-truncated to 160 bits, as 40
  // upper-case hex digits.
  std::string lfdi;
  // Short-form: the fingerprint left-truncated to 36 bits, in decimal, with a
  // sum-of-digits check digit appended.
  std::uint64_t sfdi = 0;
};

// The identity of the first certificate in `certPem`, whose fingerprint is
// the SHA-256 of its DER encoding. Throws std::invalid_argument when there
// is no PEM certificate.
DeviceIdentity identityFromCertificate(const std::string &certPem);

// The SFDI for an LFDI given as hex digits. Throws std::invalid_argument for
// fewer than nine hex digits.
std::uint64_t sfdiFromLfdi(const std::string &lfdi);

} // namespace grid_services::csipaus
