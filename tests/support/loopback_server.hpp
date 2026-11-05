#pragma once

#include <map>
#include <string>
#include <thread>

namespace grid_services::testing {

// Serves one plain HTTP request on a loopback port with a canned response,
// and keeps the request for the test to inspect.
class LoopbackServer {
public:
  struct Received {
    std::string method;
    std::string target;
    // Values by lowercased name.
    std::multimap<std::string, std::string> headers;
    std::string body;
  };

  // `response` is everything after the status line's "HTTP/1.1 ", e.g.
  // "201 Created\r\nLocation: /edev/3\r\n"; Content-Length, Connection and
  // the blank line are added, with `body` after them.
  explicit LoopbackServer(std::string response = "200 OK\r\n",
                          std::string body = "");
  ~LoopbackServer();

  LoopbackServer(const LoopbackServer &) = delete;
  LoopbackServer &operator=(const LoopbackServer &) = delete;

  // "http://127.0.0.1:<port>" followed by `path`.
  std::string url(const std::string &path = "/") const;

  // The request, once it has been served.
  const Received &received();

private:
  void serve();

  std::string response_;
  std::string body_;
  int listener_ = -1;
  int port_ = 0;
  std::thread thread_;
  Received received_;
};

} // namespace grid_services::testing
