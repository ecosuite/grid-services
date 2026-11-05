#pragma once

#include <string>

namespace grid_services {

// Secrets such as client private keys are stored sealed with the worker's
// public key (libsodium crypto_box_seal), so only the worker can read them.
// Keys and sealed values use standard base64 with padding.

struct KeyPair {
  std::string publicKeyBase64;
  std::string secretKeyBase64;
};

KeyPair generateKeyPair();

std::string seal(const std::string &publicKeyBase64,
                 const std::string &plaintext);

class SecretOpener {
public:
  // Throws std::invalid_argument for a malformed key.
  explicit SecretOpener(const std::string &secretKeyBase64);

  // Throws std::runtime_error when the value is malformed or was sealed
  // for a different key.
  std::string open(const std::string &sealedBase64) const;

  std::string publicKeyBase64() const;

private:
  std::string publicKey_;
  std::string secretKey_;
};

// Must be called once before any other function here.
void initialiseCrypto();

} // namespace grid_services
