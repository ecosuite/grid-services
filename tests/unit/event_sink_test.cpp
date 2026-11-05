#include <gtest/gtest.h>

#include "core/event_sink.hpp"
#include "core/logging.hpp"
#include "support/memory_store.hpp"

using namespace grid_services;
using namespace grid_services::testing;

namespace {

NormalizedEvent programEvent(const std::string &id, int modification = 0) {
  NormalizedEvent event;
  event.programEventId = id;
  event.modificationNumber = modification;
  event.startAt = now() + std::chrono::hours(24);
  event.durationSeconds = 3600;
  return event;
}

class EventSinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    connection = db->addConnection({});
    store = std::make_unique<MemoryRunnerStore>(db, connection);
  }

  StoreEventSink sink(Opt defaultOpt = Opt::In) {
    return StoreEventSink(*store, defaultOpt, *log);
  }

  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  std::shared_ptr<spdlog::logger> log = makeLogger("test");
  ConnectionId connection = 0;
  std::unique_ptr<MemoryRunnerStore> store;
};

} // namespace

TEST_F(EventSinkTest, NewEventGetsTheDefaultOpt) {
  auto optIn = sink(Opt::In);
  const auto first = optIn.eventReceived(programEvent("a"));
  EXPECT_TRUE(first.changed);
  EXPECT_EQ(first.event.opt, Opt::In);
  EXPECT_EQ(first.event.optSource, OptSource::Default);

  auto optOut = sink(Opt::Out);
  EXPECT_EQ(optOut.eventReceived(programEvent("b")).event.opt, Opt::Out);
}

TEST_F(EventSinkTest, NewEventWhilePausedIsOptOut) {
  auto events = sink(Opt::In);
  events.setParticipation(Participation::Paused);
  const auto update = events.eventReceived(programEvent("a"));
  EXPECT_EQ(update.event.opt, Opt::Out);
  EXPECT_EQ(update.event.optSource, OptSource::Paused);
}

TEST_F(EventSinkTest, ModificationKeepsAnOperatorOptOut) {
  const auto id = db->addEvent(connection, programEvent("a"), Opt::Out,
                               OptSource::Operator);

  auto events = sink(Opt::In);
  const auto update = events.eventReceived(programEvent("a", 1));
  EXPECT_TRUE(update.changed);
  EXPECT_EQ(update.event.id, id);
  EXPECT_EQ(update.event.modificationNumber, 1);
  EXPECT_EQ(update.event.opt, Opt::Out);
  EXPECT_EQ(update.event.optSource, OptSource::Operator);
}

TEST_F(EventSinkTest, RepeatedOrStaleModificationIsNotAChange) {
  auto events = sink();
  events.eventReceived(programEvent("a", 2));
  EXPECT_FALSE(events.eventReceived(programEvent("a", 2)).changed);

  const auto stale = events.eventReceived(programEvent("a", 1));
  EXPECT_FALSE(stale.changed);
  EXPECT_EQ(stale.event.modificationNumber, 2);
}

TEST_F(EventSinkTest, RecordsActivityForReceivedAndEndedEvents) {
  auto events = sink();
  const auto update = events.eventReceived(programEvent("a"));
  events.eventEnded(update.event, ProgramStatus::Cancelled);

  EXPECT_EQ(db->event(update.event.id)->status, ProgramStatus::Cancelled);
  EXPECT_TRUE(events.openEvents().empty());
  const auto messages = db->activityMessages(connection);
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_NE(messages[0].find("Event received from the program"),
            std::string::npos);
  EXPECT_NE(messages[1].find("Event withdrawn by the program"),
            std::string::npos);
}

TEST_F(EventSinkTest, ResponsesAreRecorded) {
  auto events = sink();
  const auto update = events.eventReceived(programEvent("a", 3));
  events.responseSent(update.event, EventAnswer::OptIn, 3, true, "");
  EXPECT_EQ(db->event(update.event.id)->respondedModification, 3);
}

TEST_F(EventSinkTest, AnEventTheSiteCannotDeclineIsOptedInEvenWhilePaused) {
  auto events = sink(Opt::Out);
  events.setParticipation(Participation::Paused);
  auto event = programEvent("a");
  event.declinable = false;

  const auto update = events.eventReceived(event);

  EXPECT_EQ(update.event.opt, Opt::In);
  EXPECT_EQ(update.event.optSource, OptSource::Default);
  EXPECT_FALSE(update.event.declinable);
  EXPECT_EQ(db->activityMessages(connection).back().find(". Opt"),
            std::string::npos);
}

TEST_F(EventSinkTest, RecordsStatusChanges) {
  auto events = sink();
  const auto update = events.eventReceived(programEvent("a"));

  events.eventStatusChanged(update.event, ProgramStatus::Active);
  EXPECT_EQ(db->event(update.event.id)->status, ProgramStatus::Active);
  EXPECT_EQ(events.openEvents().size(), 1u);

  events.eventStatusChanged(update.event, ProgramStatus::Superseded);
  EXPECT_EQ(db->event(update.event.id)->status, ProgramStatus::Superseded);
  EXPECT_TRUE(events.openEvents().empty());

  const auto messages = db->activityMessages(connection);
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_NE(messages[1].find("Event started, as the program says"),
            std::string::npos);
  EXPECT_NE(messages[2].find("Event replaced by a newer one from the program"),
            std::string::npos);
}

TEST_F(EventSinkTest, DescribesEachAnswer) {
  auto events = sink();
  const auto update = events.eventReceived(programEvent("a"));

  events.responseSent(update.event, EventAnswer::Received, 0, true, "");
  events.responseSent(update.event, EventAnswer::Cancelled, 0, false,
                      "POST /rsp gave 500");

  const auto messages = db->activityMessages(connection);
  EXPECT_NE(messages[1].find("Told the program the event was received"),
            std::string::npos);
  EXPECT_EQ(messages[2], "Failed to tell the program the event was "
                         "cancelled: POST /rsp gave 500");
  EXPECT_EQ(db->responsesFor(update.event.id)[1].answer,
            EventAnswer::Cancelled);
}
