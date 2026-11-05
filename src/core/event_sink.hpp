#pragma once

#include <spdlog/logger.h>

#include "core/adapter.hpp"
#include "core/store.hpp"

namespace grid_services {

// Records what a program tells us and decides opt answers.
//
// Opt rules:
//   - A newly received event gets opt out while participation is paused,
//     and the connection's default opt otherwise.
//   - A modified event keeps whatever opt it already has, so a program
//     modification never undoes an operator's opt out.
//   - An event the site cannot decline is always opted in.
class StoreEventSink : public ProgramEventSink {
public:
  StoreEventSink(RunnerStore &store, Opt defaultOpt, spdlog::logger &log);

  void setParticipation(Participation participation);

  std::vector<KnownEvent> openEvents() override;
  std::optional<KnownEvent>
  knownEvent(const std::string &programEventId) override;
  EventUpdate eventReceived(const NormalizedEvent &event) override;
  void eventEnded(const KnownEvent &event, ProgramStatus status) override;
  void eventStatusChanged(const KnownEvent &event,
                          ProgramStatus status) override;
  void responseSent(const KnownEvent &event, EventAnswer answer,
                    int modificationNumber, bool ok,
                    const std::string &message) override;
  void activity(ActivityEntry entry) override;
  void reportDelivered(const ReportDelivery &delivery) override;

private:
  RunnerStore &store_;
  Opt defaultOpt_;
  Participation participation_ = Participation::Active;
  spdlog::logger &log_;
};

} // namespace grid_services
