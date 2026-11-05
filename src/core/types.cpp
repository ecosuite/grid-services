#include "core/types.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace grid_services {

Timestamp now() {
  return std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
}

namespace {

template <typename Enum, std::size_t N>
std::string lookupName(const std::array<std::pair<Enum, const char *>, N> &table,
                       Enum value) {
  for (const auto &[candidate, name] : table) {
    if (candidate == value) {
      return name;
    }
  }
  throw std::logic_error("unnamed enum value");
}

template <typename Enum, std::size_t N>
Enum lookupValue(const std::array<std::pair<Enum, const char *>, N> &table,
                 const std::string &name, const char *what) {
  for (const auto &[value, candidate] : table) {
    if (name == candidate) {
      return value;
    }
  }
  throw std::invalid_argument(std::string("unknown ") + what + ": " + name);
}

constexpr std::array<std::pair<Opt, const char *>, 2> optNames{{
    {Opt::In, "in"},
    {Opt::Out, "out"},
}};

constexpr std::array<std::pair<OptSource, const char *>, 3> optSourceNames{{
    {OptSource::Default, "default"},
    {OptSource::Paused, "paused"},
    {OptSource::Operator, "operator"},
}};

constexpr std::array<std::pair<Participation, const char *>, 2>
    participationNames{{
        {Participation::Active, "active"},
        {Participation::Paused, "paused"},
    }};

constexpr std::array<std::pair<Registration, const char *>, 3>
    registrationNames{{
        {Registration::Unregistered, "unregistered"},
        {Registration::Registered, "registered"},
        {Registration::Error, "error"},
    }};

constexpr std::array<std::pair<ProgramStatus, const char *>, 5>
    programStatusNames{{
        {ProgramStatus::Pending, "pending"},
        {ProgramStatus::Active, "active"},
        {ProgramStatus::Completed, "completed"},
        {ProgramStatus::Cancelled, "cancelled"},
        {ProgramStatus::Superseded, "superseded"},
    }};

constexpr std::array<std::pair<EventAnswer, const char *>, 7> eventAnswerNames{{
    {EventAnswer::OptIn, "opt_in"},
    {EventAnswer::OptOut, "opt_out"},
    {EventAnswer::Received, "received"},
    {EventAnswer::Started, "started"},
    {EventAnswer::Completed, "completed"},
    {EventAnswer::Cancelled, "cancelled"},
    {EventAnswer::Superseded, "superseded"},
}};

constexpr std::array<std::pair<CommandKind, const char *>, 8> commandKindNames{{
    {CommandKind::Register, "register"},
    {CommandKind::Unregister, "unregister"},
    {CommandKind::Test, "test"},
    {CommandKind::Pause, "pause"},
    {CommandKind::Resume, "resume"},
    {CommandKind::ReloadConfig, "reload_config"},
    {CommandKind::OptOut, "opt_out"},
    {CommandKind::OptIn, "opt_in"},
}};

constexpr std::array<std::pair<ActivityCategory, const char *>, 3>
    activityCategoryNames{{
        {ActivityCategory::Connection, "connection"},
        {ActivityCategory::Events, "events"},
        {ActivityCategory::Reports, "reports"},
    }};

} // namespace

std::string toString(Opt value) { return lookupName(optNames, value); }
std::string toString(OptSource value) {
  return lookupName(optSourceNames, value);
}
std::string toString(Participation value) {
  return lookupName(participationNames, value);
}
std::string toString(Registration value) {
  return lookupName(registrationNames, value);
}
std::string toString(ProgramStatus value) {
  return lookupName(programStatusNames, value);
}
std::string toString(EventAnswer value) {
  return lookupName(eventAnswerNames, value);
}
std::string toString(CommandKind value) {
  return lookupName(commandKindNames, value);
}
std::string toString(ActivityCategory value) {
  return lookupName(activityCategoryNames, value);
}

Opt parseOpt(const std::string &value) {
  return lookupValue(optNames, value, "opt");
}
OptSource parseOptSource(const std::string &value) {
  return lookupValue(optSourceNames, value, "opt source");
}
Participation parseParticipation(const std::string &value) {
  return lookupValue(participationNames, value, "participation");
}
Registration parseRegistration(const std::string &value) {
  return lookupValue(registrationNames, value, "registration");
}
ProgramStatus parseProgramStatus(const std::string &value) {
  return lookupValue(programStatusNames, value, "program status");
}
EventAnswer parseEventAnswer(const std::string &value) {
  return lookupValue(eventAnswerNames, value, "event answer");
}
CommandKind parseCommandKind(const std::string &value) {
  return lookupValue(commandKindNames, value, "command kind");
}

bool isActive(const ConnectionConfig &config) {
  return config.enabled && !config.archived;
}

bool isActive(const ConnectionSummary &summary) {
  return summary.enabled && !summary.archived;
}

bool isTerminal(ProgramStatus status) {
  return status == ProgramStatus::Completed ||
         status == ProgramStatus::Cancelled ||
         status == ProgramStatus::Superseded;
}

EventAnswer optAnswer(Opt opt) {
  return opt == Opt::In ? EventAnswer::OptIn : EventAnswer::OptOut;
}

nlohmann::json signalsToJson(const std::vector<Signal> &signals) {
  auto result = nlohmann::json::array();
  for (const auto &signal : signals) {
    auto intervals = nlohmann::json::array();
    for (const auto &interval : signal.intervals) {
      intervals.push_back({{"offset_seconds", interval.offsetSeconds},
                           {"duration_seconds", interval.durationSeconds},
                           {"value", interval.value}});
    }
    result.push_back({{"name", signal.name},
                      {"type", signal.type},
                      {"id", signal.id},
                      {"intervals", intervals}});
  }
  return result;
}

std::vector<Signal> signalsFromJson(const nlohmann::json &json) {
  std::vector<Signal> signals;
  for (const auto &item : json) {
    Signal signal;
    signal.name = item.at("name").get<std::string>();
    signal.type = item.value("type", "");
    signal.id = item.value("id", "");
    for (const auto &interval : item.at("intervals")) {
      signal.intervals.push_back(
          {interval.at("offset_seconds").get<int>(),
           interval.at("duration_seconds").get<int>(),
           interval.at("value").get<double>()});
    }
    signals.push_back(std::move(signal));
  }
  return signals;
}

std::string describeWindow(Timestamp start, int durationSeconds) {
  const std::time_t seconds = start.time_since_epoch().count();
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M UTC", &utc);
  std::string result = buffer;

  if (durationSeconds <= 0) {
    return result + ", open-ended";
  }

  const int hours = durationSeconds / 3600;
  const int minutes = (durationSeconds % 3600) / 60;
  const int remainder = durationSeconds % 60;
  std::string duration;
  if (hours > 0) {
    duration += std::to_string(hours) + "h";
  }
  if (minutes > 0) {
    duration += std::to_string(minutes) + "m";
  }
  if (remainder > 0 || duration.empty()) {
    duration += std::to_string(remainder) + "s";
  }
  return result + " for " + duration;
}

std::string toIso8601(std::chrono::system_clock::time_point at) {
  const auto milliseconds =
      std::chrono::floor<std::chrono::milliseconds>(at).time_since_epoch();
  const std::time_t seconds =
      std::chrono::floor<std::chrono::seconds>(milliseconds).count();
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char date[32];
  std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &utc);
  char result[48];
  std::snprintf(result, sizeof result, "%s.%03dZ", date,
                static_cast<int>(milliseconds.count() % 1000));
  return result;
}

} // namespace grid_services
