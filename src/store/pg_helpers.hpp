#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "core/types.hpp"

// Conversions shared by the Postgres stores.
namespace grid_services::pg {

template <typename T> std::optional<T> nullable(const pqxx::field &field) {
  if (field.is_null()) {
    return std::nullopt;
  }
  return field.as<T>();
}

inline Timestamp fromEpoch(long long seconds) {
  return Timestamp(std::chrono::seconds(seconds));
}

inline long long toEpoch(Timestamp at) { return at.time_since_epoch().count(); }

inline std::optional<long long> toEpoch(const std::optional<Timestamp> &at) {
  return at ? std::optional<long long>(toEpoch(*at)) : std::nullopt;
}

inline std::optional<std::string> jsonText(const nlohmann::json &value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  return value.dump();
}

} // namespace grid_services::pg
