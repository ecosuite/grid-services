#include "datum/datum.hpp"

#include <algorithm>
#include <stdexcept>

namespace grid_services::datum {

PropertyClass parsePropertyClass(const std::string &value) {
  if (value == "instantaneous") {
    return PropertyClass::Instantaneous;
  }
  if (value == "accumulating") {
    return PropertyClass::Accumulating;
  }
  throw std::invalid_argument("unknown property class: " + value);
}

Reduce parseReduce(const std::string &value) {
  if (value == "average") {
    return Reduce::Average;
  }
  if (value == "min") {
    return Reduce::Min;
  }
  if (value == "max") {
    return Reduce::Max;
  }
  if (value == "difference") {
    return Reduce::Difference;
  }
  if (value == "last") {
    return Reduce::Last;
  }
  throw std::invalid_argument("unknown reduce: " + value);
}

std::string toString(PropertyClass value) {
  return value == PropertyClass::Instantaneous ? "instantaneous"
                                               : "accumulating";
}

std::string toString(Reduce value) {
  switch (value) {
  case Reduce::Average:
    return "average";
  case Reduce::Min:
    return "min";
  case Reduce::Max:
    return "max";
  case Reduce::Difference:
    return "difference";
  case Reduce::Last:
    return "last";
  }
  return "unknown";
}

Reduce defaultReduce(PropertyClass propertyClass) {
  return propertyClass == PropertyClass::Accumulating ? Reduce::Difference
                                                      : Reduce::Average;
}

std::string toString(Quality value) {
  switch (value) {
  case Quality::Good:
    return "good";
  case Quality::NoValue:
    return "no_value";
  case Quality::Stale:
    return "stale";
  case Quality::Unreadable:
    return "unreadable";
  }
  return "unknown";
}

namespace {

std::optional<double> property(const Datum &datum, const std::string &name) {
  const auto found = datum.values.find(name);
  if (found == datum.values.end()) {
    return std::nullopt;
  }
  return found->second;
}

Sample good(double value, const DatumSource &source) {
  return {value * source.multiplier, Quality::Good};
}

} // namespace

Sample reduceWindow(const std::vector<Datum> &datums, const DatumSource &source,
                    Timestamp start, Timestamp end) {
  std::vector<double> inWindow;
  std::optional<double> baseline;
  std::optional<double> atEnd;
  for (const auto &datum : datums) {
    const auto value = property(datum, source.property);
    if (!value) {
      continue;
    }
    if (datum.at <= start) {
      baseline = value;
    }
    if (datum.at <= end) {
      atEnd = value;
    }
    if (datum.at >= start && datum.at < end) {
      inWindow.push_back(*value);
    }
  }
  if (inWindow.empty()) {
    return {};
  }

  switch (source.reduce) {
  case Reduce::Average: {
    double total = 0;
    for (const auto value : inWindow) {
      total += value;
    }
    return good(total / static_cast<double>(inWindow.size()), source);
  }
  case Reduce::Min:
    return good(*std::min_element(inWindow.begin(), inWindow.end()), source);
  case Reduce::Max:
    return good(*std::max_element(inWindow.begin(), inWindow.end()), source);
  case Reduce::Last:
    return good(inWindow.back(), source);
  case Reduce::Difference:
    return good(atEnd.value_or(inWindow.back()) -
                    baseline.value_or(inWindow.front()),
                source);
  }
  return {};
}

Sample latestSample(const std::optional<Datum> &datum,
                    const DatumSource &source, Timestamp now) {
  if (!datum) {
    return {};
  }
  const auto value = property(*datum, source.property);
  if (!value) {
    return {};
  }
  auto sample = good(*value, source);
  if (datum->at + source.staleAfter < now) {
    sample.quality = Quality::Stale;
  }
  return sample;
}

} // namespace grid_services::datum
