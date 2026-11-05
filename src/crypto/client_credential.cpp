#include "crypto/client_credential.hpp"

#include <exception>

#include "core/errors.hpp"
#include "crypto/sealed_box.hpp"

namespace grid_services {

OpenedCredential openClientCredential(const ClientCredential &credential,
                                      const SecretOpener *opener) {
  if (opener == nullptr) {
    throw AdapterError::permanent(
        "the client key is sealed but the worker has no secret key "
        "(GRID_SERVICES_SECRET_KEY)");
  }
  try {
    return {credential.certPem, opener->open(credential.keySealed)};
  } catch (const std::exception &error) {
    throw AdapterError::permanent(std::string("cannot open the client key: ") +
                                  error.what());
  }
}

OpenedDataCredential openDataCredential(const DataCredential &credential,
                                        const SecretOpener *opener) {
  if (opener == nullptr) {
    throw AdapterError::permanent(
        "the SolarNetwork secret is sealed but the worker has no secret key "
        "(GRID_SERVICES_SECRET_KEY)");
  }
  try {
    return {credential.host, credential.token,
            opener->open(credential.secretSealed)};
  } catch (const std::exception &error) {
    throw AdapterError::permanent(
        std::string("cannot open the SolarNetwork secret: ") + error.what());
  }
}

} // namespace grid_services
