#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/store.hpp"

namespace grid_services::csipaus {

// A connection's CSIP-AUS settings. The client certificate, whose LFDI the
// server knows the connection by, is the connection's client credential.
struct CsipausSettings {
  std::string dcapUrl;
  std::optional<std::string> caCertPem;
  bool verifyServerCertificate = true;
  // Sent on registration; by default the site's NMI.
  std::optional<std::string> connectionPointId;
  // Checked against the server's Registration on registration.
  std::optional<std::uint32_t> registrationPin;
};

enum class CsipausEventKind { Control, Default };

std::string toString(CsipausEventKind kind);

// What CSIP-AUS says about a program event beyond the shared columns: a row
// of csipaus_events.
struct CsipausEventDetail {
  CsipausEventKind kind = CsipausEventKind::Control;
  std::string href;
  std::string mrid;
  std::string programMrid;
  std::optional<std::string> programDescription;
  int primacy = 0;
  std::optional<Timestamp> creationTime;
  std::optional<int> randomizeStartSeconds;
  std::optional<int> randomizeDurationSeconds;
  // A control's window after the device's randomization, in the server's
  // time. Null for a default.
  std::optional<Timestamp> effectiveStartAt;
  std::optional<Timestamp> effectiveEndAt;
  std::optional<std::string> replyTo;
  int responseRequired = 0;

  bool operator==(const CsipausEventDetail &) const = default;
};

// One stretch of time over which one cap holds one value: a row of
// csipaus_limits.
struct LimitSegment {
  // The cap, as its signal is named: export, import, generation, load,
  // max_output, energize or connect.
  std::string limit;
  Timestamp start{};
  // Null while it holds until further notice.
  std::optional<Timestamp> end;
  double value = 0;
  // A program's default, or a control.
  CsipausEventKind source = CsipausEventKind::Default;
  // The event the value comes from.
  std::string programEventId;
  // The ramp into it: a control's rampTms, in hundredths of a second, or
  // else the setGradW of the default in force, in hundredths of a percent of
  // setMaxW a second. Neither when nothing sets a ramp.
  std::optional<int> rampTms;
  std::optional<int> setGradW;

  bool operator==(const LimitSegment &) const = default;
};

// The site's DER as the connection describes it to the server: a row of
// csipaus_der. Watts, volt-amperes, vars, watt-hours and volts.
struct DerDescription {
  // IEEE 2030.5 DERType.
  int type = 0;
  double rtgMaxW = 0;
  std::optional<double> rtgMaxVa;
  std::optional<double> rtgMaxVar;
  std::optional<double> rtgMaxVarNeg;
  std::optional<double> rtgMaxChargeRateW;
  std::optional<double> rtgMaxDischargeRateW;
  std::optional<double> rtgMaxWh;
  std::optional<double> rtgVNom;
  // Null is the rating.
  std::optional<double> setMaxW;
  std::optional<double> setMaxVa;
  std::optional<double> setMaxVar;
  std::optional<double> setMaxChargeRateW;
  std::optional<double> setMaxDischargeRateW;
  // Hundredths of a percent of setMaxW per second.
  int setGradW = 27;
  // The caps the site can act on, and acts on now, as csipaus_limits names
  // them.
  std::vector<std::string> modesSupported;
  std::vector<std::string> modesEnabled;
  // When it last changed: DERSettings' updatedTime.
  Timestamp updatedAt{};
  // The state telemetry points DERStatus is sent from. Null is unbound.
  std::optional<std::string> connectedPointId;
  std::optional<std::string> operatingPointId;

  bool operator==(const DerDescription &) const = default;
};

// Where a reading is posted: the mirror usage point for the site's
// connection point, or the one for its DER.
enum class Mirror { Site, Der };

enum class ReadingKind { RealPower, ReactivePower, Voltage, Frequency };

// None for a single-phase or total reading; A, B or C for one phase's
// voltage to neutral.
enum class Phase { None, A, B, C };

std::string toString(Mirror value);
std::string toString(ReadingKind value);
std::string toString(Phase value);
// Throw std::invalid_argument for anything else.
Mirror parseMirror(const std::string &value);
ReadingKind parseReadingKind(const std::string &value);
Phase parsePhase(const std::string &value);

// A row of csipaus_readings: the telemetry point a reading is posted from.
struct ReadingRow {
  Mirror mirror = Mirror::Site;
  ReadingKind reading = ReadingKind::RealPower;
  Phase phase = Phase::None;
  std::string pointId;
  // The power of ten a power reading is sent at. Null for voltage and
  // frequency.
  std::optional<int> powerOfTen;
};

// A control the site has started or completed, by its planner's lifecycle,
// that the server has not yet been told about.
struct ProgressDue {
  KnownEvent event;
  CsipausEventDetail detail;
  // The answers still to give, in order: started, then completed.
  std::vector<EventAnswer> answers;
};

// Database access for the CSIP-AUS tables, on the runner's own session. A
// runner store that serves CSIP-AUS connections implements it beside
// RunnerStore.
class CsipausStore {
public:
  virtual ~CsipausStore() = default;

  virtual CsipausSettings loadCsipausSettings() = 0;
  // Inserts or replaces the detail of one of the connection's events.
  virtual void recordEventDetail(EventRowId eventId,
                                 const CsipausEventDetail &detail) = 0;
  // Replaces the connection's caps in force.
  virtual void replaceLimits(const std::vector<LimitSegment> &limits) = 0;
  // The controls received in the last week whose progress the server asks
  // for (responseRequired bit 1), whose planner's lifecycle has reached
  // control or completed, and that the server has not been told of. Not
  // those it cancelled or superseded, nor those it was yet to be told it
  // received when it asks for that (bit 0).
  virtual std::vector<ProgressDue> progressDue() = 0;
  // The connection's DER, if it describes one.
  virtual std::optional<DerDescription> loadDer() = 0;
  // The readings the connection posts. Throws std::invalid_argument for a
  // row the worker cannot read.
  virtual std::vector<ReadingRow> loadReadings() = 0;
};

// The CSIP-AUS side of a runner store. Throws AdapterError::permanent when
// the store has none.
CsipausStore &csipausStore(RunnerStore &store);

} // namespace grid_services::csipaus
