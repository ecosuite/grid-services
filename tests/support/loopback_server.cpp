#include "support/loopback_server.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>

namespace grid_services::testing {

namespace {

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return text;
}

} // namespace

LoopbackServer::LoopbackServer(std::string response, std::string body)
    : response_(std::move(response)), body_(std::move(body)) {
  listener_ = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof address);
  listen(listener_, 1);
  socklen_t length = sizeof address;
  getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length);
  port_ = ntohs(address.sin_port);
  thread_ = std::thread([this] { serve(); });
}

LoopbackServer::~LoopbackServer() {
  if (thread_.joinable()) {
    thread_.join();
  }
  close(listener_);
}

std::string LoopbackServer::url(const std::string &path) const {
  return "http://127.0.0.1:" + std::to_string(port_) + path;
}

const LoopbackServer::Received &LoopbackServer::received() {
  if (thread_.joinable()) {
    thread_.join();
  }
  return received_;
}

void LoopbackServer::serve() {
  const int client = accept(listener_, nullptr, nullptr);
  std::string request;
  char buffer[4096];
  auto read = [&] {
    const auto count = recv(client, buffer, sizeof buffer, 0);
    if (count > 0) {
      request.append(buffer, static_cast<size_t>(count));
    }
    return count > 0;
  };
  while (request.find("\r\n\r\n") == std::string::npos && read()) {
  }

  const auto end = request.find("\r\n\r\n");
  const auto requestLine = request.substr(0, request.find("\r\n"));
  const auto space = requestLine.find(' ');
  received_.method = requestLine.substr(0, space);
  received_.target = requestLine.substr(
      space + 1, requestLine.find(' ', space + 1) - space - 1);
  size_t line = request.find("\r\n") + 2;
  while (line < end) {
    const auto next = request.find("\r\n", line);
    const auto header = request.substr(line, next - line);
    const auto colon = header.find(':');
    auto value = header.substr(colon + 1);
    value.erase(0, value.find_first_not_of(' '));
    received_.headers.emplace(lowercase(header.substr(0, colon)), value);
    line = next + 2;
  }

  size_t length = 0;
  if (const auto found = received_.headers.find("content-length");
      found != received_.headers.end()) {
    length = std::stoul(found->second);
  }
  while (request.size() < end + 4 + length && read()) {
  }
  received_.body = request.substr(end + 4, length);

  const std::string answer = "HTTP/1.1 " + response_ + "Content-Length: " +
                             std::to_string(body_.size()) +
                             "\r\nConnection: close\r\n\r\n" + body_;
  send(client, answer.data(), answer.size(), 0);
  close(client);
}

} // namespace grid_services::testing
