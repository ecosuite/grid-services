#include "http/http_client.hpp"

#include <algorithm>
#include <cctype>

namespace grid_services::http {

std::string toString(Method method) {
  switch (method) {
  case Method::Get:
    return "GET";
  case Method::Post:
    return "POST";
  case Method::Put:
    return "PUT";
  case Method::Delete:
    return "DELETE";
  }
  return "GET";
}

std::optional<std::string> Response::header(const std::string &name) const {
  std::string key = name;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  const auto found = headers.find(key);
  if (found == headers.end()) {
    return std::nullopt;
  }
  return found->second;
}

} // namespace grid_services::http
