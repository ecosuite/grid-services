#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "protocols/csipaus/controls.hpp"

namespace grid_services::csipaus {

// How far a device moves a control's start (randomizeStart) or end
// (randomizeDuration), within `range` seconds. Each device picks its own,
// and keeps it: it is drawn from the device's LFDI and the control's mRID.
int randomOffset(const std::string &lfdi, const std::string &mrid, int range);

// A control's window as this device keeps it, in the server's time: its
// start moved by up to randomizeStart, and its end by that plus up to
// randomizeDuration. A negative randomizeDuration can leave it empty.
struct EffectiveWindow {
  Timestamp start;
  Timestamp end;
};

EffectiveWindow effectiveWindow(const NormalizedEvent &event,
                                const CsipausEventDetail &detail,
                                const std::string &lfdi);

// The caps in force from what a walk observed: for each cap, its value over
// time, in the server's time.
//
// Only the controls that set a cap compete for it (CSIP-AUS supersedes cap
// by cap). Of the controls in force, the one of the program with the lowest
// primacy wins, then the one created last. Where no control applies, the
// default of the program with the lowest primacy that sets the cap holds,
// from when its event began. Controls that have ended, or that are
// cancelled or superseded, by the server or by supersededControls, take no
// part. Each control's window is moved by the device's own randomization.
//
// Each segment carries the ramp into it (CSIP-AUS v1.2 explainer, section
// 6.1): a control's rampTms, else the setGradW in force; a default's
// segments the setGradW in force.
std::vector<LimitSegment> limitsInForce(const std::vector<ObservedEvent> &observed,
                                        const std::string &lfdi, Timestamp at);

// The setGradW in force: that of the default of the program with the lowest
// primacy that sets one, resolved on its own as a cap is. None when no
// default sets one.
std::optional<int> setGradWInForce(const std::vector<ObservedEvent> &observed);

// The controls that others supersede, by this device's own resolution: those
// still to end, by their effective windows, that set at least one cap and
// win no moment of any of them from `at` until they end. A control the
// server cancelled or superseded is not judged; nor is one that sets no cap.
std::set<std::string> supersededControls(const std::vector<ObservedEvent> &observed,
                                         const std::string &lfdi, Timestamp at);

// Whether a connect or energize cap of 0 holds at `at`: the DER is
// switched off by its caps.
bool switchedOff(const std::vector<LimitSegment> &limits, Timestamp at);

// The programs' defaults among what a walk observed.
std::vector<ObservedEvent> defaultsOf(const std::vector<ObservedEvent> &observed);

} // namespace grid_services::csipaus
