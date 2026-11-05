#include "support/scripted_rest.hpp"

#include <cctype>

namespace grid_services::testing {

void RestScript::on(const std::string &key, std::string body, long status) {
  defaults[key] = Response{status, std::move(body), {}, std::nullopt};
}

void RestScript::queue(const std::string &key, Response response) {
  queued[key].push_back(std::move(response));
}

std::vector<RestScript::Request>
RestScript::requestsTo(const std::string &key) const {
  std::vector<Request> matching;
  for (const auto &request : requests) {
    if (request.key == key) {
      matching.push_back(request);
    }
  }
  return matching;
}

std::size_t RestScript::count(const std::string &key) const {
  return requestsTo(key).size();
}

http::Response ScriptedRest::send(const http::Request &request) {
  const auto scheme = request.url.find("://");
  const auto pathStart = request.url.find('/', scheme + 3);
  const auto target = pathStart == std::string::npos
                          ? std::string("/")
                          : request.url.substr(pathStart);
  const auto key =
      http::toString(request.method) + " " + target.substr(0, target.find('?'));
  script_->requests.push_back({key, target, request.body, request.headers});

  RestScript::Response scripted{404, "", {}, std::nullopt};
  if (auto queued = script_->queued.find(key);
      queued != script_->queued.end() && !queued->second.empty()) {
    scripted = std::move(queued->second.front());
    queued->second.pop_front();
  } else if (auto fallback = script_->defaults.find(key);
             fallback != script_->defaults.end()) {
    scripted = fallback->second;
  }
  if (scripted.error) {
    throw *scripted.error;
  }

  http::Response response;
  response.status = scripted.status;
  response.body = scripted.body;
  for (const auto &[name, value] : scripted.headers) {
    std::string lower = name;
    for (auto &c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    response.headers.emplace(lower, value);
  }
  return response;
}

} // namespace grid_services::testing
