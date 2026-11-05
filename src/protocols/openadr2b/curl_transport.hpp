#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include <oadr/ven/http/IHttp.h>

#include "http/curl_http_client.hpp"

namespace grid_services::openadr2b {

// TLS 1.2 with the OpenADR 2.0b profile's cipher suites (rule 67), and no
// certificates yet.
http::TlsSettings profileTls();

// liboadr's HTTP interface over the worker's HTTP client: POSTs of
// application/xml. Failures are raised as AdapterError.
class CurlTransport : public IHttp {
public:
  CurlTransport(http::TlsSettings tls, std::chrono::seconds connectTimeout,
                std::chrono::seconds requestTimeout,
                const std::atomic<bool> *abort);

  bool post(std::string url, std::string content) override;

  // TLS is configured from the constructor; file paths are not supported.
  void setParameters(std::string, std::string, std::string, bool, std::string,
                     long) override {}

  std::string &getRequestBody() override { return requestBody_; }
  std::string &getResponseBody() override { return responseBody_; }
  std::string &getResponseCode() override { return responseCode_; }
  std::string &getResponseMessage() override { return responseMessage_; }
  time_t getResponseTime() override { return 0; }
  std::string &getServerDate() override { return serverDate_; }

private:
  http::CurlHttpClient client_;

  std::string requestBody_;
  std::string responseBody_;
  std::string responseCode_;
  std::string responseMessage_;
  std::string serverDate_;
};

} // namespace grid_services::openadr2b
