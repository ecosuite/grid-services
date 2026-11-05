#include "protocols/csipaus/controls.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace grid_services::csipaus {

namespace {

std::string formatWatts(double value) {
  char text[32];
  if (std::abs(value) >= 1000) {
    std::snprintf(text, sizeof text, "%g kW", value / 1000);
  } else {
    std::snprintf(text, sizeof text, "%g W", value);
  }
  return text;
}

} // namespace

double watts(const sep::ActivePower &power) {
  return static_cast<double>(power.value()) *
         std::pow(10.0, static_cast<int>(power.multiplier()));
}

std::string describeControlBase(const sep::DERControlBase &base) {
  std::vector<std::string> parts;
  if (base.opModExpLimW()) {
    parts.push_back("export " + formatWatts(watts(*base.opModExpLimW())));
  }
  if (base.opModImpLimW()) {
    parts.push_back("import " + formatWatts(watts(*base.opModImpLimW())));
  }
  if (base.opModGenLimW()) {
    parts.push_back("generation " + formatWatts(watts(*base.opModGenLimW())));
  }
  if (base.opModLoadLimW()) {
    parts.push_back("load " + formatWatts(watts(*base.opModLoadLimW())));
  }
  if (base.opModMaxLimW()) {
    // PerCent is in hundredths of a percent.
    char text[32];
    std::snprintf(text, sizeof text, "max %g%%",
                  static_cast<unsigned short>(*base.opModMaxLimW()) / 100.0);
    parts.push_back(text);
  }
  if (base.opModEnergize()) {
    parts.push_back(std::string("energize ") +
                    (*base.opModEnergize() ? "on" : "off"));
  }
  if (base.opModConnect()) {
    parts.push_back(std::string("connect ") +
                    (*base.opModConnect() ? "on" : "off"));
  }
  if (parts.empty()) {
    return "no limits";
  }
  std::string text;
  for (const auto &part : parts) {
    text += (text.empty() ? "" : ", ") + part;
  }
  return text;
}

std::vector<Signal> capSignals(const sep::DERControlBase &base,
                               int durationSeconds) {
  std::vector<Signal> signals;
  const auto add = [&](const std::string &name, double value) {
    signals.push_back({name, "limit", name, {{0, durationSeconds, value}}});
  };
  if (base.opModExpLimW()) {
    add("export", watts(*base.opModExpLimW()));
  }
  if (base.opModImpLimW()) {
    add("import", watts(*base.opModImpLimW()));
  }
  if (base.opModGenLimW()) {
    add("generation", watts(*base.opModGenLimW()));
  }
  if (base.opModLoadLimW()) {
    add("load", watts(*base.opModLoadLimW()));
  }
  if (base.opModMaxLimW()) {
    // PerCent is in hundredths of a percent.
    add("max_output",
        static_cast<unsigned short>(*base.opModMaxLimW()) / 100.0);
  }
  if (base.opModEnergize()) {
    add("energize", *base.opModEnergize() ? 1 : 0);
  }
  if (base.opModConnect()) {
    add("connect", *base.opModConnect() ? 1 : 0);
  }
  return signals;
}

ProgramStatus controlStatus(int currentStatus) {
  switch (currentStatus) {
  case 1:
    return ProgramStatus::Active;
  case 2:
  case 3: // Cancelled with randomization.
    return ProgramStatus::Cancelled;
  case 4:
    return ProgramStatus::Superseded;
  default:
    return ProgramStatus::Pending;
  }
}

namespace {

int firstByte(const xml_schema::hex_binary &value) {
  return value.size() == 0 ? 0 : static_cast<unsigned char>(value.data()[0]);
}

} // namespace

ObservedEvent observeControl(const sep::DERControl &control,
                             const ProgramInfo &program) {
  const auto duration = static_cast<int>(control.interval().duration());
  ObservedEvent observed;
  auto &event = observed.event;
  event.programEventId = hexString(control.mRID());
  event.status =
      controlStatus(static_cast<int>(control.EventStatus().currentStatus()));
  event.startAt = Timestamp(std::chrono::seconds(
      static_cast<long long>(control.interval().start())));
  event.durationSeconds = duration;
  event.signals = capSignals(control.DERControlBase(), duration);
  if (control.description()) {
    event.comment = std::string(*control.description());
  }
  event.declinable = false;

  auto &detail = observed.detail;
  detail.kind = CsipausEventKind::Control;
  detail.href = control.href() ? std::string(*control.href()) : "";
  detail.mrid = event.programEventId;
  detail.programMrid = program.mrid;
  detail.programDescription = program.description;
  detail.primacy = program.primacy;
  detail.creationTime = Timestamp(
      std::chrono::seconds(static_cast<long long>(control.creationTime())));
  if (control.randomizeStart()) {
    detail.randomizeStartSeconds =
        static_cast<short>(*control.randomizeStart());
  }
  if (control.randomizeDuration()) {
    detail.randomizeDurationSeconds =
        static_cast<short>(*control.randomizeDuration());
  }
  if (control.replyTo()) {
    detail.replyTo = std::string(*control.replyTo());
  }
  detail.responseRequired = firstByte(control.responseRequired());
  event.responseRequired = detail.replyTo && detail.responseRequired != 0;
  if (const auto &rampTms = control.DERControlBase().rampTms()) {
    observed.ramp.rampTms = static_cast<unsigned short>(*rampTms);
  }
  return observed;
}

std::string defaultEventId(const std::string &programMrid) {
  return "default:" + programMrid;
}

ObservedEvent observeDefault(const sep::DefaultDERControl &defaults,
                             const std::string &href,
                             const ProgramInfo &program, Timestamp since) {
  ObservedEvent observed;
  auto &event = observed.event;
  event.programEventId = defaultEventId(program.mrid);
  event.status = ProgramStatus::Active;
  event.startAt = since;
  event.durationSeconds = 0;
  event.signals = capSignals(defaults.DERControlBase(), 0);
  if (defaults.description()) {
    event.comment = std::string(*defaults.description());
  }
  event.responseRequired = false;
  event.declinable = false;

  auto &detail = observed.detail;
  detail.kind = CsipausEventKind::Default;
  detail.href = href;
  detail.mrid = hexString(defaults.mRID());
  detail.programMrid = program.mrid;
  detail.programDescription = program.description;
  detail.primacy = program.primacy;
  if (const auto &setGradW = defaults.setGradW()) {
    observed.ramp.setGradW = static_cast<unsigned short>(*setGradW);
  }
  return observed;
}

} // namespace grid_services::csipaus
