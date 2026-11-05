#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/telemetry.hpp"
#include "datum/datum.hpp"
#include "protocols/csipaus/documents.hpp"
#include "protocols/csipaus/store.hpp"

// What a CSIP-AUS device sends its server about itself: its DER's
// capability, settings and status, and the readings it mirrors. The
// documents, and the arithmetic behind them; the Reporter sends them.
namespace grid_services::csipaus {

// The post rate readings have when the server gives none: CSIP-AUS's
// default of five minutes.
inline constexpr std::chrono::seconds defaultPostRate{300};

// The largest PEN an mRID can end in: it is written in 8 decimal digits.
inline constexpr std::uint32_t maxPen = 99'999'999;

// A reading bound to the telemetry point it is read from.
struct BoundReading {
  Mirror mirror = Mirror::Site;
  ReadingKind kind = ReadingKind::RealPower;
  Phase phase = Phase::None;
  // The power of ten its values are sent at: the row's for power, -1 for
  // voltage (tenths of a volt) and -2 for frequency (hundredths of a hertz).
  int powerOfTen = 0;
  TelemetryPoint point;
};

// Binds the connection's readings to its telemetry points. Throws
// std::invalid_argument for a reading whose point the connection does not
// have, or measures something else; for a power reading without a power of
// ten, or with one outside IEEE 2030.5's -9 to 9; and for a voltage or
// frequency reading with one.
std::vector<BoundReading> bindReadings(const std::vector<ReadingRow> &rows,
                                       const std::vector<TelemetryPoint> &points);

// The state telemetry points the DER's status is sent from. Either is
// absent while unbound.
struct StatePoints {
  std::optional<TelemetryPoint> connected;
  std::optional<TelemetryPoint> operating;
};

// Binds the DER's state to its telemetry points. Throws
// std::invalid_argument for a point the connection does not have, or one
// that is not a state.
StatePoints bindState(const DerDescription &der,
                      const std::vector<TelemetryPoint> &points);

// The reading's name in deliveries: real_power, voltage_a, ...
std::string readingId(const BoundReading &reading);
// "Site real power", "DER voltage phase A": at most 32 characters, as
// IEEE 2030.5 descriptions are.
std::string describeReading(const BoundReading &reading);
std::string describeMirror(Mirror mirror);

// 32 characters: 24 hex digits drawn from the LFDI and what is identified,
// then the PEN (at most maxPen) as 8 decimal digits, as the CSIP-AUS v1.2
// explainer's section 4.1.7 asks. The same across restarts, so the worker
// finds what it created before.
std::string mirrorMrid(const std::string &lfdi, Mirror mirror,
                       std::uint32_t pen);
std::string readingMrid(const std::string &lfdi, const BoundReading &reading,
                        std::uint32_t pen);

// The value sent for a reading at `powerOfTen`: scaled and rounded. Nothing
// when it does not fit the Int16 CSIP-AUS sends readings as.
std::optional<long long> sentValue(int powerOfTen, double value);
// What the server makes of a sent value, in the reading's units.
double receivedValue(int powerOfTen, long long sent);

// The mirror usage point as it is created, with a mirror meter reading,
// and its reading type, for each of `readings`.
sep::MirrorUsagePoint mirrorUsagePoint(Mirror mirror,
                                       const std::vector<BoundReading> &readings,
                                       const std::string &lfdi,
                                       std::uint32_t pen);

// The values of one window, posted to a mirror usage point. Readings
// without a value, or whose value does not fit an Int16 at their power of
// ten, are left out: the server is not sent placeholders.
sep::MirrorMeterReadingList
readingsDocument(const std::vector<std::pair<BoundReading, datum::Sample>> &values,
                 const std::string &lfdi, std::uint32_t pen, Timestamp start,
                 std::chrono::seconds length);

// The start of the window `at` falls in, windows being aligned to whole
// multiples of `length` since the epoch.
Timestamp windowStart(Timestamp at, std::chrono::seconds length);

// The modes the site supports or has enabled, as DERControlType (the
// opMod bits) and csipaus:DOEControlType (the envelope bits) write them.
std::uint32_t modeBits(const std::vector<std::string> &modes);
std::uint8_t doeModeBits(const std::vector<std::string> &modes);

sep::DERCapability capabilityDocument(const DerDescription &der);
// The DER's settings, with the setGradW in force and when it took that
// value as updatedTime.
sep::DERSettings settingsDocument(const DerDescription &der, int setGradW,
                                  Timestamp updatedTime);

// What the site's DER is doing, as DERStatus reports it.
struct DerState {
  // genConnectStatus bits: 0 connected, 1 available, 2 operating, 4 fault.
  unsigned connectStatus = 0;
  // operationalModeStatus: 1 off, 2 operational. Absent when unknown.
  std::optional<int> operationalMode;

  bool operator==(const DerState &) const = default;
};

inline constexpr unsigned connected = 1u << 0;
inline constexpr unsigned available = 1u << 1;
inline constexpr unsigned operating = 1u << 2;
inline constexpr unsigned faulted = 1u << 4;

// A state as the worker takes it: a value other than exactly 1 or 0 is
// unreadable.
datum::Sample stateSample(const datum::Sample &sample);

// The DER's state from its connected and operating state points: bit 0 while
// connected is 1, bit 2 and operationalModeStatus 2 while operating is 1
// (1 while it is 0), bit 1 while both are good, and bit 4 while either is
// stale or has no value the worker can read, which clears its own bits.
DerState derState(const datum::Sample &connectedState,
                  const datum::Sample &operatingState);
sep::DERStatus statusDocument(const DerState &state, Timestamp at);

// A state point, and what was read from it.
struct StateReading {
  TelemetryPoint point;
  datum::Sample sample;
};

// What each document carried, as a delivery's payload records it.
nlohmann::json capabilityPayload(const DerDescription &der, Timestamp at);
nlohmann::json settingsPayload(const DerDescription &der, int setGradW,
                               Timestamp updatedTime, Timestamp at);
nlohmann::json statusPayload(const DerState &state,
                             const StateReading &connectedState,
                             const StateReading &operatingState, Timestamp at);

} // namespace grid_services::csipaus
