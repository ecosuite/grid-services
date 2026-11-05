#include "support/scripted_http.hpp"

namespace grid_services::testing {

void HttpScript::queue(const std::string &endpoint, std::string body,
                       std::string code) {
  queued[endpoint].push_back({std::move(body), std::move(code), std::nullopt});
}

void HttpScript::queueError(const std::string &endpoint, AdapterError error) {
  queued[endpoint].push_back({"", "", error});
}

std::vector<HttpScript::Request>
HttpScript::requestsTo(const std::string &endpoint) const {
  std::vector<Request> found;
  for (const auto &request : requests) {
    if (request.endpoint == endpoint) {
      found.push_back(request);
    }
  }
  return found;
}

std::vector<HttpScript::Request>
HttpScript::requestsContaining(const std::string &endpoint,
                               const std::string &text) const {
  std::vector<Request> found;
  for (const auto &request : requestsTo(endpoint)) {
    if (request.body.find(text) != std::string::npos) {
      found.push_back(request);
    }
  }
  return found;
}

ScriptedHttp::ScriptedHttp(std::shared_ptr<HttpScript> script)
    : script_(std::move(script)) {}

bool ScriptedHttp::post(std::string url, std::string content) {
  const auto endpoint = url.substr(url.find_last_of('/') + 1);
  script_->requests.push_back({endpoint, content, url});
  requestBody_ = content;

  HttpScript::Response response;
  auto &queue = script_->queued[endpoint];
  if (!queue.empty()) {
    response = queue.front();
    queue.pop_front();
  } else if (const auto found = script_->defaults.find(endpoint);
             found != script_->defaults.end()) {
    response = found->second;
  } else {
    response = {"", "500", std::nullopt};
  }

  if (response.error) {
    throw *response.error;
  }
  responseBody_ = response.body;
  responseCode_ = response.code;
  return true;
}

} // namespace grid_services::testing
