#include <gtest/gtest.h>

#include "core/types.hpp"

using namespace grid_services;

TEST(Types, EnumNamesRoundTrip) {
  for (const auto kind :
       {CommandKind::Register, CommandKind::Unregister, CommandKind::Test,
        CommandKind::Pause, CommandKind::Resume, CommandKind::ReloadConfig,
        CommandKind::OptOut, CommandKind::OptIn}) {
    EXPECT_EQ(parseCommandKind(toString(kind)), kind);
  }
  for (const auto status :
       {ProgramStatus::Pending, ProgramStatus::Active, ProgramStatus::Completed,
        ProgramStatus::Cancelled, ProgramStatus::Superseded}) {
    EXPECT_EQ(parseProgramStatus(toString(status)), status);
  }
  for (const auto answer :
       {EventAnswer::OptIn, EventAnswer::OptOut, EventAnswer::Received,
        EventAnswer::Started, EventAnswer::Completed, EventAnswer::Cancelled,
        EventAnswer::Superseded}) {
    EXPECT_EQ(parseEventAnswer(toString(answer)), answer);
  }
  EXPECT_EQ(toString(EventAnswer::OptOut), "opt_out");
  EXPECT_EQ(optAnswer(Opt::In), EventAnswer::OptIn);
  EXPECT_EQ(toString(CommandKind::ReloadConfig), "reload_config");
  EXPECT_EQ(toString(CommandKind::OptIn), "opt_in");
  EXPECT_THROW(parseOpt("maybe"), std::invalid_argument);
}

TEST(Types, TerminalStatuses) {
  EXPECT_FALSE(isTerminal(ProgramStatus::Pending));
  EXPECT_FALSE(isTerminal(ProgramStatus::Active));
  EXPECT_TRUE(isTerminal(ProgramStatus::Completed));
  EXPECT_TRUE(isTerminal(ProgramStatus::Cancelled));
  EXPECT_TRUE(isTerminal(ProgramStatus::Superseded));
}

TEST(Types, DescribeWindow) {
  const Timestamp start{std::chrono::seconds(1'784'062'800)}; // 2026-07-14 21:00Z
  EXPECT_EQ(describeWindow(start, 3 * 3600), "2026-07-14 21:00 UTC for 3h");
  EXPECT_EQ(describeWindow(start, 5400), "2026-07-14 21:00 UTC for 1h30m");
  EXPECT_EQ(describeWindow(start, 0), "2026-07-14 21:00 UTC, open-ended");
}

TEST(Types, SignalsToJson) {
  const auto json = signalsToJson(
      {{"simple", "level", "s1", {{0, 3600, 1.0}, {3600, 1800, 2.0}}}});
  ASSERT_EQ(json.size(), 1u);
  EXPECT_EQ(json[0]["name"], "simple");
  EXPECT_EQ(json[0]["intervals"][1]["offset_seconds"], 3600);
  EXPECT_EQ(json[0]["intervals"][1]["value"], 2.0);
}

TEST(Types, SignalsFromJsonReadsWhatSignalsToJsonWrites) {
  const std::vector<Signal> signals{
      {"export", "limit", "export", {{0, 1800, 1500.5}}},
      {"simple", "level", "s1", {{0, 3600, 1.0}, {3600, 1800, 2.0}}}};
  EXPECT_EQ(signalsFromJson(signalsToJson(signals)), signals);
  EXPECT_THROW(signalsFromJson(nlohmann::json::array({{{"name", "x"}}})),
               nlohmann::json::exception);
}

TEST(Types, Iso8601) {
  const std::chrono::system_clock::time_point at{
      std::chrono::milliseconds(1'784'062'800'042)};
  EXPECT_EQ(toIso8601(at), "2026-07-14T21:00:00.042Z");
}

TEST(Types, ActiveConnections) {
  ConnectionConfig config;
  EXPECT_TRUE(isActive(config));
  config.archived = true;
  EXPECT_FALSE(isActive(config));
  config.archived = false;
  config.enabled = false;
  EXPECT_FALSE(isActive(config));
}
