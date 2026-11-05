#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "protocols/csipaus/documents.hpp"
#include "protocols/csipaus/store.hpp"

namespace grid_services::csipaus {

// Watts for an ActivePower: value times ten to the multiplier.
double watts(const sep::ActivePower &power);

// What a control asks for, in words: "export 1.5 kW, import 10 kW,
// energize off". "no limits" when it asks for nothing this worker reads.
std::string describeControlBase(const sep::DERControlBase &base);

// The caps a control sets, as signals of type "limit" named for the cap:
// export, import, generation and load in watts, max_output in percent of the
// site's rating, and energize and connect as 1 for on and 0 for off. Each
// holds for `durationSeconds`, or until further notice when it is zero.
std::vector<Signal> capSignals(const sep::DERControlBase &base,
                               int durationSeconds);

// A DERControl's EventStatus.currentStatus as a program status.
ProgramStatus controlStatus(int currentStatus);

// The program a control belongs to.
struct ProgramInfo {
  std::string mrid;
  std::optional<std::string> description;
  int primacy = 0;
};

// How a control or a default says the site ramps: a control's rampTms, in
// hundredths of a second, and a default's setGradW, in hundredths of a
// percent of setMaxW a second.
struct Ramp {
  std::optional<int> rampTms;
  std::optional<int> setGradW;
};

struct ObservedEvent {
  NormalizedEvent event;
  CsipausEventDetail detail;
  Ramp ramp;
};

// A DERControl as an event the site cannot decline.
ObservedEvent observeControl(const sep::DERControl &control,
                             const ProgramInfo &program);

// The event id of a program's DefaultDERControl. It is keyed by the program,
// so a default the server replaces under a new mRID stays one event.
std::string defaultEventId(const std::string &programMrid);

// A program's DefaultDERControl, at `href`, as an open-ended event from
// `since`.
ObservedEvent observeDefault(const sep::DefaultDERControl &defaults,
                             const std::string &href,
                             const ProgramInfo &program, Timestamp since);

// DERControlResponse status codes (IEEE 2030.5, table 27).
inline constexpr int responseReceived = 1;
inline constexpr int responseStarted = 2;
inline constexpr int responseCompleted = 3;
inline constexpr int responseCancelled = 6;
inline constexpr int responseSuperseded = 7;

// responseRequired bits: bit 0 asks for "received", bit 1 for what became of
// the event (started, completed, cancelled, superseded).
inline constexpr int requiresReceived = 0x01;
inline constexpr int requiresProgress = 0x02;

} // namespace grid_services::csipaus
