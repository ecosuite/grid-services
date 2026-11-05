#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "datum/datum.hpp"

namespace grid_services::testing {

// A datum reader over datums a test puts in, per stream. Set `failure` to
// make every read fail with that message.
class FakeDatumReader : public datum::DatumReader {
public:
  using Stream = std::pair<datum::NodeId, std::string>;

  void add(datum::NodeId nodeId, const std::string &sourceId,
           datum::Datum datum) {
    streams_[{nodeId, sourceId}].push_back(std::move(datum));
  }

  std::vector<datum::Datum> list(datum::NodeId nodeId,
                                 const std::string &sourceId,
                                 datum::Timestamp start,
                                 datum::Timestamp end) override {
    throwIfFailing();
    listCalls.push_back({nodeId, sourceId, start, end});
    std::vector<datum::Datum> found;
    for (const auto &datum : streams_[{nodeId, sourceId}]) {
      if (datum.at >= start && datum.at < end) {
        found.push_back(datum);
      }
    }
    return found;
  }

  std::optional<datum::Datum> mostRecent(datum::NodeId nodeId,
                                         const std::string &sourceId) override {
    throwIfFailing();
    const auto &datums = streams_[{nodeId, sourceId}];
    if (datums.empty()) {
      return std::nullopt;
    }
    return *std::max_element(
        datums.begin(), datums.end(),
        [](const auto &a, const auto &b) { return a.at < b.at; });
  }

  // Described as a service would, so the record of a read can be checked.
  datum::DatumRequest describeList(datum::NodeId nodeId,
                                   const std::string &sourceId,
                                   datum::Timestamp start,
                                   datum::Timestamp end) const override {
    return {"/fake/list",
            {{"nodeId", std::to_string(nodeId)},
             {"sourceId", sourceId},
             {"start", std::to_string(start.time_since_epoch().count())},
             {"end", std::to_string(end.time_since_epoch().count())}}};
  }

  datum::DatumRequest
  describeMostRecent(datum::NodeId nodeId,
                     const std::string &sourceId) const override {
    return {"/fake/mostRecent",
            {{"nodeId", std::to_string(nodeId)}, {"sourceId", sourceId}}};
  }

  struct ListCall {
    datum::NodeId nodeId;
    std::string sourceId;
    datum::Timestamp start;
    datum::Timestamp end;
  };

  std::optional<std::string> failure;
  std::vector<ListCall> listCalls;

private:
  void throwIfFailing() const {
    if (failure) {
      throw datum::DatumError(*failure);
    }
  }

  std::map<Stream, std::vector<datum::Datum>> streams_;
};

} // namespace grid_services::testing
