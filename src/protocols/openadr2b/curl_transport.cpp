#include "protocols/openadr2b/curl_transport.hpp"

namespace grid_services::openadr2b {

http::TlsSettings profileTls() {
  http::TlsSettings tls;
  tls.cipherList = "AES128-SHA256:ECDHE-ECDSA-AES128-SHA256";
  tls.sslVersion = CURL_SSLVERSION_TLSv1_2;
  return tls;
}

CurlTransport::CurlTransport(http::TlsSettings tls,
                             std::chrono::seconds connectTimeout,
                             std::chrono::seconds requestTimeout,
                             const std::atomic<bool> *abort)
    : client_(std::move(tls), connectTimeout, requestTimeout, abort) {}

bool CurlTransport::post(std::string url, std::string content) {
  requestBody_ = std::move(content);
  responseBody_.clear();
  responseCode_.clear();
  responseMessage_.clear();

  http::Request request;
  request.method = http::Method::Post;
  request.url = std::move(url);
  request.body = requestBody_;
  // libcurl would otherwise send "Accept: */*" (OpenADR 2.0b section
  // 9.1.9.1).
  request.headers = {"Content-Type: application/xml",
                     "Accept: application/xml"};

  auto response = client_.send(request);
  responseBody_ = std::move(response.body);
  responseCode_ = std::to_string(response.status);
  return true;
}

} // namespace grid_services::openadr2b
