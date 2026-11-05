#include "protocols/csipaus/store.hpp"

#include <stdexcept>

#include "core/errors.hpp"

namespace grid_services::csipaus {

std::string toString(CsipausEventKind kind) {
  return kind == CsipausEventKind::Default ? "default" : "control";
}

std::string toString(Mirror value) {
  return value == Mirror::Der ? "der" : "site";
}

std::string toString(ReadingKind value) {
  switch (value) {
  case ReadingKind::ReactivePower:
    return "reactive_power";
  case ReadingKind::Voltage:
    return "voltage";
  case ReadingKind::Frequency:
    return "frequency";
  default:
    return "real_power";
  }
}

std::string toString(Phase value) {
  switch (value) {
  case Phase::A:
    return "a";
  case Phase::B:
    return "b";
  case Phase::C:
    return "c";
  default:
    return "none";
  }
}

Mirror parseMirror(const std::string &value) {
  if (value == "site") {
    return Mirror::Site;
  }
  if (value == "der") {
    return Mirror::Der;
  }
  throw std::invalid_argument("unknown mirror " + value);
}

ReadingKind parseReadingKind(const std::string &value) {
  if (value == "real_power") {
    return ReadingKind::RealPower;
  }
  if (value == "reactive_power") {
    return ReadingKind::ReactivePower;
  }
  if (value == "voltage") {
    return ReadingKind::Voltage;
  }
  if (value == "frequency") {
    return ReadingKind::Frequency;
  }
  throw std::invalid_argument("unknown reading " + value);
}

Phase parsePhase(const std::string &value) {
  if (value == "none") {
    return Phase::None;
  }
  if (value == "a") {
    return Phase::A;
  }
  if (value == "b") {
    return Phase::B;
  }
  if (value == "c") {
    return Phase::C;
  }
  throw std::invalid_argument("unknown phase " + value);
}

CsipausStore &csipausStore(RunnerStore &store) {
  auto *csipaus = dynamic_cast<CsipausStore *>(&store);
  if (csipaus == nullptr) {
    throw AdapterError::permanent("the worker's store has no CSIP-AUS settings");
  }
  return *csipaus;
}

} // namespace grid_services::csipaus
