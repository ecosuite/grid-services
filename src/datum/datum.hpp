#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace grid_services::datum {

using Timestamp = std::chrono::sys_seconds;
using NodeId = std::int64_t;

// One SolarNetwork datum: a stream's numeric properties at one time. Status
// properties are not carried; reports only take numbers.
struct Datum {
  Timestamp at{};
  std::map<std::string, double> values;
};

// Raised when datums cannot be read: the service is unreachable, refused the
// request, or answered with something other than datums.
class DatumError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// A read of the datum service, as it could be made again: the request path
// and its query, without paging. Recorded with every report so the datums
// behind a value can be fetched afresh.
struct DatumRequest {
  std::string path;
  std::map<std::string, std::string> query;
};

// Where datums come from. The runner's report service reads through this;
// tests replace it.
class DatumReader {
public:
  virtual ~DatumReader() = default;

  // Every datum of the stream with `at` in [start, end), oldest first.
  virtual std::vector<Datum> list(NodeId nodeId, const std::string &sourceId,
                                  Timestamp start, Timestamp end) = 0;

  // The stream's latest datum, or nothing when it has none.
  virtual std::optional<Datum> mostRecent(NodeId nodeId,
                                          const std::string &sourceId) = 0;

  // The requests `list` and `mostRecent` make, for the record. A reader
  // that is not a service describes nothing.
  virtual DatumRequest describeList(NodeId, const std::string &, Timestamp,
                                    Timestamp) const {
    return {};
  }
  virtual DatumRequest describeMostRecent(NodeId, const std::string &) const {
    return {};
  }
};

// How a stream property behaves over time.
//   Instantaneous  a sampled value, such as watts: averaged over a window
//   Accumulating   a meter reading, such as wattHours: differenced over a
//                  window
enum class PropertyClass { Instantaneous, Accumulating };

// How a window of datums becomes one value.
enum class Reduce { Average, Min, Max, Difference, Last };

PropertyClass parsePropertyClass(const std::string &value);
Reduce parseReduce(const std::string &value);
std::string toString(PropertyClass value);
std::string toString(Reduce value);

// The reduce a property class takes when none is chosen.
Reduce defaultReduce(PropertyClass propertyClass);

// One data point's source: a stream property and how to read it.
struct DatumSource {
  NodeId nodeId = 0;
  std::string sourceId;
  std::string property;
  PropertyClass propertyClass = PropertyClass::Instantaneous;
  Reduce reduce = Reduce::Average;
  // Sign and unit scaling applied to the reduced value.
  double multiplier = 1;
  // A latest value older than this is reported as stale.
  std::chrono::seconds staleAfter{600};
};

// How good a reported value is. Protocols translate it into their own terms.
enum class Quality {
  Good,
  NoValue,   // nothing in the window
  Stale,     // the latest value is older than the source allows
  Unreadable // datums could not be read
};

// "good", "no_value", "stale" or "unreadable", as deliveries record it.
std::string toString(Quality value);

struct Sample {
  // Absent when there is nothing to report; the report then carries a
  // placeholder with the quality saying why.
  std::optional<double> value;
  Quality quality = Quality::NoValue;
};

// Reduces the datums with `at` in [start, end) to one value, per the
// source's reduce. `datums` is oldest first.
//
// A difference is the reading at the end of the window less the reading at
// its start, each being the last datum at or before that boundary. So
// `datums` may reach back before `start`, and may include a datum at `end`.
// Without a reading at or before `start`, the first datum in the window is
// the baseline.
Sample reduceWindow(const std::vector<Datum> &datums, const DatumSource &source,
                    Timestamp start, Timestamp end);

// The latest value, stale when older than the source allows.
Sample latestSample(const std::optional<Datum> &datum,
                    const DatumSource &source, Timestamp now);

} // namespace grid_services::datum
