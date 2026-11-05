#include "crypto/sealed_box.hpp"

#include <stdexcept>
#include <vector>

#include <sodium.h>

namespace grid_services {

namespace {

constexpr int base64Variant = sodium_base64_VARIANT_ORIGINAL;

std::string toBase64(const unsigned char *data, std::size_t length) {
  std::string encoded(sodium_base64_encoded_len(length, base64Variant), '\0');
  sodium_bin2base64(encoded.data(), encoded.size(), data, length,
                    base64Variant);
  encoded.resize(encoded.find('\0'));
  return encoded;
}

std::vector<unsigned char> fromBase64(const std::string &encoded,
                                      const char *what) {
  std::vector<unsigned char> decoded(encoded.size());
  std::size_t length = 0;
  if (sodium_base642bin(decoded.data(), decoded.size(), encoded.data(),
                        encoded.size(), " \n\r\t", &length, nullptr,
                        base64Variant) != 0) {
    throw std::invalid_argument(std::string(what) + " is not valid base64");
  }
  decoded.resize(length);
  return decoded;
}

std::string bytes(const std::vector<unsigned char> &data) {
  return {data.begin(), data.end()};
}

} // namespace

void initialiseCrypto() {
  if (sodium_init() < 0) {
    throw std::runtime_error("libsodium failed to initialise");
  }
}

KeyPair generateKeyPair() {
  unsigned char publicKey[crypto_box_PUBLICKEYBYTES];
  unsigned char secretKey[crypto_box_SECRETKEYBYTES];
  crypto_box_keypair(publicKey, secretKey);
  KeyPair pair{toBase64(publicKey, sizeof publicKey),
               toBase64(secretKey, sizeof secretKey)};
  sodium_memzero(secretKey, sizeof secretKey);
  return pair;
}

std::string seal(const std::string &publicKeyBase64,
                 const std::string &plaintext) {
  const auto publicKey = fromBase64(publicKeyBase64, "public key");
  if (publicKey.size() != crypto_box_PUBLICKEYBYTES) {
    throw std::invalid_argument("public key has the wrong length");
  }
  std::vector<unsigned char> sealed(crypto_box_SEALBYTES + plaintext.size());
  crypto_box_seal(sealed.data(),
                  reinterpret_cast<const unsigned char *>(plaintext.data()),
                  plaintext.size(), publicKey.data());
  return toBase64(sealed.data(), sealed.size());
}

SecretOpener::SecretOpener(const std::string &secretKeyBase64) {
  const auto secretKey = fromBase64(secretKeyBase64, "secret key");
  if (secretKey.size() != crypto_box_SECRETKEYBYTES) {
    throw std::invalid_argument("secret key has the wrong length");
  }
  std::vector<unsigned char> publicKey(crypto_box_PUBLICKEYBYTES);
  crypto_scalarmult_base(publicKey.data(), secretKey.data());
  secretKey_ = bytes(secretKey);
  publicKey_ = bytes(publicKey);
}

std::string SecretOpener::open(const std::string &sealedBase64) const {
  const auto sealed = fromBase64(sealedBase64, "sealed value");
  if (sealed.size() < crypto_box_SEALBYTES) {
    throw std::runtime_error("sealed value is too short");
  }
  std::string plaintext(sealed.size() - crypto_box_SEALBYTES, '\0');
  if (crypto_box_seal_open(
          reinterpret_cast<unsigned char *>(plaintext.data()), sealed.data(),
          sealed.size(),
          reinterpret_cast<const unsigned char *>(publicKey_.data()),
          reinterpret_cast<const unsigned char *>(secretKey_.data())) != 0) {
    throw std::runtime_error(
        "sealed value could not be opened with the worker's key");
  }
  return plaintext;
}

std::string SecretOpener::publicKeyBase64() const {
  return toBase64(reinterpret_cast<const unsigned char *>(publicKey_.data()),
                  publicKey_.size());
}

} // namespace grid_services
