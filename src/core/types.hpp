#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace grid_services {

using ConnectionId = std::int64_t;
using EventRowId = std::int64_t;
using CommandId = std::int64_t;
using Timestamp = std::chrono::sys_seconds;
using Clock = std::chrono::steady_clock;

Timestamp now();

enum class Opt { In, Out };
enum class OptSource { Default, Paused, Operator };
enum class Participation { Active, Paused };
enum class Registration { Unregistered, Registered, Error };
enum class ProgramStatus { Pending, Active, Completed, Cancelled, Superseded };
// An answer given to the program about one of its events: an opt answer
// (OpenADR), or a report of what became of the event (CSIP-AUS).
enum class EventAnswer {
  OptIn,
  OptOut,
  Received,
  Started,
  Completed,
  Cancelled,
  Superseded
};
enum class CommandKind {
  Register,
  Unregister,
  Test,
  Pause,
  Resume,
  ReloadConfig,
  OptOut,
  OptIn
};
enum class ActivityCategory { Connection, Events, Reports };

std::string toString(Opt value);
std::string toString(OptSource value);
std::string toString(Participation value);
std::string toString(Registration value);
std::string toString(ProgramStatus value);
std::string toString(EventAnswer value);
std::string toString(CommandKind value);
std::string toString(ActivityCategory value);

Opt parseOpt(const std::string &value);
OptSource parseOptSource(const std::string &value);
Participation parseParticipation(const std::string &value);
Registration parseRegistration(const std::string &value);
ProgramStatus parseProgramStatus(const std::string &value);
EventAnswer parseEventAnswer(const std::string &value);
CommandKind parseCommandKind(const std::string &value);

bool isTerminal(ProgramStatus status);

EventAnswer optAnswer(Opt opt);

struct ConnectionSummary {
  ConnectionId id = 0;
  std::string protocol;
  bool enabled = true;
  int configVersion = 0;
  bool archived = false;
  // Registered with the program, as far as the database knows.
  bool registered = false;
};

struct ConnectionConfig {
  ConnectionId id = 0;
  std::string siteCode;
  std::string protocol;
  bool enabled = true;
  Opt defaultOpt = Opt::In;
  std::optional<int> pollIntervalSeconds;
  // Each wait between polls is lengthened by a random part of up to this
  // fraction of the poll interval, so connections polling on the same
  // interval drift apart (OpenADR 2.0b section 9.1.8), where the protocol's
  // adapter jitters its polls.
  double pollJitter = 0.1;
  int configVersion = 0;
  bool archived = false;
};

// True when the connection is meant to run: enabled and not archived.
bool isActive(const ConnectionConfig &config);
bool isActive(const ConnectionSummary &summary);

struct ConnectionState {
  Registration registration = Registration::Unregistered;
  Participation participation = Participation::Active;
  // The id the program knows the connection by, such as an OpenADR VEN id.
  std::optional<std::string> clientId;
  std::optional<std::string> registrationId;
  std::optional<int> programPollIntervalSeconds;
};

struct RegistrationInfo {
  std::string clientId;
  std::string registrationId;
  std::optional<int> pollIntervalSeconds;
};

// What a program said when asked about a connection without registering.
struct TestResult {
  std::string description;
  // Whether the program has this connection registered.
  bool registered = false;
  // The id the program has the connection registered under, when it said.
  std::optional<std::string> clientId;
};

struct SignalInterval {
  int offsetSeconds = 0;
  int durationSeconds = 0;
  double value = 0;

  bool operator==(const SignalInterval &) const = default;
};

struct Signal {
  std::string name;
  std::string type;
  std::string id;
  std::vector<SignalInterval> intervals;

  bool operator==(const Signal &) const = default;
};

nlohmann::json signalsToJson(const std::vector<Signal> &signals);
// Throws nlohmann::json::exception for anything signalsToJson does not
// write.
std::vector<Signal> signalsFromJson(const nlohmann::json &json);

// An event as delivered by a program, in protocol-neutral form.
struct NormalizedEvent {
  std::string programEventId;
  int modificationNumber = 0;
  ProgramStatus status = ProgramStatus::Pending;
  Timestamp startAt{};
  // Zero means open-ended.
  int durationSeconds = 0;
  // The program's randomization window: the event starts somewhere between
  // startAt and startAt plus this. The planner picks the actual start.
  std::optional<int> startAfterSeconds;
  std::vector<Signal> signals;
  std::optional<std::string> marketContext;
  bool testEvent = false;
  bool responseRequired = true;
  std::optional<std::string> comment;
  std::optional<std::string> programRequestId;
  // False when the program applies the event whatever the site answers, as
  // CSIP-AUS does its controls. Such an event is always opted in.
  bool declinable = true;
};

// A program event as stored.
struct KnownEvent {
  EventRowId id = 0;
  std::string programEventId;
  int modificationNumber = 0;
  ProgramStatus status = ProgramStatus::Pending;
  Timestamp startAt{};
  int durationSeconds = 0;
  bool responseRequired = true;
  bool declinable = true;
  std::vector<Signal> signals;
  Opt opt = Opt::In;
  OptSource optSource = OptSource::Default;
  std::optional<std::string> programRequestId;
  // Highest modification number the program has successfully been given an
  // answer for.
  std::optional<int> respondedModification;
};

struct Command {
  CommandId id = 0;
  CommandKind kind = CommandKind::Test;
  std::optional<EventRowId> eventId;
  std::optional<std::string> requestedBy;
};

struct CommandOutcome {
  bool ok = false;
  std::string message;
  // Structured result for the API, so it need not parse the message. Null
  // when there is none.
  nlohmann::json result = nullptr;
};

struct ActivityEntry {
  ActivityCategory category = ActivityCategory::Connection;
  std::string message;
  std::optional<EventRowId> eventId;
  // Null when there is no detail. Connection entries carry the raw protocol
  // exchange here, as {"exchange": [...]}; see Adapter::takeExchange.
  nlohmann::json detail;
  // Who caused the entry. Empty means the system.
  std::optional<std::string> actor;
};

// "2026-07-14 21:00 UTC for 3h" or "2026-07-14 21:00 UTC, open-ended".
std::string describeWindow(Timestamp start, int durationSeconds);

// ISO 8601 in UTC with milliseconds: "2026-07-14T21:00:00.000Z".
std::string toIso8601(std::chrono::system_clock::time_point at);

} // namespace grid_services
