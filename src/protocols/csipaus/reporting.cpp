#include "protocols/csipaus/reporting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>

#include <sodium.h>

namespace grid_services::csipaus {

namespace {

// IEEE 2030.5 codes.
constexpr unsigned char uomVolts = 29;
constexpr unsigned char uomHertz = 33;
constexpr unsigned char uomWatts = 38;
constexpr unsigned char uomVars = 63;
constexpr unsigned char kindPower = 37;
constexpr unsigned char qualifierAverage = 2;
constexpr unsigned char serviceElectricity = 0;
constexpr unsigned char statusOn = 1;
// isMirror and isPremisesAggregationPoint; isMirror, isDER and isSubmeter.
constexpr const char *siteRoleFlags = "03";
constexpr const char *derRoleFlags = "49";
// Valid.
constexpr const char *goodQuality = "0001";

struct PointKind {
  const char *kind;
  const char *units;
};

PointKind pointKind(ReadingKind kind) {
  switch (kind) {
  case ReadingKind::ReactivePower:
    return {"power_reactive", "var"};
  case ReadingKind::Voltage:
    return {"voltage", "V"};
  case ReadingKind::Frequency:
    return {"frequency", "Hz"};
  default:
    return {"power_real", "W"};
  }
}

unsigned char uom(ReadingKind kind) {
  switch (kind) {
  case ReadingKind::ReactivePower:
    return uomVars;
  case ReadingKind::Voltage:
    return uomVolts;
  case ReadingKind::Frequency:
    return uomHertz;
  default:
    return uomWatts;
  }
}

// Voltage is to neutral: AN, BN, CN, and AN on a single phase (CSIP-AUS
// v1.1a, Annex A). Power and frequency are the whole site's or DER's.
unsigned char phaseCode(ReadingKind kind, Phase phase) {
  switch (phase) {
  case Phase::A:
    return 129;
  case Phase::B:
    return 65;
  case Phase::C:
    return 33;
  default:
    return kind == ReadingKind::Voltage ? 129 : 0;
  }
}

// Volts in tenths and hertz in hundredths, which keeps them within an Int16.
std::optional<int> fixedPowerOfTen(ReadingKind kind) {
  switch (kind) {
  case ReadingKind::Voltage:
    return -1;
  case ReadingKind::Frequency:
    return -2;
  default:
    return std::nullopt;
  }
}

// IEEE 2030.5's PowerOfTenMultiplierType.
constexpr int minPowerOfTen = -9;
constexpr int maxPowerOfTen = 9;

int boundPowerOfTen(const ReadingRow &row, const std::string &where) {
  if (const auto fixed = fixedPowerOfTen(row.reading)) {
    if (row.powerOfTen) {
      throw std::invalid_argument(
          where + " has a power of ten, " + std::to_string(*row.powerOfTen) +
          ", but is always sent at " + std::to_string(*fixed));
    }
    return *fixed;
  }
  if (!row.powerOfTen) {
    throw std::invalid_argument(where + " has no power of ten to send it at");
  }
  if (*row.powerOfTen < minPowerOfTen || *row.powerOfTen > maxPowerOfTen) {
    throw std::invalid_argument(where + " has a power of ten of " +
                                std::to_string(*row.powerOfTen) +
                                ", outside -9 to 9");
  }
  return *row.powerOfTen;
}

std::string phaseName(Phase phase) {
  switch (phase) {
  case Phase::A:
    return "A";
  case Phase::B:
    return "B";
  case Phase::C:
    return "C";
  default:
    return "";
  }
}

std::string readingName(ReadingKind kind) {
  switch (kind) {
  case ReadingKind::ReactivePower:
    return "reactive power";
  case ReadingKind::Voltage:
    return "voltage";
  case ReadingKind::Frequency:
    return "frequency";
  default:
    return "real power";
  }
}

std::string hexDigits(std::uint64_t value, int digits) {
  char buffer[17];
  std::snprintf(buffer, sizeof buffer, "%0*llX", digits,
                static_cast<unsigned long long>(value));
  return buffer;
}

std::string mrid(const std::string &seed, std::uint32_t pen) {
  if (pen > maxPen) {
    throw std::invalid_argument("the PEN " + std::to_string(pen) +
                                " is longer than the 8 digits an mRID ends in");
  }
  std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
  crypto_hash_sha256(hash.data(),
                     reinterpret_cast<const unsigned char *>(seed.data()),
                     seed.size());
  std::string hex;
  for (std::size_t i = 0; i < 12; ++i) {
    hex += hexDigits(hash[i], 2);
  }
  char decimal[9];
  std::snprintf(decimal, sizeof decimal, "%08u", static_cast<unsigned>(pen));
  return hex + decimal;
}

long long seconds(Timestamp at) { return at.time_since_epoch().count(); }

// A rating or setting as IEEE 2030.5 writes it: a value of `limit` at most,
// at the smallest power of ten that keeps it within.
template <typename T, typename Value, typename Raw>
T scaledQuantity(double value, double limit) {
  int multiplier = 0;
  double scaled = std::round(value);
  while (std::abs(scaled) > limit && multiplier < 9) {
    ++multiplier;
    scaled = std::round(value / std::pow(10.0, multiplier));
  }
  return T(sep::PowerOfTenMultiplierType(static_cast<signed char>(multiplier)),
           Value(static_cast<Raw>(scaled)));
}

sep::ActivePower activePower(double watts) {
  return scaledQuantity<sep::ActivePower, sep::Int16, short>(watts, 32767);
}
sep::ReactivePower reactivePower(double vars) {
  return scaledQuantity<sep::ReactivePower, sep::Int16, short>(vars, 32767);
}
sep::ApparentPower apparentPower(double voltAmperes) {
  return scaledQuantity<sep::ApparentPower, sep::UInt16, unsigned short>(voltAmperes, 65535);
}
sep::VoltageRMS voltage(double volts) {
  return scaledQuantity<sep::VoltageRMS, sep::UInt16, unsigned short>(volts, 65535);
}
sep::WattHour wattHours(double wattHours) {
  return scaledQuantity<sep::WattHour, sep::UInt16, unsigned short>(wattHours, 65535);
}

const std::map<std::string, std::uint32_t> &opModBits() {
  // DERControlType: opModConnect, opModEnergize, opModMaxLimW.
  static const std::map<std::string, std::uint32_t> bits = {
      {"connect", 1u << 2}, {"energize", 1u << 3}, {"max_output", 1u << 20}};
  return bits;
}

const std::map<std::string, std::uint8_t> &doeBits() {
  // csipaus:DOEControlType: opModExpLimW, opModImpLimW, opModGenLimW,
  // opModLoadLimW.
  static const std::map<std::string, std::uint8_t> bits = {
      {"export", 1u << 0}, {"import", 1u << 1}, {"generation", 1u << 2},
      {"load", 1u << 3}};
  return bits;
}

nlohmann::json entry(const std::string &rid, double value, Timestamp at) {
  return {{"rid", rid},
          {"dtstart", isoTime(at)},
          {"duration_seconds", 0},
          {"value", value},
          {"quality", "good"}};
}

void addIfSet(nlohmann::json &payload, const std::string &rid,
              const std::optional<double> &value, Timestamp at) {
  if (value) {
    payload.push_back(entry(rid, *value, at));
  }
}

} // namespace

std::vector<BoundReading> bindReadings(const std::vector<ReadingRow> &rows,
                                       const std::vector<TelemetryPoint> &points) {
  std::vector<BoundReading> bound;
  for (const auto &row : rows) {
    BoundReading reading{row.mirror, row.reading, row.phase, 0, {}};
    const auto where = describeReading(reading);
    reading.powerOfTen = boundPowerOfTen(row, where);
    const auto found =
        std::find_if(points.begin(), points.end(), [&](const auto &point) {
          return point.pointId == row.pointId;
        });
    if (found == points.end()) {
      throw std::invalid_argument(where + " is read from telemetry point " +
                                  row.pointId +
                                  ", which the connection does not have");
    }
    const auto expected = pointKind(row.reading);
    if (found->item.kind != expected.kind || found->item.units != expected.units ||
        found->item.scale != "none") {
      throw std::invalid_argument(
          where + " needs a " + expected.kind + " point in " + expected.units +
          ", but telemetry point " + row.pointId + " is " + found->item.kind +
          " in " + (found->item.scale == "none" ? "" : found->item.scale) +
          found->item.units);
    }
    reading.point = *found;
    bound.push_back(std::move(reading));
  }
  return bound;
}

StatePoints bindState(const DerDescription &der,
                      const std::vector<TelemetryPoint> &points) {
  const auto bind = [&points](const std::optional<std::string> &pointId,
                              const std::string &what)
      -> std::optional<TelemetryPoint> {
    if (!pointId) {
      return std::nullopt;
    }
    const auto found =
        std::find_if(points.begin(), points.end(), [&](const auto &point) {
          return point.pointId == *pointId;
        });
    if (found == points.end()) {
      throw std::invalid_argument("The DER's " + what +
                                  " state is read from telemetry point " +
                                  *pointId +
                                  ", which the connection does not have");
    }
    if (found->item.kind != "state") {
      throw std::invalid_argument("The DER's " + what +
                                  " state needs a state point, but telemetry "
                                  "point " +
                                  *pointId + " is " + found->item.kind);
    }
    return *found;
  };
  return {bind(der.connectedPointId, "connected"),
          bind(der.operatingPointId, "operating")};
}

std::string readingId(const BoundReading &reading) {
  const auto phase = toString(reading.phase);
  return toString(reading.kind) + (reading.phase == Phase::None ? "" : "_" + phase);
}

std::string describeMirror(Mirror mirror) {
  return mirror == Mirror::Site ? "Site" : "DER";
}

std::string describeReading(const BoundReading &reading) {
  auto text = describeMirror(reading.mirror) + " " + readingName(reading.kind);
  if (reading.phase != Phase::None) {
    text += " phase " + phaseName(reading.phase);
  }
  return text;
}

std::string mirrorMrid(const std::string &lfdi, Mirror mirror,
                       std::uint32_t pen) {
  return mrid("mirror:" + lfdi + ":" + toString(mirror), pen);
}

std::string readingMrid(const std::string &lfdi, const BoundReading &reading,
                        std::uint32_t pen) {
  return mrid("reading:" + lfdi + ":" + toString(reading.mirror) + ":" +
                  readingId(reading),
              pen);
}

std::optional<long long> sentValue(int powerOfTen, double value) {
  const auto scaled = std::round(value / std::pow(10.0, powerOfTen));
  if (!std::isfinite(scaled) || scaled < std::numeric_limits<short>::min() ||
      scaled > std::numeric_limits<short>::max()) {
    return std::nullopt;
  }
  return static_cast<long long>(scaled);
}

double receivedValue(int powerOfTen, long long sent) {
  return static_cast<double>(sent) * std::pow(10.0, powerOfTen);
}

sep::MirrorUsagePoint mirrorUsagePoint(Mirror mirror,
                                       const std::vector<BoundReading> &readings,
                                       const std::string &lfdi,
                                       std::uint32_t pen) {
  sep::MirrorUsagePoint point(
      sep::mRIDType(hexBinary(mirrorMrid(lfdi, mirror, pen))),
      sep::RoleFlagsType(hexBinary(mirror == Mirror::Site ? siteRoleFlags
                                                          : derRoleFlags)),
      sep::ServiceKind(serviceElectricity), sep::UInt8(statusOn),
      sep::HexBinary160(hexBinary(lfdi)));
  point.description(sep::String32(describeMirror(mirror)));
  for (const auto &reading : readings) {
    sep::MirrorMeterReading meterReading(
        sep::mRIDType(hexBinary(readingMrid(lfdi, reading, pen))));
    meterReading.description(sep::String32(describeReading(reading)));
    sep::ReadingType type;
    type.dataQualifier(sep::DataQualifierType(qualifierAverage));
    type.kind(sep::KindType(kindPower));
    type.phase(sep::PhaseCode(phaseCode(reading.kind, reading.phase)));
    type.powerOfTenMultiplier(sep::PowerOfTenMultiplierType(
        static_cast<signed char>(reading.powerOfTen)));
    type.uom(sep::UomType(uom(reading.kind)));
    meterReading.ReadingType(type);
    point.MirrorMeterReading().push_back(meterReading);
  }
  return point;
}

sep::MirrorMeterReadingList
readingsDocument(const std::vector<std::pair<BoundReading, datum::Sample>> &values,
                 const std::string &lfdi, std::uint32_t pen, Timestamp start,
                 std::chrono::seconds length) {
  std::vector<sep::MirrorMeterReading> readings;
  for (const auto &[reading, sample] : values) {
    if (!sample.value || sample.quality != datum::Quality::Good) {
      continue;
    }
    const auto sent = sentValue(reading.powerOfTen, *sample.value);
    if (!sent) {
      continue;
    }
    sep::MirrorMeterReading meterReading(
        sep::mRIDType(hexBinary(readingMrid(lfdi, reading, pen))));
    meterReading.description(sep::String32(describeReading(reading)));
    sep::Reading value;
    value.qualityFlags(sep::HexBinary16(hexBinary(goodQuality)));
    value.timePeriod(sep::DateTimeInterval(
        sep::UInt32(static_cast<unsigned int>(length.count())),
        sep::TimeType(seconds(start))));
    value.value(sep::Int48(*sent));
    meterReading.Reading(value);
    readings.push_back(std::move(meterReading));
  }
  const auto count = static_cast<unsigned int>(readings.size());
  sep::MirrorMeterReadingList list(count, count);
  for (auto &reading : readings) {
    list.MirrorMeterReading().push_back(std::move(reading));
  }
  return list;
}

Timestamp windowStart(Timestamp at, std::chrono::seconds length) {
  const auto count = length.count() > 0 ? length.count() : 1;
  const auto epoch = seconds(at);
  const auto floored = epoch - ((epoch % count) + count) % count;
  return Timestamp(std::chrono::seconds(floored));
}

std::uint32_t modeBits(const std::vector<std::string> &modes) {
  std::uint32_t bits = 0;
  for (const auto &mode : modes) {
    if (const auto found = opModBits().find(mode); found != opModBits().end()) {
      bits |= found->second;
    }
  }
  return bits;
}

std::uint8_t doeModeBits(const std::vector<std::string> &modes) {
  std::uint8_t bits = 0;
  for (const auto &mode : modes) {
    if (const auto found = doeBits().find(mode); found != doeBits().end()) {
      bits = static_cast<std::uint8_t>(bits | found->second);
    }
  }
  return bits;
}

sep::DERCapability capabilityDocument(const DerDescription &der) {
  sep::DERCapability capability(
      sep::DERControlType(hexBinary(hexDigits(modeBits(der.modesSupported), 8))),
      activePower(der.rtgMaxW),
      sep::DERType(static_cast<unsigned char>(der.type)),
      ext::DOEControlType(
          hexBinary(hexDigits(doeModeBits(der.modesSupported), 2))));
  if (der.rtgMaxVa) {
    capability.rtgMaxVA(apparentPower(*der.rtgMaxVa));
  }
  if (der.rtgMaxVar) {
    capability.rtgMaxVar(reactivePower(*der.rtgMaxVar));
  }
  if (der.rtgMaxVarNeg) {
    capability.rtgMaxVarNeg(reactivePower(*der.rtgMaxVarNeg));
  }
  if (der.rtgMaxChargeRateW) {
    capability.rtgMaxChargeRateW(activePower(*der.rtgMaxChargeRateW));
  }
  if (der.rtgMaxDischargeRateW) {
    capability.rtgMaxDischargeRateW(activePower(*der.rtgMaxDischargeRateW));
  }
  if (der.rtgMaxWh) {
    capability.rtgMaxWh(wattHours(*der.rtgMaxWh));
  }
  if (der.rtgVNom) {
    capability.rtgVNom(voltage(*der.rtgVNom));
  }
  return capability;
}

sep::DERSettings settingsDocument(const DerDescription &der, int setGradW,
                                  Timestamp updatedTime) {
  sep::DERSettings settings(
      sep::UInt16(static_cast<unsigned short>(setGradW)),
      activePower(der.setMaxW.value_or(der.rtgMaxW)),
      sep::TimeType(seconds(updatedTime)));
  settings.modesEnabled(
      sep::DERControlType(hexBinary(hexDigits(modeBits(der.modesEnabled), 8))));
  settings.doeModesEnabled(
      ext::DOEControlType(hexBinary(hexDigits(doeModeBits(der.modesEnabled), 2))));
  if (const auto value = der.setMaxVa.has_value() ? der.setMaxVa : der.rtgMaxVa) {
    settings.setMaxVA(apparentPower(*value));
  }
  if (const auto value = der.setMaxVar.has_value() ? der.setMaxVar : der.rtgMaxVar) {
    settings.setMaxVar(reactivePower(*value));
  }
  if (der.rtgMaxVarNeg) {
    settings.setMaxVarNeg(reactivePower(*der.rtgMaxVarNeg));
  }
  if (const auto value = der.setMaxChargeRateW.has_value()
                             ? der.setMaxChargeRateW
                             : der.rtgMaxChargeRateW) {
    settings.setMaxChargeRateW(activePower(*value));
  }
  if (const auto value = der.setMaxDischargeRateW.has_value()
                             ? der.setMaxDischargeRateW
                             : der.rtgMaxDischargeRateW) {
    settings.setMaxDischargeRateW(activePower(*value));
  }
  if (der.rtgMaxWh) {
    settings.setMaxWh(wattHours(*der.rtgMaxWh));
  }
  if (der.rtgVNom) {
    settings.setVNom(voltage(*der.rtgVNom));
  }
  return settings;
}

datum::Sample stateSample(const datum::Sample &sample) {
  if (sample.value && *sample.value != 0.0 && *sample.value != 1.0) {
    return {std::nullopt, datum::Quality::Unreadable};
  }
  return sample;
}

DerState derState(const datum::Sample &connectedState,
                  const datum::Sample &operatingState) {
  const auto isConnected = stateSample(connectedState);
  const auto isOperating = stateSample(operatingState);
  const auto isOne = [](const datum::Sample &sample) {
    return sample.value == 1.0;
  };
  const auto isGood = [](const datum::Sample &sample) {
    return sample.value && sample.quality == datum::Quality::Good;
  };
  const bool current = isGood(isConnected) && isGood(isOperating);
  // A DER off the grid is not available to it, whatever its readings.
  return {(isOne(isConnected) ? connected : 0u) |
              (isOne(isOperating) ? operating : 0u) |
              (current ? (isOne(isConnected) ? available : 0u) : faulted),
          isOperating.value
              ? std::optional<int>(isOne(isOperating) ? 2 : 1)
              : std::nullopt};
}

sep::DERStatus statusDocument(const DerState &state, Timestamp at) {
  const sep::TimeType time(seconds(at));
  sep::DERStatus status(time);
  status.genConnectStatus(sep::ConnectStatusType(
      time, sep::HexBinary8(hexBinary(hexDigits(state.connectStatus, 2)))));
  if (state.operationalMode) {
    status.operationalModeStatus(sep::OperationalModeStatusType(
        time, sep::UInt8(static_cast<unsigned char>(*state.operationalMode))));
  }
  return status;
}

nlohmann::json capabilityPayload(const DerDescription &der, Timestamp at) {
  auto payload = nlohmann::json::array();
  payload.push_back(entry("type", der.type, at));
  payload.push_back(entry("rtg_max_w", der.rtgMaxW, at));
  addIfSet(payload, "rtg_max_va", der.rtgMaxVa, at);
  addIfSet(payload, "rtg_max_var", der.rtgMaxVar, at);
  addIfSet(payload, "rtg_max_var_neg", der.rtgMaxVarNeg, at);
  addIfSet(payload, "rtg_max_charge_rate_w", der.rtgMaxChargeRateW, at);
  addIfSet(payload, "rtg_max_discharge_rate_w", der.rtgMaxDischargeRateW, at);
  addIfSet(payload, "rtg_max_wh", der.rtgMaxWh, at);
  addIfSet(payload, "rtg_v_nom", der.rtgVNom, at);
  payload.push_back(entry("modes_supported", modeBits(der.modesSupported), at));
  payload.push_back(
      entry("doe_modes_supported", doeModeBits(der.modesSupported), at));
  return payload;
}

nlohmann::json settingsPayload(const DerDescription &der, int setGradW,
                               Timestamp updatedTime, Timestamp at) {
  auto payload = nlohmann::json::array();
  payload.push_back(entry("set_max_w", der.setMaxW.value_or(der.rtgMaxW), at));
  addIfSet(payload, "set_max_va",
           der.setMaxVa.has_value() ? der.setMaxVa : der.rtgMaxVa, at);
  addIfSet(payload, "set_max_var",
           der.setMaxVar.has_value() ? der.setMaxVar : der.rtgMaxVar, at);
  addIfSet(payload, "set_max_charge_rate_w",
           der.setMaxChargeRateW.has_value() ? der.setMaxChargeRateW
                                             : der.rtgMaxChargeRateW,
           at);
  addIfSet(payload, "set_max_discharge_rate_w",
           der.setMaxDischargeRateW.has_value() ? der.setMaxDischargeRateW
                                                : der.rtgMaxDischargeRateW,
           at);
  payload.push_back(entry("set_grad_w", setGradW, at));
  payload.push_back(entry("modes_enabled", modeBits(der.modesEnabled), at));
  payload.push_back(
      entry("doe_modes_enabled", doeModeBits(der.modesEnabled), at));
  payload.push_back(
      entry("updated_time", static_cast<double>(seconds(updatedTime)), at));
  return payload;
}

nlohmann::json statusPayload(const DerState &state,
                             const StateReading &connectedState,
                             const StateReading &operatingState, Timestamp at) {
  auto payload = nlohmann::json::array();
  payload.push_back(entry("gen_connect_status", state.connectStatus, at));
  if (state.operationalMode) {
    payload.push_back(
        entry("operational_mode_status", *state.operationalMode, at));
  }
  for (const auto &[rid, input] :
       {std::pair{"connected", &connectedState},
        std::pair{"operating", &operatingState}}) {
    const auto sample = stateSample(input->sample);
    payload.push_back(
        {{"rid", rid},
         {"point_id", input->point.pointId},
         {"dtstart", isoTime(at)},
         {"duration_seconds", 0},
         {"value", sample.value ? nlohmann::json(*sample.value)
                                : nlohmann::json(nullptr)},
         {"quality", datum::toString(sample.quality)},
         {"source", sourceRecord(input->point.source)}});
  }
  return payload;
}

} // namespace grid_services::csipaus
