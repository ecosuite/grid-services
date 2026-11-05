#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "core/types.hpp"

namespace grid_services::csipaus {

// When each resource is next due to be fetched, by href. A server gives a
// poll rate on lists and some resources, and the rest inherit their
// parent's (CSIP-AUS v1.2 explainer, section 4.2.1): an EndDevice is polled
// at its EndDeviceList's rate, and DERControls at their DERProgramList's.
class ResourceSchedule {
public:
  explicit ResourceSchedule(std::function<Clock::time_point()> clock = nullptr);

  // Never fetched, or its poll rate has passed since.
  bool due(const std::string &href) const;
  // How long until it is due, at least zero. Nothing when it has not been
  // fetched since the schedule was last cleared.
  std::optional<Clock::duration> untilDue(const std::string &href) const;
  void fetched(const std::string &href, std::chrono::seconds pollRate);
  // Every resource becomes due, as after a failure or a new registration.
  void clear();

private:
  std::function<Clock::time_point()> clock_;
  std::map<std::string, Clock::time_point> next_;
};

} // namespace grid_services::csipaus
