#include "core/event_sink.hpp"

namespace grid_services {

namespace {

std::string receivedMessage(const NormalizedEvent &event,
                            const std::optional<KnownEvent> &previous,
                            Opt opt) {
  const auto window = describeWindow(event.startAt, event.durationSeconds);
  if (event.status == ProgramStatus::Cancelled) {
    return "Event cancelled by the program: " + window;
  }
  if (previous) {
    return "Event updated by the program (modification " +
           std::to_string(event.modificationNumber) + "): " + window;
  }
  if (!event.declinable) {
    return "Event received from the program: " + window + ".";
  }
  return "Event received from the program: " + window + ". Opt " +
         toString(opt) + ".";
}

std::optional<std::string> statusMessage(ProgramStatus status,
                                         const std::string &window) {
  switch (status) {
  case ProgramStatus::Active:
    return "Event started, as the program says: " + window;
  case ProgramStatus::Completed:
    return "Event completed: " + window;
  case ProgramStatus::Cancelled:
    return "Event cancelled by the program: " + window;
  case ProgramStatus::Superseded:
    return "Event replaced by a newer one from the program: " + window;
  case ProgramStatus::Pending:
    return std::nullopt;
  }
  return std::nullopt;
}

std::string answerMessage(const KnownEvent &event, EventAnswer answer,
                          bool ok, const std::string &message) {
  const auto window = describeWindow(event.startAt, event.durationSeconds);
  if (answer == EventAnswer::OptIn || answer == EventAnswer::OptOut) {
    const std::string opt = answer == EventAnswer::OptIn ? "in" : "out";
    return ok ? "Told the program opt " + opt + " for event " + window
              : "Failed to send opt " + opt + " to the program: " + message;
  }
  const auto status = toString(answer);
  return ok ? "Told the program the event was " + status + ": " + window
            : "Failed to tell the program the event was " + status + ": " +
                  message;
}

} // namespace

StoreEventSink::StoreEventSink(RunnerStore &store, Opt defaultOpt,
                               spdlog::logger &log)
    : store_(store), defaultOpt_(defaultOpt), log_(log) {}

void StoreEventSink::setParticipation(Participation participation) {
  participation_ = participation;
}

std::vector<KnownEvent> StoreEventSink::openEvents() {
  return store_.openProgramEvents();
}

std::optional<KnownEvent>
StoreEventSink::knownEvent(const std::string &programEventId) {
  return store_.findProgramEvent(programEventId);
}

EventUpdate StoreEventSink::eventReceived(const NormalizedEvent &event) {
  const auto previous = store_.findProgramEvent(event.programEventId);
  if (previous && previous->modificationNumber >= event.modificationNumber) {
    return {*previous, false};
  }

  Opt opt = defaultOpt_;
  OptSource optSource = OptSource::Default;
  if (!event.declinable) {
    opt = Opt::In;
  } else if (previous) {
    opt = previous->opt;
    optSource = previous->optSource;
  } else if (participation_ == Participation::Paused) {
    opt = Opt::Out;
    optSource = OptSource::Paused;
  }

  const auto stored = store_.upsertProgramEvent(event, opt, optSource);
  store_.appendActivity(
      {.category = ActivityCategory::Events,
       .message = receivedMessage(event, previous, stored.opt),
       .eventId = stored.id});
  log_.info("event {} modification {} stored as {} ({})", event.programEventId,
            event.modificationNumber, stored.id, toString(event.status));
  return {stored, true};
}

void StoreEventSink::eventEnded(const KnownEvent &event,
                                ProgramStatus status) {
  store_.setProgramStatus(event.id, status);
  const auto window = describeWindow(event.startAt, event.durationSeconds);
  const auto message =
      status == ProgramStatus::Completed
          ? "Event completed: " + window
          : "Event withdrawn by the program: " + window;
  store_.appendActivity({.category = ActivityCategory::Events,
                         .message = message,
                         .eventId = event.id});
  log_.info("event {} ended as {}", event.programEventId, toString(status));
}

void StoreEventSink::eventStatusChanged(const KnownEvent &event,
                                        ProgramStatus status) {
  store_.setProgramStatus(event.id, status);
  const auto message = statusMessage(
      status, describeWindow(event.startAt, event.durationSeconds));
  if (message) {
    store_.appendActivity({.category = ActivityCategory::Events,
                           .message = *message,
                           .eventId = event.id});
  }
  log_.info("event {} is now {}", event.programEventId, toString(status));
}

void StoreEventSink::responseSent(const KnownEvent &event, EventAnswer answer,
                                  int modificationNumber, bool ok,
                                  const std::string &message) {
  store_.recordResponse(event.id, answer, modificationNumber, ok, message);
  store_.appendActivity({.category = ActivityCategory::Events,
                         .message = answerMessage(event, answer, ok, message),
                         .eventId = event.id});
}

void StoreEventSink::activity(ActivityEntry entry) {
  store_.appendActivity(entry);
}

void StoreEventSink::reportDelivered(const ReportDelivery &delivery) {
  store_.recordReportDelivery(delivery);
  if (!delivery.ok) {
    const std::string report =
        delivery.programRequestId
            ? "A report for request " + *delivery.programRequestId
            : std::string("A report");
    store_.appendActivity({.category = ActivityCategory::Reports,
                           .message = report + " was not delivered: " +
                                      delivery.message});
  }
}

} // namespace grid_services
