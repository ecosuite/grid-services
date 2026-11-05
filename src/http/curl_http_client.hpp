#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include <curl/curl.h>

#include "http/http_client.hpp"

namespace grid_services::http {

struct TlsSettings {
  std::string clientCertPem;
  std::string clientKeyPem;
  // Trusted in place of the system's CAs when set.
  std::string caCertPem;
  bool verifyServer = true;
  // OpenSSL cipher names; empty leaves libcurl's default.
  std::string cipherList;
  long sslVersion = CURL_SSLVERSION_TLSv1_2;
};

// HttpClient over libcurl. Certificates come from memory, and a transfer
// aborts as soon as `abort` is set. Redirects are not followed.
class CurlHttpClient : public HttpClient {
public:
  CurlHttpClient(TlsSettings tls, std::chrono::seconds connectTimeout,
                 std::chrono::seconds requestTimeout,
                 const std::atomic<bool> *abort);
  ~CurlHttpClient() override;

  CurlHttpClient(const CurlHttpClient &) = delete;
  CurlHttpClient &operator=(const CurlHttpClient &) = delete;

  Response send(const Request &request) override;

private:
  void configure(const Request &request, Response &response);
  void configureTls();

  TlsSettings tls_;
  std::chrono::seconds connectTimeout_;
  std::chrono::seconds requestTimeout_;
  const std::atomic<bool> *abort_;
  CURL *curl_ = nullptr;
  curl_slist *headers_ = nullptr;
  char errorBuffer_[CURL_ERROR_SIZE] = {};
};

} // namespace grid_services::http
