#include "datum/solarnetwork_client.hpp"

#include <cstdio>
#include <ctime>

#include <curl/curl.h>

namespace grid_services::datum {

namespace {

constexpr const char *listPath = "/solarquery/api/v1/sec/datum/list";
constexpr const char *mostRecentPath =
    "/solarquery/api/v1/sec/datum/mostRecent";

size_t appendBody(char *data, size_t size, size_t count, void *target) {
  static_cast<std::string *>(target)->append(data, size * count);
  return size * count;
}

int checkAbort(void *flag, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto *abort = static_cast<const std::atomic<bool> *>(flag);
  return abort != nullptr && abort->load() ? 1 : 0;
}

void check(CURLcode code, const char *what) {
  if (code != CURLE_OK) {
    throw DatumError(std::string("could not set ") + what + ": " +
                     curl_easy_strerror(code));
  }
}

const nlohmann::json *resultsOf(const nlohmann::json &body) {
  if (!body.is_object()) {
    return nullptr;
  }
  if (body.contains("success") && !body["success"].get<bool>()) {
    std::string message = "SolarNetwork answered with success=false";
    if (body.contains("message") && body["message"].is_string()) {
      message += ": " + body["message"].get<std::string>();
    }
    throw DatumError(message);
  }
  const auto data = body.find("data");
  if (data == body.end()) {
    return nullptr;
  }
  if (data->is_array()) {
    return &*data;
  }
  if (data->is_object() && data->contains("results") &&
      (*data)["results"].is_array()) {
    return &(*data)["results"];
  }
  return nullptr;
}

nlohmann::json parseJson(const std::string &body) {
  try {
    return nlohmann::json::parse(body);
  } catch (const std::exception &error) {
    throw DatumError(std::string("SolarNetwork answered with invalid JSON: ") +
                     error.what());
  }
}

void collectNumbers(const nlohmann::json &object, Datum &datum) {
  for (const auto &[key, value] : object.items()) {
    if (value.is_number() && key != "nodeId" && key != "locationId") {
      datum.values[key] = value.get<double>();
    }
  }
}

} // namespace

Timestamp parseSolarNetworkTime(const std::string &text) {
  std::tm parts{};
  char separator = 0;
  const int read = std::sscanf(text.c_str(), "%4d-%2d-%2d%c%2d:%2d:%2d",
                               &parts.tm_year, &parts.tm_mon, &parts.tm_mday,
                               &separator, &parts.tm_hour, &parts.tm_min,
                               &parts.tm_sec);
  if (read < 6 || (separator != ' ' && separator != 'T')) {
    throw DatumError("cannot read datum time \"" + text + "\"");
  }
  parts.tm_year -= 1900;
  parts.tm_mon -= 1;
  return Timestamp(std::chrono::seconds(timegm(&parts)));
}

std::string formatQueryTime(Timestamp at) {
  const std::time_t seconds = at.time_since_epoch().count();
  std::tm parts{};
  gmtime_r(&seconds, &parts);
  char text[64];
  std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d",
                parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                parts.tm_hour, parts.tm_min);
  return text;
}

Datum parseDatum(const nlohmann::json &json) {
  if (!json.is_object() || !json.contains("created") ||
      !json["created"].is_string()) {
    throw DatumError("a datum has no created time");
  }
  Datum datum;
  datum.at = parseSolarNetworkTime(json["created"].get<std::string>());
  collectNumbers(json, datum);
  for (const char *group : {"i", "a"}) {
    if (json.contains(group) && json[group].is_object()) {
      collectNumbers(json[group], datum);
    }
  }
  return datum;
}

DatumPage parseListResponse(const std::string &body) {
  const auto json = parseJson(body);
  const auto *results = resultsOf(json);
  if (results == nullptr) {
    throw DatumError("SolarNetwork answered the datum list without results");
  }
  DatumPage page;
  for (const auto &item : *results) {
    page.datums.push_back(parseDatum(item));
  }
  page.returnedResultCount = static_cast<long>(page.datums.size());
  const auto &data = json["data"];
  if (data.is_object()) {
    if (data.contains("totalResults") && data["totalResults"].is_number()) {
      page.totalResults = data["totalResults"].get<long>();
    }
    if (data.contains("returnedResultCount") &&
        data["returnedResultCount"].is_number()) {
      page.returnedResultCount = data["returnedResultCount"].get<long>();
    }
  }
  return page;
}

std::optional<Datum> parseMostRecentResponse(const std::string &body) {
  const auto json = parseJson(body);
  const auto *results = resultsOf(json);
  if (results == nullptr) {
    throw DatumError("SolarNetwork answered mostRecent without results");
  }
  if (results->empty()) {
    return std::nullopt;
  }
  return parseDatum(results->front());
}

// ---------------------------------------------------------------------------

SolarNetworkClient::SolarNetworkClient(
    SolarNetworkCredential credential, std::chrono::seconds connectTimeout,
    std::chrono::seconds requestTimeout, const std::atomic<bool> *abort,
    std::function<std::chrono::system_clock::time_point()> clock)
    : credential_(std::move(credential)), connectTimeout_(connectTimeout),
      requestTimeout_(requestTimeout), abort_(abort),
      clock_(clock ? std::move(clock)
                   : [] { return std::chrono::system_clock::now(); }) {
  const auto separator = credential_.host.find("://");
  if (separator == std::string::npos) {
    scheme_ = "https";
    hostHeader_ = credential_.host;
  } else {
    scheme_ = credential_.host.substr(0, separator);
    hostHeader_ = credential_.host.substr(separator + 3);
  }
  while (!hostHeader_.empty() && hostHeader_.back() == '/') {
    hostHeader_.pop_back();
  }
  if (hostHeader_.empty()) {
    throw DatumError("the SolarNetwork host is empty");
  }
}

SolarNetworkClient::~SolarNetworkClient() = default;

SolarNetworkClient::Response
SolarNetworkClient::get(const SignedRequest &request) {
  if (abort_ != nullptr && abort_->load()) {
    throw DatumError("the worker is stopping");
  }
  const auto headers =
      sign(request, credential_.token, credential_.secret, clock_());
  const std::string url = scheme_ + "://" + hostHeader_ + request.path + "?" +
                          canonicalQuery(request.query);

  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    throw DatumError("curl_easy_init failed");
  }
  curl_slist *headerList = nullptr;
  headerList = curl_slist_append(headerList, ("X-SN-Date: " + headers.date).c_str());
  headerList = curl_slist_append(
      headerList, ("Authorization: " + headers.authorization).c_str());
  headerList = curl_slist_append(headerList, "Accept: application/json");
  headerList = curl_slist_append(headerList, "Accept-Encoding: gzip");

  Response response;
  char errorBuffer[CURL_ERROR_SIZE] = {};
  CURLcode result = CURLE_OK;
  try {
    check(curl_easy_setopt(curl, CURLOPT_URL, url.c_str()), "URL");
    check(curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer),
          "error buffer");
    check(curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L), "no-signal");
    check(curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L), "GET");
    check(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList), "headers");
    check(curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""), "encoding");
    check(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &appendBody),
          "write function");
    check(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body),
          "write target");
    check(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                           static_cast<long>(connectTimeout_.count())),
          "connect timeout");
    check(curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                           static_cast<long>(requestTimeout_.count())),
          "timeout");
    check(curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L), "progress");
    check(curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &checkAbort),
          "abort check");
    check(curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abort_), "abort flag");
    result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    }
  } catch (...) {
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    throw;
  }
  curl_slist_free_all(headerList);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    std::string message = curl_easy_strerror(result);
    if (errorBuffer[0] != '\0') {
      message += ": ";
      message += errorBuffer;
    }
    throw DatumError("SolarNetwork request failed: " + message);
  }
  if (response.status != 200) {
    throw DatumError("SolarNetwork answered HTTP " +
                     std::to_string(response.status) + " for " + request.path);
  }
  return response;
}

DatumRequest listRequest(NodeId nodeId, const std::string &sourceId,
                         Timestamp start, Timestamp end) {
  const auto endMinutes = std::chrono::ceil<std::chrono::minutes>(end);
  return {listPath,
          {{"nodeId", std::to_string(nodeId)},
           {"sourceId", sourceId},
           {"startDate", formatQueryTime(start)},
           {"endDate", formatQueryTime(Timestamp(endMinutes))}}};
}

DatumRequest mostRecentRequest(NodeId nodeId, const std::string &sourceId) {
  return {mostRecentPath,
          {{"nodeId", std::to_string(nodeId)}, {"sourceId", sourceId}}};
}

namespace {

SignedRequest signedRequest(const std::string &host,
                            const DatumRequest &request) {
  SignedRequest signed_;
  signed_.host = host;
  signed_.path = request.path;
  for (const auto &[key, value] : request.query) {
    signed_.query.emplace_back(key, value);
  }
  return signed_;
}

} // namespace

std::vector<Datum> SolarNetworkClient::list(NodeId nodeId,
                                            const std::string &sourceId,
                                            Timestamp start, Timestamp end) {
  // Date parameters are minute-precision, so the query is widened to whole
  // minutes and trimmed to [start, end) here.
  const auto described = listRequest(nodeId, sourceId, start, end);
  std::vector<Datum> datums;
  for (long offset = 0;; offset += pageSize) {
    SignedRequest request = signedRequest(hostHeader_, described);
    request.query.emplace_back("max", std::to_string(pageSize));
    request.query.emplace_back("offset", std::to_string(offset));
    const auto page = parseListResponse(get(request).body);
    for (const auto &datum : page.datums) {
      if (datum.at >= start && datum.at < end) {
        datums.push_back(datum);
      }
    }
    const bool lastPage =
        page.returnedResultCount < pageSize ||
        (page.totalResults && offset + pageSize >= *page.totalResults);
    if (lastPage || page.datums.empty()) {
      break;
    }
  }
  return datums;
}

std::optional<Datum> SolarNetworkClient::mostRecent(
    NodeId nodeId, const std::string &sourceId) {
  return parseMostRecentResponse(
      get(signedRequest(hostHeader_, mostRecentRequest(nodeId, sourceId)))
          .body);
}

DatumRequest SolarNetworkClient::describeList(NodeId nodeId,
                                              const std::string &sourceId,
                                              Timestamp start,
                                              Timestamp end) const {
  return listRequest(nodeId, sourceId, start, end);
}

DatumRequest
SolarNetworkClient::describeMostRecent(NodeId nodeId,
                                       const std::string &sourceId) const {
  return mostRecentRequest(nodeId, sourceId);
}

} // namespace grid_services::datum
