#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/errors.hpp"
#include "http/http_client.hpp"

namespace grid_services::testing {

// Scripted responses for a REST server, shared with the test so it can keep
// scripting and inspecting after the adapter takes ownership of the client.
// Requests are keyed by method and path without the query, e.g.
// "GET /edev". A request nothing is scripted for gets a 404.
struct RestScript {
  struct Response {
    long status = 200;
    std::string body;
    std::map<std::string, std::string> headers;
    std::optional<AdapterError> error;
  };
  struct Request {
    std::string key;
    // The path with its query, e.g. "/edev?s=0&l=100".
    std::string target;
    std::string body;
    std::vector<std::string> headers;
  };

  std::map<std::string, std::deque<Response>> queued;
  std::map<std::string, Response> defaults;
  std::vector<Request> requests;

  // Answers every `key` request with `body`.
  void on(const std::string &key, std::string body, long status = 200);
  // Answers the next `key` request with `response`, before any default.
  void queue(const std::string &key, Response response);
  std::vector<Request> requestsTo(const std::string &key) const;
  std::size_t count(const std::string &key) const;
};

class ScriptedRest : public http::HttpClient {
public:
  explicit ScriptedRest(std::shared_ptr<RestScript> script)
      : script_(std::move(script)) {}

  http::Response send(const http::Request &request) override;

private:
  std::shared_ptr<RestScript> script_;
};

} // namespace grid_services::testing
