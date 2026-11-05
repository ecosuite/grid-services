#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/types.hpp"
#include "datum/datum.hpp"

namespace grid_services {

using TelemetryPointId = std::int64_t;

// What a telemetry point measures.
//   kind   power_real, power_reactive, power_apparent, energy_real,
//          energy_reactive, energy_apparent, voltage, frequency,
//          temperature, state (1 or 0), custom
//   units  W, Wh, V, Hz, celsius, ..., none for a state; scale an SI
//          prefix: none, k, M
struct TelemetryItem {
  std::string kind = "power_real";
  std::string units = "W";
  std::string scale = "none";
  // For custom items.
  std::string description;
};

// Something the site measures, and where its value comes from. Protocols
// bind points into what they report to the program.
struct TelemetryPoint {
  TelemetryPointId id = 0;
  // The connection's own name for the point, unique per connection.
  std::string pointId;
  TelemetryItem item;
  datum::DatumSource source;
};

// The SolarNetwork token the connection reads datums with.
struct DataCredential {
  std::string host;
  std::string token;
  // Sealed with the worker's public key.
  std::string secretSealed;
};

// The client certificate the connection authenticates to its program with.
// One credential may serve several connections.
struct ClientCredential {
  std::string certPem;
  // The private key, sealed with the worker's public key.
  std::string keySealed;
};

// One report sent, or attempted.
struct ReportDelivery {
  // The program's request the report answered, for protocols whose programs
  // ask for reports.
  std::optional<std::string> programRequestId;
  std::optional<Timestamp> intervalStart;
  std::optional<Timestamp> intervalEnd;
  // [{"rid", "point_id"?, "dtstart", "duration_seconds", "value" |
  //   "online" + "manual_override", "quality", "source"?}]
  // where "rid" is the protocol's id for the value and "quality" one of
  // datum::Quality's.
  nlohmann::json payload = nlohmann::json::array();
  // [{"node_id", "source_id", "kind", "path", "query", "datum_count",
  //   "error"}]
  nlohmann::json reads = nlohmann::json::array();
  bool ok = false;
  std::optional<std::string> responseCode;
  std::string message;
  // What was reported, for protocols whose programs do not ask for reports,
  // such as CSIP-AUS's site_readings.
  std::optional<std::string> report;
  // The messages exchanged to deliver it, as Adapter::takeExchange gives
  // them; null when they are not kept.
  nlohmann::json exchange;
};

// Checks an item read from the database. Throws std::invalid_argument for
// one the worker cannot report.
void checkItem(const TelemetryItem &item, const std::string &where);

// Reads a telemetry point's source column. Throws std::invalid_argument for
// one the worker cannot read.
datum::DatumSource parseDatumSource(const nlohmann::json &json,
                                    const std::string &where);

// Where a reported value came from and how it was reduced, as a delivery's
// payload entry records it under "source".
nlohmann::json sourceRecord(const datum::DatumSource &source);

// One datum read, as a delivery's reads record it. A list read also records
// the window the datums were trimmed to, which is narrower than the
// minute-precision request.
nlohmann::json readRecord(datum::NodeId nodeId, const std::string &sourceId,
                          const char *kind, const datum::DatumRequest &request,
                          std::optional<std::size_t> datumCount,
                          const std::optional<std::string> &error,
                          std::optional<std::pair<Timestamp, Timestamp>> window =
                              std::nullopt);

// ISO 8601 in UTC with milliseconds, as deliveries record times.
std::string isoTime(Timestamp at);

} // namespace grid_services
