#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace grid_services::http {

enum class Method { Get, Post, Put, Delete };

std::string toString(Method method);

struct Request {
  Method method = Method::Get;
  std::string url;
  std::string body;
  // Each "Name: value".
  std::vector<std::string> headers;
};

struct Response {
  long status = 0;
  // By lowercased name.
  std::multimap<std::string, std::string> headers;
  std::string body;

  // The first value of the header with this name, in any case.
  std::optional<std::string> header(const std::string &name) const;
  bool ok() const { return status >= 200 && status < 300; }
};

// One HTTP exchange at a time. A response with any status is returned; a
// request that gets no response throws AdapterError: permanent for problems
// only configuration can fix, such as a rejected certificate, transient for
// the rest, and stopped when the transfer was aborted.
class HttpClient {
public:
  virtual ~HttpClient() = default;

  virtual Response send(const Request &request) = 0;
};

} // namespace grid_services::http
