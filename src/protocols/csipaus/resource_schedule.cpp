#include "protocols/csipaus/resource_schedule.hpp"

#include <algorithm>

namespace grid_services::csipaus {

ResourceSchedule::ResourceSchedule(std::function<Clock::time_point()> clock)
    : clock_(clock ? std::move(clock) : [] { return Clock::now(); }) {}

bool ResourceSchedule::due(const std::string &href) const {
  const auto found = next_.find(href);
  return found == next_.end() || clock_() >= found->second;
}

std::optional<Clock::duration>
ResourceSchedule::untilDue(const std::string &href) const {
  const auto found = next_.find(href);
  if (found == next_.end()) {
    return std::nullopt;
  }
  return std::max(found->second - clock_(), Clock::duration::zero());
}

void ResourceSchedule::fetched(const std::string &href,
                               std::chrono::seconds pollRate) {
  next_[href] = clock_() + pollRate;
}

void ResourceSchedule::clear() { next_.clear(); }

} // namespace grid_services::csipaus
