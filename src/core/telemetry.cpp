#include "core/telemetry.hpp"

#include <set>
#include <stdexcept>

namespace grid_services {

namespace {

const std::set<std::string> itemKinds = {
    "power_real",      "power_reactive",  "power_apparent", "energy_real",
    "energy_reactive", "energy_apparent", "voltage",        "frequency",
    "temperature",     "state",           "custom"};

std::string requiredString(const nlohmann::json &object, const char *key,
                           const std::string &where) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_string() ||
      found->get<std::string>().empty()) {
    throw std::invalid_argument(where + " needs a " + key);
  }
  return found->get<std::string>();
}

std::string optionalString(const nlohmann::json &object, const char *key,
                           const std::string &fallback) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    return fallback;
  }
  if (!found->is_string()) {
    throw std::invalid_argument(std::string(key) + " must be a string");
  }
  return found->get<std::string>();
}

} // namespace

void checkItem(const TelemetryItem &item, const std::string &where) {
  if (itemKinds.count(item.kind) == 0) {
    throw std::invalid_argument(where + " has unknown item kind " + item.kind);
  }
  if (item.kind == "custom" && item.description.empty()) {
    throw std::invalid_argument(where + " custom item needs a description");
  }
  if (item.kind == "temperature" && item.units != "celsius" &&
      item.units != "fahrenheit") {
    throw std::invalid_argument(where +
                                " temperature units must be celsius or "
                                "fahrenheit");
  }
  if (item.kind == "state" && (item.units != "none" || item.scale != "none")) {
    throw std::invalid_argument(where +
                                " state is 1 or 0: its units and scale must "
                                "be none");
  }
}

datum::DatumSource parseDatumSource(const nlohmann::json &json,
                                    const std::string &where) {
  if (!json.is_object()) {
    throw std::invalid_argument(where + " needs a source");
  }
  datum::DatumSource source;
  const auto nodeId = json.find("node_id");
  if (nodeId == json.end() || !nodeId->is_number_integer()) {
    throw std::invalid_argument(where + " source needs a node_id");
  }
  source.nodeId = nodeId->get<datum::NodeId>();
  source.sourceId = requiredString(json, "source_id", where + " source");
  source.property = requiredString(json, "property", where + " source");
  try {
    source.propertyClass = datum::parsePropertyClass(
        optionalString(json, "property_class", "instantaneous"));
    source.reduce = json.contains("reduce") && !json["reduce"].is_null()
                        ? datum::parseReduce(requiredString(
                              json, "reduce", where + " source"))
                        : datum::defaultReduce(source.propertyClass);
  } catch (const std::invalid_argument &error) {
    throw std::invalid_argument(where + " source: " + error.what());
  }
  if (const auto multiplier = json.find("multiplier");
      multiplier != json.end() && !multiplier->is_null()) {
    if (!multiplier->is_number()) {
      throw std::invalid_argument(where + " source multiplier must be a number");
    }
    source.multiplier = multiplier->get<double>();
  }
  if (const auto stale = json.find("stale_after_seconds");
      stale != json.end() && !stale->is_null()) {
    if (!stale->is_number_integer() || stale->get<long>() <= 0) {
      throw std::invalid_argument(
          where + " source stale_after_seconds must be a positive integer");
    }
    source.staleAfter = std::chrono::seconds(stale->get<long>());
  }
  return source;
}

nlohmann::json sourceRecord(const datum::DatumSource &source) {
  return {{"node_id", source.nodeId},
          {"source_id", source.sourceId},
          {"property", source.property},
          {"property_class", datum::toString(source.propertyClass)},
          {"reduce", datum::toString(source.reduce)},
          {"multiplier", source.multiplier},
          {"stale_after_seconds", source.staleAfter.count()}};
}

nlohmann::json readRecord(datum::NodeId nodeId, const std::string &sourceId,
                          const char *kind, const datum::DatumRequest &request,
                          std::optional<std::size_t> datumCount,
                          const std::optional<std::string> &error,
                          std::optional<std::pair<Timestamp, Timestamp>> window) {
  nlohmann::json read = {
      {"node_id", nodeId},
      {"source_id", sourceId},
      {"kind", kind},
      {"path", request.path},
      {"query", request.query},
      {"datum_count",
       datumCount ? nlohmann::json(*datumCount) : nlohmann::json(nullptr)},
      {"error", error ? nlohmann::json(*error) : nlohmann::json(nullptr)}};
  if (window) {
    read["start"] = isoTime(window->first);
    read["end"] = isoTime(window->second);
  }
  return read;
}

std::string isoTime(Timestamp at) {
  return toIso8601(std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          at.time_since_epoch())));
}

} // namespace grid_services
