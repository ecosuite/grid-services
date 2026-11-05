#pragma once

#include <optional>
#include <string>
#include <vector>

#include <oadrsd/2b/oadr_20b.hxx>

#include "core/types.hpp"

namespace grid_services::openadr2b {

// Converts a 2.0b event into the protocol-neutral event. Throws
// std::runtime_error for an event that is missing required values.
NormalizedEvent normalizeEvent(const oadr2b::oadr::oadrEvent &event,
                               const std::string &requestId);

// True when the event is meant for this VEN. An event that lists VEN ids
// must include ours; events targeted another way are accepted.
bool targetsVen(const oadr2b::oadr::oadrEvent &event, const std::string &venId);

// Why the VEN will not act on an event: an OpenADR response code and a
// description.
struct Refusal {
  std::string code;
  std::string reason;
};

// Refuses an event with a signal the VEN does not support. It supports only
// the signals OpenADR 2.0b rule 510 requires, in the units Table 1 gives
// them: SIMPLE/level, ELECTRICITY_PRICE/price in currency per kWh, and
// LOAD_DISPATCH/setpoint in power.
std::optional<Refusal> unsupportedSignal(const oadr2b::oadr::oadrEvent &event);

// Refuses an event with a signal the connection does not accept, which is
// one its site has no way to act on. Names are compared without regard to
// case, as SIMPLE may be written in either (rule 7).
std::optional<Refusal>
unacceptedSignal(const NormalizedEvent &event,
                 const std::vector<std::string> &acceptedSignals);

// Refuses an event whose SIMPLE signal has a level other than 0 (normal),
// 1 (moderate), 2 (high) or 3 (special), which rule 9 allows, as invalid
// data.
std::optional<Refusal> outOfRangeSignal(const NormalizedEvent &event);

// True when two market context URIs are equivalent under RFC 3986 section
// 6.2: the scheme and authority are case-insensitive, and an empty path is
// "/".
bool sameMarketContext(const std::string &a, const std::string &b);

} // namespace grid_services::openadr2b
