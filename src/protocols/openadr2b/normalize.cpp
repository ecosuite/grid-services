#include "protocols/openadr2b/normalize.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include <oadr/helper/DateTimeConverter.h>
#include <oadr/helper/ISO8601Duration.h>

namespace grid_services::openadr2b {

namespace {

using oadr2b::ei::EventStatusEnumeratedType;

ProgramStatus toProgramStatus(const EventStatusEnumeratedType &status) {
  switch (status) {
  case EventStatusEnumeratedType::active:
    return ProgramStatus::Active;
  case EventStatusEnumeratedType::completed:
    return ProgramStatus::Completed;
  case EventStatusEnumeratedType::cancelled:
    return ProgramStatus::Cancelled;
  case EventStatusEnumeratedType::none:
  case EventStatusEnumeratedType::far:
  case EventStatusEnumeratedType::near:
    return ProgramStatus::Pending;
  }
  return ProgramStatus::Pending;
}

int durationSeconds(const std::string &duration) {
  return static_cast<int>(ISO8601Duration::TotalSeconds(duration));
}

double payloadValue(const oadr2b::ei::IntervalType &interval) {
  for (const auto &payload : interval.streamPayloadBase()) {
    const auto *signalPayload =
        dynamic_cast<const oadr2b::ei::signalPayloadType *>(&payload);
    if (signalPayload == nullptr) {
      continue;
    }
    const auto *value = dynamic_cast<const oadr2b::ei::PayloadFloatType *>(
        &signalPayload->payloadBase());
    if (value != nullptr) {
      return value->value();
    }
  }
  throw std::runtime_error("interval has no float signal payload");
}

Signal toSignal(const oadr2b::ei::eiEventSignalType &source) {
  Signal signal;
  signal.name = source.signalName();
  signal.type = source.signalType();
  signal.id = source.signalID();

  int offset = 0;
  for (const auto &interval : source.intervals().interval()) {
    const int length =
        interval.duration() ? durationSeconds(interval.duration()->duration())
                            : 0;
    signal.intervals.push_back({offset, length, payloadValue(interval)});
    offset += length;
  }
  return signal;
}

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return text;
}

bool isTestEvent(const oadr2b::ei::eventDescriptorType &descriptor) {
  if (!descriptor.testEvent()) {
    return false;
  }
  // Anything other than "false" marks a test event, but an empty element is
  // the same as an omitted one (OpenADR 2.0b rules 6 and 46).
  const auto value = lowercase(*descriptor.testEvent());
  return !value.empty() && value != "false";
}

bool supported(const oadr2b::ei::eiEventSignalType &signal) {
  const std::string name = signal.signalName();
  const std::string &type = signal.signalType();
  const oadr2b::emix::ItemBaseType *units =
      signal.itemBase() ? &*signal.itemBase() : nullptr;

  if (lowercase(name) == "simple") {
    return type == "level" && units == nullptr;
  }
  if (name == "ELECTRICITY_PRICE") {
    const auto *currency =
        dynamic_cast<const oadr2b::oadr::currencyType *>(units);
    return type == "price" && currency != nullptr &&
           static_cast<const std::string &>(currency->itemDescription()) ==
               "currencyPerKWh";
  }
  if (name == "LOAD_DISPATCH") {
    return type == "setpoint" &&
           (dynamic_cast<const power::PowerRealType *>(units) != nullptr ||
            dynamic_cast<const power::PowerApparentType *>(units) != nullptr ||
            dynamic_cast<const power::PowerReactiveType *>(units) != nullptr);
  }
  return false;
}

std::string normalizedUri(std::string uri) {
  const auto scheme = uri.find("://");
  if (scheme == std::string::npos) {
    return uri;
  }
  const auto path = uri.find_first_of("/?#", scheme + 3);
  const auto authorityEnd = path == std::string::npos ? uri.size() : path;
  std::transform(uri.begin(), uri.begin() + static_cast<long>(authorityEnd),
                 uri.begin(), [](unsigned char c) { return std::tolower(c); });
  if (path == std::string::npos || uri[path] != '/') {
    uri.insert(authorityEnd, "/");
  }
  return uri;
}

} // namespace

NormalizedEvent normalizeEvent(const oadr2b::oadr::oadrEvent &event,
                               const std::string &requestId) {
  const auto &eiEvent = event.eiEvent();
  const auto &descriptor = eiEvent.eventDescriptor();
  const auto &properties = eiEvent.eiActivePeriod().properties();

  NormalizedEvent normalized;
  normalized.programEventId = descriptor.eventID();
  normalized.modificationNumber =
      static_cast<int>(descriptor.modificationNumber());
  normalized.status = toProgramStatus(descriptor.eventStatus());
  normalized.startAt = Timestamp(std::chrono::seconds(
      DateTimeConverter::DateTimeToTime_t(properties.dtstart().date_time())));
  normalized.durationSeconds =
      durationSeconds(properties.duration().duration());
  if (properties.tolerance() &&
      properties.tolerance()->tolerate().startafter()) {
    normalized.startAfterSeconds =
        durationSeconds(*properties.tolerance()->tolerate().startafter());
  }
  normalized.marketContext =
      std::string(descriptor.eiMarketContext().marketContext());
  normalized.testEvent = isTestEvent(descriptor);
  normalized.responseRequired =
      event.oadrResponseRequired() ==
      oadr2b::oadr::ResponseRequiredType::always;
  if (descriptor.vtnComment()) {
    normalized.comment = std::string(*descriptor.vtnComment());
  }
  if (!requestId.empty()) {
    normalized.programRequestId = requestId;
  }

  for (const auto &signal : eiEvent.eiEventSignals().eiEventSignal()) {
    normalized.signals.push_back(toSignal(signal));
  }
  return normalized;
}

std::optional<Refusal> unsupportedSignal(const oadr2b::oadr::oadrEvent &event) {
  for (const auto &signal : event.eiEvent().eiEventSignals().eiEventSignal()) {
    if (!supported(signal)) {
      return Refusal{"460", "signal " + std::string(signal.signalName()) +
                                " of type " + std::string(signal.signalType()) +
                                " in these units is not supported"};
    }
  }
  return std::nullopt;
}

std::optional<Refusal>
unacceptedSignal(const NormalizedEvent &event,
                 const std::vector<std::string> &acceptedSignals) {
  for (const auto &signal : event.signals) {
    const auto name = lowercase(signal.name);
    const bool accepted =
        std::any_of(acceptedSignals.begin(), acceptedSignals.end(),
                    [&](const auto &candidate) {
                      return lowercase(candidate) == name;
                    });
    if (!accepted) {
      return Refusal{"460", "signal " + signal.name +
                                " is not accepted by this connection"};
    }
  }
  return std::nullopt;
}

std::optional<Refusal> outOfRangeSignal(const NormalizedEvent &event) {
  for (const auto &signal : event.signals) {
    if (lowercase(signal.name) != "simple") {
      continue;
    }
    for (const auto &interval : signal.intervals) {
      const double level = interval.value;
      if (level != 0 && level != 1 && level != 2 && level != 3) {
        std::ostringstream reason;
        reason << "signal " << signal.name << " level " << level
               << " is not 0, 1, 2 or 3";
        return Refusal{"454", reason.str()};
      }
    }
  }
  return std::nullopt;
}

bool sameMarketContext(const std::string &a, const std::string &b) {
  return normalizedUri(a) == normalizedUri(b);
}

bool targetsVen(const oadr2b::oadr::oadrEvent &event,
                const std::string &venId) {
  const auto &venIds = event.eiEvent().eiTarget().venID();
  if (venIds.empty() || venId.empty()) {
    return true;
  }
  return std::any_of(venIds.begin(), venIds.end(),
                     [&](const auto &id) { return id == venId; });
}

} // namespace grid_services::openadr2b
