#include "http/curl_http_client.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "core/errors.hpp"

namespace grid_services::http {

namespace {

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return text;
}

size_t appendBody(char *data, size_t size, size_t count, void *target) {
  static_cast<Response *>(target)->body.append(data, size * count);
  return size * count;
}

// Called once per header line, including the status line and the blank
// line that ends the headers.
size_t addHeader(char *data, size_t size, size_t count, void *target) {
  auto &headers = static_cast<Response *>(target)->headers;
  std::string line(data, size * count);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  if (line.rfind("HTTP/", 0) == 0) {
    // A new response, such as a final one after "100 Continue".
    headers.clear();
  } else if (const auto colon = line.find(':'); colon != std::string::npos) {
    auto value = line.substr(colon + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    headers.emplace(lowercase(line.substr(0, colon)), value);
  }
  return size * count;
}

int checkAbort(void *flag, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto *abort = static_cast<const std::atomic<bool> *>(flag);
  return abort != nullptr && abort->load() ? 1 : 0;
}

bool isPermanent(CURLcode code) {
  switch (code) {
  case CURLE_UNSUPPORTED_PROTOCOL:
  case CURLE_URL_MALFORMAT:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CIPHER:
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CACERT_BADFILE:
  case CURLE_SSL_ISSUER_ERROR:
  case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
  case CURLE_SSL_CRL_BADFILE:
    return true;
  default:
    return false;
  }
}

void check(CURLcode code, const char *what) {
  if (code != CURLE_OK) {
    throw AdapterError::permanent(std::string("could not set ") + what + ": " +
                                  curl_easy_strerror(code));
  }
}

void setBlob(CURL *curl, CURLoption option, const std::string &pem,
             const char *what) {
  curl_blob blob{const_cast<char *>(pem.data()), pem.size(),
                 CURL_BLOB_COPY};
  check(curl_easy_setopt(curl, option, &blob), what);
}

} // namespace

CurlHttpClient::CurlHttpClient(TlsSettings tls,
                               std::chrono::seconds connectTimeout,
                               std::chrono::seconds requestTimeout,
                               const std::atomic<bool> *abort)
    : tls_(std::move(tls)), connectTimeout_(connectTimeout),
      requestTimeout_(requestTimeout), abort_(abort) {
  curl_ = curl_easy_init();
  if (curl_ == nullptr) {
    throw std::runtime_error("curl_easy_init failed");
  }
}

CurlHttpClient::~CurlHttpClient() {
  curl_slist_free_all(headers_);
  curl_easy_cleanup(curl_);
}

void CurlHttpClient::configure(const Request &request, Response &response) {
  curl_easy_reset(curl_);
  errorBuffer_[0] = '\0';

  curl_slist_free_all(headers_);
  headers_ = nullptr;
  for (const auto &header : request.headers) {
    headers_ = curl_slist_append(headers_, header.c_str());
  }
  // No "Expect: 100-continue" round trip before a body.
  headers_ = curl_slist_append(headers_, "Expect:");

  check(curl_easy_setopt(curl_, CURLOPT_URL, request.url.c_str()), "URL");
  check(curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, errorBuffer_),
        "error buffer");
  check(curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L), "no-signal");
  switch (request.method) {
  case Method::Get:
    check(curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L), "GET");
    break;
  case Method::Delete:
    check(curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE"), "DELETE");
    break;
  case Method::Post:
  case Method::Put:
    check(curl_easy_setopt(curl_, CURLOPT_POST, 1L), "POST");
    if (request.method == Method::Put) {
      check(curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT"), "PUT");
    }
    check(curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, request.body.data()),
          "body");
    check(curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE,
                           static_cast<curl_off_t>(request.body.size())),
          "body size");
    break;
  }
  check(curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_), "headers");
  check(curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &appendBody),
        "write function");
  check(curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response), "write target");
  check(curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &addHeader),
        "header function");
  check(curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &response),
        "header target");
  check(curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT,
                         static_cast<long>(connectTimeout_.count())),
        "connect timeout");
  check(curl_easy_setopt(curl_, CURLOPT_TIMEOUT,
                         static_cast<long>(requestTimeout_.count())),
        "timeout");
  check(curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L), "progress");
  check(curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, &checkAbort),
        "abort check");
  check(curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, abort_), "abort flag");

  if (request.url.rfind("https://", 0) == 0) {
    configureTls();
  }
}

void CurlHttpClient::configureTls() {
  const long verify = tls_.verifyServer ? 1L : 0L;
  check(curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, verify), "verify peer");
  check(curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, verify * 2L),
        "verify host");
  check(curl_easy_setopt(curl_, CURLOPT_SSLVERSION, tls_.sslVersion),
        "TLS version");
  if (!tls_.cipherList.empty()) {
    check(curl_easy_setopt(curl_, CURLOPT_SSL_CIPHER_LIST,
                           tls_.cipherList.c_str()),
          "cipher list");
  }
  if (!tls_.clientCertPem.empty()) {
    check(curl_easy_setopt(curl_, CURLOPT_SSLCERTTYPE, "PEM"),
          "certificate type");
    setBlob(curl_, CURLOPT_SSLCERT_BLOB, tls_.clientCertPem,
            "client certificate");
  }
  if (!tls_.clientKeyPem.empty()) {
    check(curl_easy_setopt(curl_, CURLOPT_SSLKEYTYPE, "PEM"), "key type");
    setBlob(curl_, CURLOPT_SSLKEY_BLOB, tls_.clientKeyPem, "client key");
  }
  if (!tls_.caCertPem.empty()) {
    setBlob(curl_, CURLOPT_CAINFO_BLOB, tls_.caCertPem, "CA certificate");
  }
}

Response CurlHttpClient::send(const Request &request) {
  if (abort_ != nullptr && abort_->load()) {
    throw AdapterError::stopped();
  }

  Response response;
  configure(request, response);

  const CURLcode result = curl_easy_perform(curl_);
  if (result == CURLE_ABORTED_BY_CALLBACK) {
    throw AdapterError::stopped();
  }
  if (result != CURLE_OK) {
    std::string message = curl_easy_strerror(result);
    if (errorBuffer_[0] != '\0') {
      message += ": ";
      message += errorBuffer_;
    }
    throw isPermanent(result) ? AdapterError::permanent(message)
                              : AdapterError::transient(message);
  }

  curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response.status);
  return response;
}

} // namespace grid_services::http
