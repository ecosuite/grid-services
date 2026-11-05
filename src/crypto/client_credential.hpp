#pragma once

#include <string>

#include "core/telemetry.hpp"

namespace grid_services {

class SecretOpener;

struct OpenedCredential {
  std::string certPem;
  std::string keyPem;
};

// A connection's client certificate with its private key opened, for TLS.
// Throws AdapterError::permanent when the key cannot be opened, including
// when the worker has no secret key (`opener` is null).
OpenedCredential openClientCredential(const ClientCredential &credential,
                                      const SecretOpener *opener);

struct OpenedDataCredential {
  std::string host;
  std::string token;
  std::string secret;
};

// A connection's SolarNetwork token with its secret opened. Throws
// AdapterError::permanent when the secret cannot be opened, including when
// the worker has no secret key (`opener` is null).
OpenedDataCredential openDataCredential(const DataCredential &credential,
                                        const SecretOpener *opener);

} // namespace grid_services
