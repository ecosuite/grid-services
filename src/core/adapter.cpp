#include "core/adapter.hpp"

#include "core/errors.hpp"

namespace grid_services {

std::string Adapter::describeParticipation(Participation participation,
                                           Opt defaultOpt) const {
  return participation == Participation::Paused
             ? "Participation paused. New events will be answered opt out."
             : "Participation resumed. New events will be answered opt " +
                   toString(defaultOpt) + ".";
}

void AdapterRegistry::add(const std::string &protocol, AdapterFactory factory) {
  factories_[protocol] = std::move(factory);
}

std::unique_ptr<Adapter>
AdapterRegistry::create(const AdapterContext &context) const {
  const auto found = factories_.find(context.config.protocol);
  if (found == factories_.end()) {
    throw AdapterError::permanent("no adapter for protocol '" +
                                  context.config.protocol + "'");
  }
  return found->second(context);
}

} // namespace grid_services
