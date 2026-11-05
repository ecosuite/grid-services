#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <oadr/ven/http/IHttp.h>

#include "core/errors.hpp"

namespace grid_services::testing {

// Scripted responses per OpenADR endpoint, shared with the test so it can
// keep scripting and inspecting after the adapter takes ownership of the
// transport.
struct HttpScript {
  struct Response {
    std::string body;
    std::string code = "200";
    std::optional<AdapterError> error;
  };
  struct Request {
    std::string endpoint;
    std::string body;
    std::string url;
  };

  // Keyed by the endpoint's last path segment, e.g. "OadrPoll".
  std::map<std::string, std::deque<Response>> queued;
  // Used when nothing is queued for an endpoint.
  std::map<std::string, Response> defaults;
  std::vector<Request> requests;

  void queue(const std::string &endpoint, std::string body,
             std::string code = "200");
  void queueError(const std::string &endpoint, AdapterError error);
  std::vector<Request> requestsTo(const std::string &endpoint) const;
  // Requests to `endpoint` whose body contains `text`.
  std::vector<Request> requestsContaining(const std::string &endpoint,
                                          const std::string &text) const;
};

class ScriptedHttp : public IHttp {
public:
  explicit ScriptedHttp(std::shared_ptr<HttpScript> script);

  bool post(std::string url, std::string content) override;
  void setParameters(std::string, std::string, std::string, bool, std::string,
                     long) override {}
  std::string &getRequestBody() override { return requestBody_; }
  std::string &getResponseBody() override { return responseBody_; }
  std::string &getResponseCode() override { return responseCode_; }
  std::string &getResponseMessage() override { return responseMessage_; }
  time_t getResponseTime() override { return 0; }
  std::string &getServerDate() override { return serverDate_; }

private:
  std::shared_ptr<HttpScript> script_;
  std::string requestBody_;
  std::string responseBody_;
  std::string responseCode_;
  std::string responseMessage_;
  std::string serverDate_;
};

} // namespace grid_services::testing
