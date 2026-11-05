#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "datum/datum.hpp"
#include "datum/snws2.hpp"

namespace grid_services::datum {

struct SolarNetworkCredential {
  // "data.solarnetwork.net", or with a scheme: "http://fake-sn:8080".
  std::string host;
  std::string token;
  std::string secret;
};

// A page of the datum list endpoint.
struct DatumPage {
  std::vector<Datum> datums;
  // Absent when the service left the total out.
  std::optional<long> totalResults;
  long returnedResultCount = 0;
};

// The parts of the client that need no network, exposed for tests.

// "2026-09-30 13:00:00.000Z" or "2026-09-30T13:00:00Z", as SolarNetwork
// writes datum times. Throws DatumError otherwise.
Timestamp parseSolarNetworkTime(const std::string &text);

// The minute-precision UTC form SolarNetwork takes in date parameters.
std::string formatQueryTime(Timestamp at);

// The requests behind `list` and `mostRecent`, without paging. A list is
// widened to whole minutes, as SolarNetwork's date parameters are.
DatumRequest listRequest(NodeId nodeId, const std::string &sourceId,
                         Timestamp start, Timestamp end);
DatumRequest mostRecentRequest(NodeId nodeId, const std::string &sourceId);

// Reads one datum from SolarNetwork's JSON: `created`, then every numeric
// property, whether at the top level or under "i" and "a".
Datum parseDatum(const nlohmann::json &json);

DatumPage parseListResponse(const std::string &body);
std::optional<Datum> parseMostRecentResponse(const std::string &body);

// Reads datums from SolarNetwork over HTTPS, signing each request with
// SNWS2. Failures are raised as DatumError.
class SolarNetworkClient : public DatumReader {
public:
  static constexpr long pageSize = 1000;

  // `clock` dates the signatures; tests replace it. `abort` cuts transfers
  // short when set.
  SolarNetworkClient(
      SolarNetworkCredential credential, std::chrono::seconds connectTimeout,
      std::chrono::seconds requestTimeout, const std::atomic<bool> *abort,
      std::function<std::chrono::system_clock::time_point()> clock = nullptr);
  ~SolarNetworkClient() override;

  SolarNetworkClient(const SolarNetworkClient &) = delete;
  SolarNetworkClient &operator=(const SolarNetworkClient &) = delete;

  std::vector<Datum> list(NodeId nodeId, const std::string &sourceId,
                          Timestamp start, Timestamp end) override;
  std::optional<Datum> mostRecent(NodeId nodeId,
                                  const std::string &sourceId) override;
  DatumRequest describeList(NodeId nodeId, const std::string &sourceId,
                            Timestamp start, Timestamp end) const override;
  DatumRequest describeMostRecent(NodeId nodeId,
                                  const std::string &sourceId) const override;

private:
  struct Response {
    long status = 0;
    std::string body;
  };

  Response get(const SignedRequest &request);

  SolarNetworkCredential credential_;
  std::string scheme_;
  std::string hostHeader_;
  std::chrono::seconds connectTimeout_;
  std::chrono::seconds requestTimeout_;
  const std::atomic<bool> *abort_;
  std::function<std::chrono::system_clock::time_point()> clock_;
};

} // namespace grid_services::datum
