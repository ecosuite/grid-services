#include <gtest/gtest.h>

#include <set>

#include "protocols/csipaus/limits.hpp"

using namespace grid_services;
using namespace grid_services::csipaus;

namespace {

const std::string lfdi = "C995EBDDFCFA53DE23DA8EA2BFFF91834D51D617";

Timestamp at(long long seconds) {
  return Timestamp(std::chrono::seconds(seconds));
}

ObservedEvent standing(const std::string &program, int primacy,
                       const std::string &limit, double value,
                       long long since = 1000) {
  ObservedEvent observed;
  observed.event.programEventId = "default:" + program;
  observed.event.status = ProgramStatus::Active;
  observed.event.startAt = at(since);
  observed.event.signals = {{limit, "limit", limit, {{0, 0, value}}}};
  observed.detail.kind = CsipausEventKind::Default;
  observed.detail.mrid = "D" + program;
  observed.detail.programMrid = program;
  observed.detail.primacy = primacy;
  return observed;
}

ObservedEvent control(const std::string &mrid, int primacy,
                      const std::string &limit, double value, long long start,
                      int duration, long long created = 500) {
  ObservedEvent observed;
  observed.event.programEventId = mrid;
  observed.event.status = ProgramStatus::Pending;
  observed.event.startAt = at(start);
  observed.event.durationSeconds = duration;
  observed.event.signals = {{limit, "limit", limit, {{0, duration, value}}}};
  observed.detail.kind = CsipausEventKind::Control;
  observed.detail.mrid = mrid;
  observed.detail.primacy = primacy;
  observed.detail.creationTime = at(created);
  return observed;
}

LimitSegment segment(const std::string &limit, long long start,
                     std::optional<long long> end, double value,
                     CsipausEventKind source, const std::string &eventId) {
  return {limit,
          at(start),
          end ? std::optional<Timestamp>(at(*end)) : std::nullopt,
          value,
          source,
          eventId};
}

ObservedEvent rampingIn(ObservedEvent observed, int rampTms) {
  observed.ramp.rampTms = rampTms;
  return observed;
}

ObservedEvent grading(ObservedEvent observed, int setGradW) {
  observed.ramp.setGradW = setGradW;
  return observed;
}

LimitSegment ramped(LimitSegment segment, std::optional<int> rampTms,
                    std::optional<int> setGradW) {
  segment.rampTms = rampTms;
  segment.setGradW = setGradW;
  return segment;
}

} // namespace

TEST(CsipausLimits, ADefaultHoldsFromWhenItWasSeen) {
  const auto limits =
      limitsInForce({standing("P1", 1, "export", 5000)}, lfdi, at(2000));
  EXPECT_EQ(limits, (std::vector<LimitSegment>{segment(
                        "export", 1000, std::nullopt, 5000,
                        CsipausEventKind::Default, "default:P1")}));
}

TEST(CsipausLimits, AControlHoldsOverTheDefaultForItsWindow) {
  const auto limits = limitsInForce(
      {standing("P1", 1, "export", 5000), control("C1", 1, "export", 1500, 3000, 600)},
      lfdi, at(2000));
  EXPECT_EQ(limits,
            (std::vector<LimitSegment>{
                segment("export", 1000, 3000, 5000, CsipausEventKind::Default,
                        "default:P1"),
                segment("export", 3000, 3600, 1500, CsipausEventKind::Control,
                        "C1"),
                segment("export", 3600, std::nullopt, 5000,
                        CsipausEventKind::Default, "default:P1")}));
}

TEST(CsipausLimits, ControlsCompeteOnlyForTheCapsTheySet) {
  const auto limits = limitsInForce(
      {standing("P1", 1, "export", 5000), control("C1", 1, "import", 3000, 3000, 600)},
      lfdi, at(2000));
  ASSERT_EQ(limits.size(), 2u);
  EXPECT_EQ(limits[0], segment("export", 1000, std::nullopt, 5000,
                               CsipausEventKind::Default, "default:P1"));
  EXPECT_EQ(limits[1],
            segment("import", 3000, 3600, 3000, CsipausEventKind::Control, "C1"));
}

TEST(CsipausLimits, TheLowerPrimacyWinsThenTheNewer) {
  const auto limits = limitsInForce(
      {control("LOW", 2, "export", 1000, 3000, 600),
       control("HIGH", 1, "export", 2000, 3300, 600),
       control("OLDER", 2, "export", 500, 3000, 300, 100)},
      lfdi, at(2000));
  EXPECT_EQ(limits,
            (std::vector<LimitSegment>{
                segment("export", 3000, 3300, 1000, CsipausEventKind::Control,
                        "LOW"),
                segment("export", 3300, 3900, 2000, CsipausEventKind::Control,
                        "HIGH")}));
}

TEST(CsipausLimits, TheDefaultOfTheLowerPrimacyHolds) {
  const auto limits = limitsInForce(
      {standing("P2", 2, "export", 9000), standing("P1", 1, "export", 4000)},
      lfdi, at(2000));
  ASSERT_EQ(limits.size(), 1u);
  EXPECT_EQ(limits[0].value, 4000);
}

TEST(CsipausLimits, EndedCancelledAndSupersededControlsTakeNoPart) {
  auto cancelled = control("X", 1, "export", 0, 3000, 600);
  cancelled.event.status = ProgramStatus::Cancelled;
  auto superseded = control("Y", 1, "export", 0, 3000, 600);
  superseded.event.status = ProgramStatus::Superseded;
  const auto limits = limitsInForce(
      {cancelled, superseded, control("Z", 1, "export", 0, 1000, 600)}, lfdi,
      at(2000));
  EXPECT_TRUE(limits.empty());
}

TEST(CsipausLimits, EachDeviceMovesAControlByItsOwnRandomOffset) {
  const auto offset = randomOffset(lfdi, "C1", 300);
  EXPECT_GE(offset, 0);
  EXPECT_LE(offset, 300);
  EXPECT_EQ(randomOffset(lfdi, "C1", 300), offset);
  EXPECT_LE(randomOffset(lfdi, "C1", -300), 0);
  EXPECT_EQ(randomOffset(lfdi, "C1", 0), 0);

  auto randomized = control("C1", 1, "export", 1500, 3000, 600);
  randomized.detail.randomizeStartSeconds = 300;
  const auto limits = limitsInForce({randomized}, lfdi, at(2000));
  ASSERT_EQ(limits.size(), 1u);
  EXPECT_EQ(limits[0].start, at(3000 + offset));
  EXPECT_EQ(limits[0].end, at(3600 + offset));
}

TEST(CsipausLimits, TheEffectiveWindowIsTheRandomizedOne) {
  auto randomized = control("C1", 1, "export", 1500, 3000, 600);
  randomized.detail.randomizeStartSeconds = 300;
  randomized.detail.randomizeDurationSeconds = -120;
  const auto start = 3000 + randomOffset(lfdi, "C1", 300);
  const auto end = start + 600 + randomOffset(lfdi, "C1", -120);

  const auto window =
      effectiveWindow(randomized.event, randomized.detail, lfdi);
  EXPECT_EQ(window.start, at(start));
  EXPECT_EQ(window.end, at(end));

  const auto limits = limitsInForce({randomized}, lfdi, at(2000));
  ASSERT_EQ(limits.size(), 1u);
  EXPECT_EQ(limits[0].start, window.start);
  EXPECT_EQ(limits[0].end, window.end);
}

TEST(CsipausLimits, AControlOutrankedForItsWholeWindowIsSuperseded) {
  EXPECT_EQ(supersededControls({control("LOW", 2, "export", 1000, 3000, 600),
                                control("HIGH", 1, "export", 2000, 2900, 800)},
                               lfdi, at(2000)),
            (std::set<std::string>{"LOW"}));
}

TEST(CsipausLimits, AnOlderControlOfOneProgramCoveredByANewerOneIsSuperseded) {
  EXPECT_EQ(supersededControls(
                {control("OLDER", 1, "export", 1000, 3000, 600, 100),
                 control("NEWER", 1, "export", 0, 3000, 600, 500)},
                lfdi, at(2000)),
            (std::set<std::string>{"OLDER"}));
}

// GEN-10's DERC1: it has started, and a control that outranks it holds the
// rest of its window.
TEST(CsipausLimits, AStartedControlWhoseRestIsCoveredIsSuperseded) {
  const std::vector<ObservedEvent> observed = {
      control("DERC1", 2, "export", 1000, 1000, 2000),
      control("DERC2", 1, "export", 0, 2000, 1000)};
  EXPECT_EQ(supersededControls(observed, lfdi, at(2000)),
            (std::set<std::string>{"DERC1"}));
  EXPECT_TRUE(supersededControls(observed, lfdi, at(1500)).empty())
      << "it holds export until DERC2 starts";
}

// GEN-10's DERC8: export is held by another control, generation is not.
TEST(CsipausLimits, AControlThatStillWinsOneCapIsNotSuperseded) {
  auto both = control("BOTH", 2, "export", 1000, 3000, 600);
  both.event.signals.push_back(
      {"generation", "limit", "generation", {{0, 600, 4000}}});
  EXPECT_TRUE(supersededControls(
                  {both, control("HIGH", 1, "export", 0, 3000, 600)}, lfdi,
                  at(2000))
                  .empty());
}

TEST(CsipausLimits, APartlyCoveredControlIsNotSuperseded) {
  EXPECT_TRUE(supersededControls({control("LOW", 2, "export", 1000, 3000, 600),
                                  control("HIGH", 1, "export", 0, 3300, 600)},
                                 lfdi, at(2000))
                  .empty());
}

TEST(CsipausLimits, AControlSettingNoCapIsNeverSuperseded) {
  auto none = control("NONE", 2, "export", 0, 3000, 600);
  none.event.signals.clear();
  EXPECT_TRUE(supersededControls(
                  {none, control("HIGH", 1, "export", 0, 2000, 3000)}, lfdi,
                  at(2000))
                  .empty());
}

TEST(CsipausLimits, EndedEmptyAndServerMarkedControlsAreNotJudged) {
  auto cancelled = control("X", 2, "export", 0, 3000, 600);
  cancelled.event.status = ProgramStatus::Cancelled;
  auto superseded = control("Y", 2, "export", 0, 3000, 600);
  superseded.event.status = ProgramStatus::Superseded;
  const auto shrink = -randomOffset(lfdi, "E", -300);
  ASSERT_GT(shrink, 0);
  auto empty = control("E", 2, "export", 0, 3000, shrink);
  empty.detail.randomizeDurationSeconds = -300;
  EXPECT_TRUE(supersededControls({cancelled, superseded, empty,
                                  control("Z", 2, "export", 0, 1000, 600),
                                  control("HIGH", 1, "export", 0, 500, 5000)},
                                 lfdi, at(2000))
                  .empty());
}

TEST(CsipausLimits, SupersessionUsesTheEffectiveWindow) {
  auto low = control("LOW", 2, "export", 1000, 3000, 600);
  low.detail.randomizeStartSeconds = 300;
  const auto offset = randomOffset(lfdi, "LOW", 300);
  ASSERT_GT(offset, 0);

  EXPECT_TRUE(
      supersededControls({low, control("HIGH", 1, "export", 0, 3000, 600)},
                         lfdi, at(2000))
          .empty())
      << "it holds export past HIGH's end";
  EXPECT_EQ(supersededControls(
                {low, control("HIGH", 1, "export", 0, 3000 + offset, 600)},
                lfdi, at(2000)),
            (std::set<std::string>{"LOW"}));
}

TEST(CsipausLimits, AControlsRampTmsGoesOnEachOfItsRows) {
  const auto limits = limitsInForce(
      {grading(standing("P1", 1, "export", 5000), 100),
       rampingIn(control("C1", 1, "export", 1500, 3000, 600), 6000),
       control("OUT", 0, "export", 0, 3200, 100)},
      lfdi, at(2000));
  EXPECT_EQ(limits,
            (std::vector<LimitSegment>{
                ramped(segment("export", 1000, 3000, 5000,
                               CsipausEventKind::Default, "default:P1"),
                       std::nullopt, 100),
                ramped(segment("export", 3000, 3200, 1500,
                               CsipausEventKind::Control, "C1"),
                       6000, std::nullopt),
                ramped(segment("export", 3200, 3300, 0,
                               CsipausEventKind::Control, "OUT"),
                       std::nullopt, 100),
                ramped(segment("export", 3300, 3600, 1500,
                               CsipausEventKind::Control, "C1"),
                       6000, std::nullopt),
                ramped(segment("export", 3600, std::nullopt, 5000,
                               CsipausEventKind::Default, "default:P1"),
                       std::nullopt, 100)}))
      << "C1 resumes with its own ramp once OUT ends";
}

TEST(CsipausLimits, WithNoSetGradWAnywhereRowsCarryNoRamp) {
  const auto limits = limitsInForce(
      {standing("P1", 1, "export", 5000), control("C1", 1, "export", 1500, 3000, 600)},
      lfdi, at(2000));
  ASSERT_EQ(limits.size(), 3u);
  for (const auto &row : limits) {
    EXPECT_FALSE(row.rampTms.has_value()) << row.programEventId;
    EXPECT_FALSE(row.setGradW.has_value()) << row.programEventId;
  }
  EXPECT_FALSE(setGradWInForce({standing("P1", 1, "export", 5000)}).has_value());
}

TEST(CsipausLimits, TheSetGradWInForceIsTheLowestPrimacyDefaultsThatSetsOne) {
  const std::vector<ObservedEvent> observed = {
      standing("P1", 1, "export", 5000),
      grading(standing("P3", 3, "import", 3000), 200),
      grading(standing("P2", 2, "generation", 4000), 50),
      control("C1", 1, "export", 1500, 3000, 600)};
  EXPECT_EQ(setGradWInForce(observed), 50);

  for (const auto &row : limitsInForce(observed, lfdi, at(2000))) {
    EXPECT_EQ(row.setGradW, 50) << row.limit << " " << row.programEventId
                                << ": not its own program's default's";
    EXPECT_FALSE(row.rampTms.has_value());
  }
}

TEST(CsipausLimits, AChangedSetGradWAloneChangesTheLimits) {
  const auto with = [](int setGradW) {
    return limitsInForce({grading(standing("P1", 1, "export", 5000), setGradW),
                          control("C1", 1, "export", 1500, 3000, 600)},
                         lfdi, at(2000));
  };
  EXPECT_NE(with(100), with(27));
  EXPECT_EQ(with(100), with(100));
}

TEST(CsipausLimits, ARampedControlsRowsStillJoin) {
  const auto limits = limitsInForce(
      {rampingIn(control("LOW", 2, "export", 1000, 3000, 600), 6000),
       rampingIn(control("LOWER", 3, "export", 500, 3200, 100), 1000)},
      lfdi, at(2000));
  EXPECT_EQ(limits, (std::vector<LimitSegment>{ramped(
                        segment("export", 3000, 3600, 1000,
                                CsipausEventKind::Control, "LOW"),
                        6000, std::nullopt)}));
}

TEST(CsipausLimits, SwitchedOffWhileAConnectOrEnergizeCapOfZeroCoversNow) {
  const std::vector<LimitSegment> limits = {
      segment("export", 1000, std::nullopt, 0, CsipausEventKind::Default,
              "default:P1"),
      segment("connect", 3000, 3600, 0, CsipausEventKind::Control, "C1"),
      segment("energize", 4000, std::nullopt, 0, CsipausEventKind::Default,
              "default:P1")};
  EXPECT_FALSE(switchedOff(limits, at(2999)));
  EXPECT_TRUE(switchedOff(limits, at(3000))) << "connect 0 from its start";
  EXPECT_TRUE(switchedOff(limits, at(3599)));
  EXPECT_FALSE(switchedOff(limits, at(3600))) << "not from its end";
  EXPECT_TRUE(switchedOff(limits, at(5000))) << "a default's energize 0";
}

TEST(CsipausLimits, NotSwitchedOffByACapOfOneOrOneThatEnded) {
  EXPECT_FALSE(switchedOff(
      {segment("connect", 3000, 3600, 1, CsipausEventKind::Control, "C1"),
       segment("energize", 3000, std::nullopt, 1, CsipausEventKind::Default,
               "default:P1"),
       segment("export", 3000, std::nullopt, 0, CsipausEventKind::Default,
               "default:P1")},
      at(3100)))
      << "switches of 1, and an export cap of 0, switch nothing off";
  EXPECT_FALSE(switchedOff(
      {segment("connect", 3000, 3600, 0, CsipausEventKind::Control, "C1")},
      at(3700)));
  EXPECT_FALSE(switchedOff({}, at(3100)));
}

TEST(CsipausLimits, DefaultsOfKeepsOnlyDefaults) {
  const auto defaults =
      defaultsOf({standing("P1", 1, "export", 5000),
                  control("C1", 1, "export", 1500, 3000, 600),
                  standing("P2", 2, "connect", 1)});
  ASSERT_EQ(defaults.size(), 2u);
  EXPECT_EQ(defaults[0].event.programEventId, "default:P1");
  EXPECT_EQ(defaults[1].event.programEventId, "default:P2");
}
