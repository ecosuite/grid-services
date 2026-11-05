#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/telemetry.hpp"
#include "core/types.hpp"

namespace grid_services {

class RunnerStore;

struct EventUpdate {
  KnownEvent event;
  // False when the program sent a modification we already have.
  bool changed = false;
};

// What an adapter reports back while talking to a program. The runner
// implements it on top of the store; it owns every decision that is not
// protocol-specific, such as which opt answer to give.
class ProgramEventSink {
public:
  virtual ~ProgramEventSink() = default;

  // Program events for this connection that have not completed or been
  // cancelled.
  virtual std::vector<KnownEvent> openEvents() = 0;

  // The stored program event with this id, whatever its status.
  virtual std::optional<KnownEvent>
  knownEvent(const std::string &programEventId) = 0;

  // Stores a new or modified program event and returns it with the opt
  // answer to give.
  virtual EventUpdate eventReceived(const NormalizedEvent &event) = 0;

  // The program stopped listing an open event.
  virtual void eventEnded(const KnownEvent &event, ProgramStatus status) = 0;

  // The program changed an event's status without modifying it, as a
  // CSIP-AUS server does when a control starts, is cancelled or is
  // superseded.
  virtual void eventStatusChanged(const KnownEvent &event,
                                  ProgramStatus status) = 0;

  virtual void responseSent(const KnownEvent &event, EventAnswer answer,
                            int modificationNumber, bool ok,
                            const std::string &message) = 0;

  virtual void activity(ActivityEntry entry) = 0;

  // A report was sent to the program, or failed to be.
  virtual void reportDelivered(const ReportDelivery &delivery) = 0;
};

enum class PollOutcome { Ok, RegistrationCancelled, ReregistrationRequested };

struct PollResult {
  PollOutcome outcome = PollOutcome::Ok;
  // What happened, when the poll was more than a routine check-in. The
  // runner records it as connection activity with the exchange attached.
  std::optional<std::string> summary;
  // The program sent a message, so more may be queued. The runner polls
  // again straight away instead of waiting for the interval.
  bool moreQueued = false;
  // The program's poll interval as this poll found it, when the protocol
  // learns it from polling. The runner stores it in place of registering's
  // when it differs.
  std::optional<int> pollIntervalSeconds;
};

// One protocol's view of a single connection. Every method runs on the
// connection's runner thread and throws AdapterError on failure.
//
// Adapters do not write connection activity themselves. Each operation
// starts a fresh exchange record, and the runner takes it to attach to the
// activity entry it writes.
class Adapter {
public:
  virtual ~Adapter() = default;

  // Confirms a registration saved before a restart. Returns the registration
  // as the program now reports it, or nothing when the program no longer has
  // it.
  virtual std::optional<RegistrationInfo>
  resume(const ConnectionState &state, ProgramEventSink &sink) = 0;

  virtual RegistrationInfo registerConnection(ProgramEventSink &sink) = 0;

  virtual void unregisterConnection(ProgramEventSink &sink) = 0;

  // Checks reachability and authentication without registering. Returns
  // what the program said, in words and as values.
  virtual TestResult test(ProgramEventSink &sink) = 0;

  virtual PollResult poll(ProgramEventSink &sink) = 0;

  // Tells the program our opt answer for an event.
  virtual void respond(const KnownEvent &event, Opt opt,
                       ProgramEventSink &sink) = 0;

  // Time-driven protocol work, such as periodic reports.
  virtual void runDueWork(ProgramEventSink &sink) = 0;

  // How long until runDueWork has something to do, when the adapter knows;
  // the runner runs it then rather than waiting for its heartbeat.
  virtual std::optional<std::chrono::milliseconds> dueWorkIn() const {
    return std::nullopt;
  }

  // Whether due work found something that wants the program polled now,
  // whatever the poll interval or a backoff says.
  virtual bool pollWanted() const { return false; }

  // Whether each wait between polls is lengthened by the connection's poll
  // jitter. A protocol whose servers spread their clients themselves polls
  // at the interval exactly.
  virtual bool jittersPolls() const { return true; }

  // How long to wait before trying the program again after this many failed
  // attempts in a row, when the protocol has its own rule, given the
  // interval the runner polls at. Nothing keeps the runner's backoff.
  virtual std::optional<std::chrono::milliseconds>
  retryDelay(int /*failures*/, std::chrono::milliseconds /*pollInterval*/) {
    return std::nullopt;
  }

  virtual void setParticipation(Participation participation) = 0;

  virtual std::optional<std::chrono::seconds> requestedPollInterval() const = 0;

  // What pausing or resuming participation does, as an operator reads it.
  // By default new events are answered with `defaultOpt`, or opt out while
  // paused.
  virtual std::string describeParticipation(Participation participation,
                                            Opt defaultOpt) const;

  // The program's id for the connection, as an operator reads it, such as
  // "VEN ven-1".
  virtual std::string describeClient(const std::string &clientId) const {
    return clientId;
  }

  // The raw messages exchanged by the most recent operation, and clears
  // them. Null when there were none. Otherwise an array, oldest first, of
  //
  //   {"direction": "to_program" | "from_program",
  //    "at": "2026-07-14T21:00:00.000Z",
  //    "format": "xml",
  //    "body": "..."}
  //
  // where `format` says what `body` holds.
  virtual nlohmann::json takeExchange() = 0;
};

struct AdapterContext {
  const ConnectionConfig &config;
  RunnerStore &store;
  // Set when the runner is stopping; adapters abort in-flight requests.
  const std::atomic<bool> &abort;
};

using AdapterFactory =
    std::function<std::unique_ptr<Adapter>(const AdapterContext &)>;

class AdapterRegistry {
public:
  void add(const std::string &protocol, AdapterFactory factory);

  // Throws AdapterError::permanent for an unknown protocol.
  std::unique_ptr<Adapter> create(const AdapterContext &context) const;

private:
  std::map<std::string, AdapterFactory> factories_;
};

} // namespace grid_services
