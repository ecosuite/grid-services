#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>

#include "core/runner.hpp"
#include "support/fake_adapter.hpp"
#include "support/memory_store.hpp"

using namespace std::chrono_literals;
using namespace grid_services;
using namespace grid_services::testing;

namespace {

bool contains(const std::vector<std::string> &messages,
              const std::string &text) {
  return std::any_of(messages.begin(), messages.end(), [&](const auto &m) {
    return m.find(text) != std::string::npos;
  });
}

NormalizedEvent futureEvent(const std::string &id) {
  NormalizedEvent event;
  event.programEventId = id;
  event.startAt = now() + std::chrono::hours(24);
  event.durationSeconds = 3600;
  return event;
}

class RunnerTest : public ::testing::Test {
protected:
  void SetUp() override {
    adapters.add("fake", fakeAdapters(script));
    options.workerId = "test-worker";
    options.defaultPollInterval = 20ms;
    options.heartbeatInterval = 20ms;
    options.minBackoff = 10ms;
    options.maxBackoff = 40ms;
    connection = db->addConnection({});
  }

  void TearDown() override {
    if (runner) {
      runner->requestStop();
      runner->join();
    }
  }

  void start() {
    runner = std::make_unique<ConnectionRunner>(
        db->connections.at(connection),
        std::make_unique<MemoryRunnerStore>(db, connection), adapters, options);
    runner->start();
  }

  void registered(std::optional<int> programPollIntervalSeconds = std::nullopt) {
    std::lock_guard lock(db->mutex);
    auto &state = db->states[connection];
    state.registration = Registration::Registered;
    state.clientId = "ven-1";
    state.registrationId = "reg-1";
    state.programPollIntervalSeconds = programPollIntervalSeconds;
  }

  bool commandDone(CommandId id) {
    return db->waitFor([id](const MemoryDatabase &d) {
      const auto status = d.commands.at(static_cast<size_t>(id) - 1).status;
      return status == "succeeded" || status == "failed";
    });
  }

  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  std::shared_ptr<FakeAdapterScript> script =
      std::make_shared<FakeAdapterScript>();
  AdapterRegistry adapters;
  RunnerOptions options;
  ConnectionId connection = 0;
  std::unique_ptr<ConnectionRunner> runner;
};

} // namespace

TEST_F(RunnerTest, UnregisteredConnectionHeartbeatsButDoesNotPoll) {
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.extras.count(connection) && d.extras.at(connection).heartbeats >= 3;
  }));
  EXPECT_EQ(script->polled(), 0);
  EXPECT_EQ(db->stateExtras(connection).workerId, "test-worker");
  EXPECT_EQ(db->stateExtras(connection).appliedConfigVersion, 1);
}

TEST_F(RunnerTest, RegisterCommandRegistersAndStartsPolling) {
  start();
  const auto command =
      db->addCommand(connection, CommandKind::Register, std::nullopt, "ops");
  ASSERT_TRUE(commandDone(command));

  EXPECT_EQ(db->command(command).status, "succeeded");
  const auto state = db->state(connection);
  EXPECT_EQ(state.registration, Registration::Registered);
  EXPECT_EQ(state.clientId, "ven-1");
  EXPECT_EQ(state.registrationId, "reg-1");
  EXPECT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    std::lock_guard lock(script->mutex);
    return script->pollCalls >= 2;
  }));
  const auto entry = db->activityEntry(
      connection, "Registered with the program as client ven-1");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->actor, "ops");
  EXPECT_EQ(db->command(command).result,
            (nlohmann::json{{"client_id", "ven-1"},
                            {"registration_id", "reg-1"},
                            {"poll_interval_seconds", nullptr}}));
  EXPECT_EQ(db->command(command).message, "Registered as client ven-1.");
}

TEST_F(RunnerTest, UnregisterCommandRecordsTheOperator) {
  registered();
  start();
  const auto command =
      db->addCommand(connection, CommandKind::Unregister, std::nullopt, "ops");
  ASSERT_TRUE(commandDone(command));

  EXPECT_EQ(db->command(command).status, "succeeded");
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered);
  const auto entry =
      db->activityEntry(connection, "Unregistered from the program");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->actor, "ops");

  // The adapter learns the saved registration before cancelling it.
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->resumeCalls, 1);
  EXPECT_EQ(script->unregisterCalls, 1);
}

TEST_F(RunnerTest, UnregisterSkipsTheProgramWhenItHasNoRegistration) {
  registered();
  script->resumeResult.reset();
  // A long poll interval, so the command arrives before the first poll.
  options.defaultPollInterval = 60s;
  const auto command = db->addCommand(connection, CommandKind::Unregister);
  start();
  ASSERT_TRUE(commandDone(command));

  EXPECT_EQ(db->command(command).status, "succeeded");
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered);
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->unregisterCalls, 0);
}

TEST_F(RunnerTest, RejectedRegistrationMarksTheConnectionInError) {
  script->registerError = AdapterError::permanent("the VTN said no");
  start();
  const auto command =
      db->addCommand(connection, CommandKind::Register, std::nullopt, "ops");
  ASSERT_TRUE(commandDone(command));

  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_EQ(db->command(command).message, "the VTN said no");
  EXPECT_TRUE(db->command(command).result.is_null());
  EXPECT_EQ(db->state(connection).registration, Registration::Error);
  EXPECT_EQ(script->polled(), 0);
  const auto entry = db->activityEntry(connection, "Registration failed");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->actor, "ops");
}

TEST_F(RunnerTest, UnreachableProgramFailsRegistrationWithoutAnError) {
  script->registerError = AdapterError::transient("connection refused");
  start();
  const auto command = db->addCommand(connection, CommandKind::Register);
  ASSERT_TRUE(commandDone(command));

  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered);
  EXPECT_TRUE(contains(db->activityMessages(connection),
                       "Registration failed: connection refused"));
}

TEST_F(RunnerTest, ReregistrationTheProgramAskedForIsTheSystems) {
  registered();
  script->queuePoll([](ProgramEventSink &) {
    return PollResult{PollOutcome::ReregistrationRequested, std::nullopt};
  });
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    std::lock_guard lock(script->mutex);
    return script->registerCalls == 1;
  }));
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return std::any_of(d.activity.begin(), d.activity.end(), [](const auto &e) {
      return e.entry.message.find("Registered with the program") !=
             std::string::npos;
    });
  }));
  EXPECT_FALSE(
      db->activityEntry(connection, "Registered with the program")->actor);
}

TEST_F(RunnerTest, ResumesASavedRegistration) {
  registered();
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    return script->polled() >= 1;
  }));
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->resumeCalls, 1);
}

TEST_F(RunnerTest, ClearsARegistrationTheProgramNoLongerHas) {
  registered();
  script->resumeResult.reset();
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.states.at(connection).registration == Registration::Unregistered;
  }));
  EXPECT_FALSE(db->state(connection).registrationId);
  EXPECT_EQ(script->polled(), 0);
  EXPECT_EQ(db->stateExtras(connection).lastError,
            "The program no longer has this connection's registration. "
            "Register again to resume.");
}

TEST_F(RunnerTest, FailsCommandsInterruptedByAPreviousWorker) {
  const auto command = db->addCommand(connection, CommandKind::Test);
  db->commands.at(0).status = "running";
  start();
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_NE(db->command(command).message.find("stopped before"),
            std::string::npos);
}

TEST_F(RunnerTest, PauseAndResumeChangeParticipation) {
  start();
  const auto pause =
      db->addCommand(connection, CommandKind::Pause, std::nullopt, "ops");
  ASSERT_TRUE(commandDone(pause));
  EXPECT_EQ(db->state(connection).participation, Participation::Paused);

  const auto again = db->addCommand(connection, CommandKind::Pause);
  ASSERT_TRUE(commandDone(again));
  EXPECT_EQ(db->command(again).message, "Participation is already paused.");

  const auto resume = db->addCommand(connection, CommandKind::Resume);
  ASSERT_TRUE(commandDone(resume));
  EXPECT_EQ(db->state(connection).participation, Participation::Active);

  std::lock_guard lock(script->mutex);
  ASSERT_GE(script->participation.size(), 3u);
  EXPECT_EQ(script->participation.back(), Participation::Active);

  std::lock_guard dbLock(db->mutex);
  const auto paused = std::find_if(
      db->activity.begin(), db->activity.end(), [](const auto &entry) {
        return entry.entry.message.find("Participation paused") !=
               std::string::npos;
      });
  ASSERT_NE(paused, db->activity.end());
  EXPECT_EQ(paused->entry.actor, "ops");
}

TEST_F(RunnerTest, NewEventsWhilePausedAreOptedOut) {
  registered();
  {
    std::lock_guard lock(db->mutex);
    db->states[connection].participation = Participation::Paused;
  }
  script->queuePoll([](ProgramEventSink &sink) {
    sink.eventReceived(futureEvent("e1"));
    return PollResult{};
  });
  start();
  ASSERT_TRUE(db->waitFor([](const MemoryDatabase &d) {
    return !d.events.empty();
  }));
  EXPECT_EQ(db->event(1)->opt, Opt::Out);
  EXPECT_EQ(db->event(1)->optSource, OptSource::Paused);
}

TEST_F(RunnerTest, ReloadConfigRestartsTheRunner) {
  start();
  const auto command = db->addCommand(connection, CommandKind::ReloadConfig);
  ASSERT_TRUE(commandDone(command));
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    return runner->finished();
  }));
  EXPECT_EQ(runner->exitReason(), RunnerExit::RestartRequested);
}

TEST_F(RunnerTest, TransientPollFailuresBackOffAndRecover) {
  registered();
  script->queuePollError(AdapterError::transient("connection refused"));
  script->queuePollError(AdapterError::transient("connection refused"));
  start();

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    return script->polled() >= 4;
  }));
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return !d.extras.at(connection).lastError;
  }));
  EXPECT_EQ(db->stateExtras(connection).consecutiveFailures, 0);
  EXPECT_FALSE(db->stateExtras(connection).nextRetryAt);
  const auto messages = db->activityMessages(connection);
  EXPECT_EQ(std::count_if(messages.begin(), messages.end(),
                          [](const auto &m) {
                            return m.find("Lost contact") != std::string::npos;
                          }),
            1);
  EXPECT_TRUE(contains(messages, "Contact with the program restored"));
  EXPECT_EQ(db->state(connection).registration, Registration::Registered);
}

TEST_F(RunnerTest, FailedAttemptsAreCountedWithTheNextRetry) {
  registered();
  options.minBackoff = 60s;
  options.maxBackoff = 60s;
  script->queuePollError(AdapterError::transient("connection refused"));
  const auto before = now();
  start();

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.extras.count(connection) &&
           d.extras.at(connection).consecutiveFailures == 1;
  }));
  const auto extras = db->stateExtras(connection);
  ASSERT_TRUE(extras.nextRetryAt);
  // Backoff is jittered by a fifth either way.
  EXPECT_GE(*extras.nextRetryAt, before + 48s);
  EXPECT_LE(*extras.nextRetryAt, now() + 73s);
  EXPECT_EQ(db->state(connection).registration, Registration::Registered);
}

TEST_F(RunnerTest, RetriesAtTheDelayTheAdapterGives) {
  registered();
  script->resumeResult = RegistrationInfo{"ven-1", "reg-1", 7};
  options.minBackoff = 60s;
  options.maxBackoff = 60s;
  script->retryDelay = 30ms;
  for (int i = 0; i < 3; ++i) {
    script->queuePollError(AdapterError::transient("connection refused"));
  }
  start();

  ASSERT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 4; }, 2s))
      << "long before the runner's backoff ends";
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->retriesAsked,
            (std::vector<std::pair<int, std::chrono::milliseconds>>{
                {1, 7s}, {2, 7s}, {3, 7s}}))
      << "the program's poll interval";
}

TEST_F(RunnerTest, CountsAFailedAttemptWithTheAdaptersDelay) {
  registered();
  script->retryDelay = 90s;
  script->queuePollError(AdapterError::transient("connection refused"));
  const auto before = now();
  start();

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.extras.count(connection) &&
           d.extras.at(connection).consecutiveFailures == 1;
  }));
  const auto extras = db->stateExtras(connection);
  ASSERT_TRUE(extras.nextRetryAt);
  EXPECT_GE(*extras.nextRetryAt, before + 90s);
  EXPECT_LE(*extras.nextRetryAt, now() + 91s);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(script->polled(), 1) << "not at the runner's backoff";
}

TEST_F(RunnerTest, KeepsItsBackoffWhenTheAdapterGivesNoDelay) {
  registered();
  options.defaultPollInterval = 5s;
  options.minBackoff = 60s;
  options.maxBackoff = 60s;
  script->queuePollError(AdapterError::transient("connection refused"));
  const auto before = now();
  start();

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.extras.count(connection) &&
           d.extras.at(connection).consecutiveFailures == 1;
  }));
  const auto extras = db->stateExtras(connection);
  ASSERT_TRUE(extras.nextRetryAt);
  EXPECT_GE(*extras.nextRetryAt, before + 48s);
  EXPECT_LE(*extras.nextRetryAt, now() + 73s);
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->retriesAsked,
            (std::vector<std::pair<int, std::chrono::milliseconds>>{{1, 5s}}));
}

TEST_F(RunnerTest, AFailedResumeIsRetriedAtTheAdaptersDelay) {
  registered(9);
  options.minBackoff = 60s;
  options.maxBackoff = 60s;
  script->retryDelay = 30ms;
  script->resumeErrors = {AdapterError::transient("500 COMMS DISABLED"),
                          AdapterError::transient("500 COMMS DISABLED")};
  start();

  ASSERT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 1; }, 2s))
      << "resumed long before the runner's backoff ends";
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->resumeCalls, 3);
  EXPECT_EQ(script->retriesAsked,
            (std::vector<std::pair<int, std::chrono::milliseconds>>{{1, 9s},
                                                                     {2, 9s}}))
      << "the stored poll interval";
}

TEST_F(RunnerTest, PermanentPollFailureStopsPolling) {
  registered();
  script->queuePollError(AdapterError::permanent("certificate rejected"));
  start();

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.states.at(connection).registration == Registration::Error;
  }));
  const int polls = script->polled();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(script->polled(), polls);
  EXPECT_NE(db->stateExtras(connection).lastError->find("certificate rejected"),
            std::string::npos);
  EXPECT_EQ(db->stateExtras(connection).consecutiveFailures, 0);
  EXPECT_FALSE(db->stateExtras(connection).nextRetryAt);
}

TEST_F(RunnerTest, ProgramCancellingTheRegistrationUnregisters) {
  registered();
  script->queuePoll([](ProgramEventSink &) {
    return PollResult{PollOutcome::RegistrationCancelled,
                      "The program cancelled the registration"};
  });
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.states.at(connection).registration == Registration::Unregistered;
  }));
  EXPECT_TRUE(contains(db->activityMessages(connection),
                       "cancelled this connection's registration"));
}

TEST_F(RunnerTest, ReregistrationRequestRegistersAgain) {
  registered();
  script->queuePoll([](ProgramEventSink &) {
    return PollResult{PollOutcome::ReregistrationRequested, std::nullopt};
  });
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    std::lock_guard lock(script->mutex);
    return script->registerCalls == 1;
  }));
}

TEST_F(RunnerTest, OptOutTellsTheProgramAndRecordsTheOperator) {
  registered();
  const auto event = db->addEvent(connection, futureEvent("e1"));
  start();

  const auto command =
      db->addCommand(connection, CommandKind::OptOut, event, "ops");
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "succeeded");
  EXPECT_EQ(db->event(event)->opt, Opt::Out);
  EXPECT_EQ(db->event(event)->optSource, OptSource::Operator);
  ASSERT_EQ(db->responsesFor(event).size(), 1u);
  EXPECT_EQ(db->responsesFor(event)[0].answer, EventAnswer::OptOut);
  EXPECT_EQ(db->command(command).result,
            (nlohmann::json{{"opt", "out"}, {"program_told", true}}));

  std::lock_guard lock(script->mutex);
  ASSERT_EQ(script->responses.size(), 1u);
  EXPECT_EQ(script->responses[0].second, Opt::Out);
}

TEST_F(RunnerTest, OptInUndoesAnOptOut) {
  registered();
  const auto event = db->addEvent(connection, futureEvent("e1"), Opt::Out,
                                  OptSource::Operator);
  start();

  const auto command =
      db->addCommand(connection, CommandKind::OptIn, event, "ops");
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "succeeded");
  EXPECT_EQ(db->command(command).message, "Opted in.");
  EXPECT_EQ(db->command(command).result,
            (nlohmann::json{{"opt", "in"}, {"program_told", true}}));
  EXPECT_EQ(db->event(event)->opt, Opt::In);
  EXPECT_EQ(db->event(event)->optSource, OptSource::Operator);
  const auto entry = db->activityEntry(connection, "Opted in to event");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->actor, "ops");
  EXPECT_EQ(entry->eventId, event);

  {
    std::lock_guard lock(script->mutex);
    ASSERT_EQ(script->responses.size(), 1u);
    EXPECT_EQ(script->responses[0].second, Opt::In);
  }

  const auto again = db->addCommand(connection, CommandKind::OptIn, event);
  ASSERT_TRUE(commandDone(again));
  EXPECT_EQ(db->command(again).status, "succeeded");
  EXPECT_EQ(db->command(again).message, "Already opted in.");
}

TEST_F(RunnerTest, PausedConnectionCanOptInToOneEvent) {
  registered();
  {
    std::lock_guard lock(db->mutex);
    db->states[connection].participation = Participation::Paused;
  }
  const auto event = db->addEvent(connection, futureEvent("e1"), Opt::Out,
                                  OptSource::Paused);
  start();

  const auto command = db->addCommand(connection, CommandKind::OptIn, event);
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "succeeded");
  EXPECT_EQ(db->event(event)->opt, Opt::In);
  EXPECT_EQ(db->state(connection).participation, Participation::Paused);
}

TEST_F(RunnerTest, OptInIsRefusedOnceTheEventHasStartedOrEnded) {
  registered();
  auto running = futureEvent("running");
  running.startAt = now() - std::chrono::minutes(5);
  running.status = ProgramStatus::Active;
  const auto started =
      db->addEvent(connection, running, Opt::Out, OptSource::Operator);
  auto cancelled = futureEvent("cancelled");
  cancelled.status = ProgramStatus::Cancelled;
  const auto ended =
      db->addEvent(connection, cancelled, Opt::Out, OptSource::Operator);
  start();

  const auto late = db->addCommand(connection, CommandKind::OptIn, started);
  const auto over = db->addCommand(connection, CommandKind::OptIn, ended);
  ASSERT_TRUE(commandDone(late));
  ASSERT_TRUE(commandDone(over));
  EXPECT_EQ(db->command(late).status, "failed");
  EXPECT_EQ(db->command(late).message, "Event has already started.");
  EXPECT_EQ(db->command(over).message, "Event is already cancelled.");
  EXPECT_EQ(db->event(started)->opt, Opt::Out);

  std::lock_guard lock(script->mutex);
  EXPECT_TRUE(script->responses.empty());
}

TEST_F(RunnerTest, FailedOptInLeavesTheEventOptedOut) {
  registered();
  script->respondError = AdapterError::transient("timeout");
  const auto event = db->addEvent(connection, futureEvent("e1"), Opt::Out,
                                  OptSource::Operator);
  start();

  const auto command = db->addCommand(connection, CommandKind::OptIn, event);
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_EQ(db->event(event)->opt, Opt::Out);
}

TEST_F(RunnerTest, AnEventTheSiteCannotDeclineCannotBeOptedOutOf) {
  registered();
  auto limit = futureEvent("e1");
  limit.declinable = false;
  const auto event = db->addEvent(connection, limit);
  start();

  const auto command =
      db->addCommand(connection, CommandKind::OptOut, event, "ops");
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_NE(db->command(command).message.find("whatever the site answers"),
            std::string::npos);
  EXPECT_EQ(db->event(event)->opt, Opt::In);

  std::lock_guard lock(script->mutex);
  EXPECT_TRUE(script->responses.empty());
}

TEST_F(RunnerTest, OptOutOfAnEventWithoutAnswersIsLocal) {
  registered();
  auto quiet = futureEvent("e1");
  quiet.responseRequired = false;
  const auto event = db->addEvent(connection, quiet);
  start();

  const auto command =
      db->addCommand(connection, CommandKind::OptOut, event, "ops");
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "succeeded");
  EXPECT_NE(db->command(command).message.find("not told"), std::string::npos);
  EXPECT_EQ(db->event(event)->opt, Opt::Out);
  EXPECT_EQ(db->event(event)->optSource, OptSource::Operator);
  EXPECT_TRUE(db->responsesFor(event).empty());

  std::lock_guard lock(script->mutex);
  EXPECT_TRUE(script->responses.empty());
}

TEST_F(RunnerTest, FailedOptOutLeavesTheEventOptedIn) {
  registered();
  script->respondError = AdapterError::transient("timeout");
  const auto event = db->addEvent(connection, futureEvent("e1"));
  start();

  const auto command = db->addCommand(connection, CommandKind::OptOut, event);
  ASSERT_TRUE(commandDone(command));
  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_EQ(db->event(event)->opt, Opt::In);
  ASSERT_EQ(db->responsesFor(event).size(), 1u);
  EXPECT_FALSE(db->responsesFor(event)[0].ok);
}

TEST_F(RunnerTest, OptOutRejectsUnknownAndEndedEvents) {
  registered();
  const auto other = db->addConnection({});
  const auto foreign = db->addEvent(other, futureEvent("x"));
  auto ended = futureEvent("done");
  ended.status = ProgramStatus::Completed;
  const auto completed = db->addEvent(connection, ended);
  start();

  const auto unknown = db->addCommand(connection, CommandKind::OptOut, foreign);
  const auto late = db->addCommand(connection, CommandKind::OptOut, completed);
  ASSERT_TRUE(commandDone(unknown));
  ASSERT_TRUE(commandDone(late));
  EXPECT_EQ(db->command(unknown).message,
            "Event not found for this connection.");
  EXPECT_EQ(db->command(late).message, "Event is already completed.");
}

TEST_F(RunnerTest, InvalidConfigurationFailsProtocolCommandsOnly) {
  script->failCreate = "no client key";
  start();

  const auto registerCommand = db->addCommand(connection, CommandKind::Register);
  const auto pause = db->addCommand(connection, CommandKind::Pause);
  ASSERT_TRUE(commandDone(registerCommand));
  ASSERT_TRUE(commandDone(pause));
  EXPECT_EQ(db->command(registerCommand).status, "failed");
  EXPECT_NE(db->command(registerCommand).message.find("no client key"),
            std::string::npos);
  EXPECT_EQ(db->command(pause).status, "succeeded");
  EXPECT_TRUE(contains(db->activityMessages(connection),
                       "Connection configuration is invalid"));
  EXPECT_EQ(db->stateExtras(connection).configError, "no client key");
}

TEST_F(RunnerTest, ValidConfigurationClearsTheConfigurationError) {
  {
    std::lock_guard lock(db->mutex);
    db->extras[connection].configError = "no client key";
  }
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.extras.at(connection).heartbeats > 0;
  }));
  EXPECT_FALSE(db->stateExtras(connection).configError);
}

TEST_F(RunnerTest, TestCommandReportsTheOutcome) {
  start();
  const auto passed =
      db->addCommand(connection, CommandKind::Test, std::nullopt, "ops");
  ASSERT_TRUE(commandDone(passed));
  EXPECT_EQ(db->command(passed).message, "reached the fake VTN");
  EXPECT_EQ(db->command(passed).result,
            (nlohmann::json{{"registered", false}, {"client_id", nullptr}}));
  EXPECT_EQ(db->activityEntry(connection, "Connection test passed")->actor,
            "ops");

  {
    std::lock_guard lock(script->mutex);
    script->testResult = {"reached the fake VTN", true, "ven-1"};
  }
  const auto known = db->addCommand(connection, CommandKind::Test);
  ASSERT_TRUE(commandDone(known));
  EXPECT_EQ(db->command(known).result,
            (nlohmann::json{{"registered", true}, {"client_id", "ven-1"}}));

  {
    std::lock_guard lock(script->mutex);
    script->testError = AdapterError::permanent("HTTP 403");
  }
  const auto failed =
      db->addCommand(connection, CommandKind::Test, std::nullopt, "ops");
  ASSERT_TRUE(commandDone(failed));
  EXPECT_EQ(db->command(failed).status, "failed");
  EXPECT_TRUE(db->command(failed).result.is_null());
  EXPECT_EQ(db->activityEntry(connection, "Connection test failed")->actor,
            "ops");
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered)
      << "a failed test does not change registration";
}

TEST_F(RunnerTest, UnexpectedErrorsFailTheRunner) {
  registered();
  script->queuePoll([](ProgramEventSink &) -> PollResult {
    throw std::runtime_error("database went away");
  });
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    return runner->finished();
  }));
  EXPECT_EQ(runner->exitReason(), RunnerExit::Failed);
}

TEST_F(RunnerTest, StopsPromptlyWhileWaiting) {
  registered();
  options.defaultPollInterval = 60s;
  options.heartbeatInterval = 60s;
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    return script->polled() >= 1;
  }));

  const auto before = std::chrono::steady_clock::now();
  runner->requestStop();
  runner->join();
  EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms);
  EXPECT_EQ(runner->exitReason(), RunnerExit::Stopped);
}

TEST_F(RunnerTest, WakesForTheAdaptersDueWork) {
  registered();
  options.defaultPollInterval = 60s;
  options.heartbeatInterval = 60s;
  script->dueWorkIn = 50ms;
  start();

  EXPECT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) {
        std::lock_guard lock(script->mutex);
        return script->dueWorkRuns >= 3;
      },
      2s))
      << "long before the heartbeat";
  EXPECT_EQ(script->polled(), 1);
}

TEST_F(RunnerTest, PollsAtOnceWhenTheAdapterWantsIt) {
  registered();
  options.defaultPollInterval = 60s;
  options.heartbeatInterval = 60s;
  script->dueWorkIn = 50ms;
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    return script->polled() >= 1;
  }));

  {
    std::lock_guard lock(script->mutex);
    script->pollWanted = true;
  }
  EXPECT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 2; }, 2s))
      << "long before the poll interval";

  const auto runs = [&] {
    std::lock_guard lock(script->mutex);
    return script->dueWorkRuns;
  };
  const auto before = runs();
  ASSERT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return runs() >= before + 3; }, 2s));
  EXPECT_EQ(script->polled(), 2) << "once: the poll took the wish";
}

TEST_F(RunnerTest, PollsAtOnceWhenTheAdapterWantsItDuringABackoff) {
  registered();
  options.defaultPollInterval = 60s;
  options.heartbeatInterval = 60s;
  options.minBackoff = 60s;
  options.maxBackoff = 60s;
  script->dueWorkIn = 50ms;
  script->queuePollError(AdapterError::transient("the program is down"));
  start();
  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    return script->polled() >= 1;
  }));

  {
    std::lock_guard lock(script->mutex);
    script->pollWanted = true;
  }
  EXPECT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 2; }, 2s))
      << "long before the backoff ends";
}

namespace {
// The gaps between the first `count` + 1 polls.
std::vector<std::chrono::milliseconds>
pollGaps(const std::shared_ptr<FakeAdapterScript> &script, size_t count) {
  std::lock_guard lock(script->mutex);
  std::vector<std::chrono::milliseconds> gaps;
  for (size_t i = 1; i <= count; ++i) {
    gaps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
        script->pollTimes[i] - script->pollTimes[i - 1]));
  }
  return gaps;
}
} // namespace

TEST_F(RunnerTest, LengthensEachWaitBetweenPollsByTheConnectionsJitter) {
  registered();
  db->connections.at(connection).pollJitter = 1.0;
  options.defaultPollInterval = 100ms;
  start();

  ASSERT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 21; }, 10s));
  const auto gaps = pollGaps(script, 20);
  EXPECT_TRUE(std::all_of(gaps.begin(), gaps.end(),
                          [](auto gap) { return gap >= 100ms; }));
  EXPECT_TRUE(std::any_of(gaps.begin(), gaps.end(),
                          [](auto gap) { return gap > 150ms; }));
}

TEST_F(RunnerTest, PollsAtTheIntervalExactlyWhenTheAdapterDoesNotJitter) {
  registered();
  db->connections.at(connection).pollJitter = 1.0;
  options.defaultPollInterval = 100ms;
  script->jittersPolls = false;
  start();

  ASSERT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 21; }, 10s));
  for (const auto gap : pollGaps(script, 20)) {
    EXPECT_GE(gap, 100ms);
    EXPECT_LT(gap, 150ms);
  }
}

TEST_F(RunnerTest, StoresThePollIntervalAPollFinds) {
  registered();
  ASSERT_FALSE(script->resumeResult->pollIntervalSeconds);
  script->retryDelay = 30ms;
  script->queuePoll([](ProgramEventSink &) {
    PollResult result;
    result.pollIntervalSeconds = 60;
    result.moreQueued = true;
    return result;
  });
  script->queuePollError(AdapterError::transient("connection refused"));
  start();

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    std::lock_guard lock(script->mutex);
    return !script->retriesAsked.empty();
  }));
  EXPECT_EQ(db->state(connection).programPollIntervalSeconds, 60);
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->retriesAsked.front(),
            (std::pair<int, std::chrono::milliseconds>{1, 60s}))
      << "the interval the poll found, not the runner's default";
}

TEST_F(RunnerTest, KeepsTheStoredPollIntervalWhenAPollFindsNone) {
  registered();
  script->resumeResult = RegistrationInfo{"ven-1", "reg-1", 9};
  script->retryDelay = 30ms;
  script->queuePoll([](ProgramEventSink &) {
    PollResult result;
    result.moreQueued = true;
    return result;
  });
  script->queuePollError(AdapterError::transient("connection refused"));
  start();

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &) {
    std::lock_guard lock(script->mutex);
    return !script->retriesAsked.empty();
  }));
  EXPECT_EQ(db->state(connection).programPollIntervalSeconds, 9);
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->retriesAsked.front(),
            (std::pair<int, std::chrono::milliseconds>{1, 9s}));
}

TEST_F(RunnerTest, ExpiredCommandsAreLeftForTheSupervisor) {
  const auto command = db->addCommand(connection, CommandKind::Pause);
  db->commands.at(0).expired = true;
  const auto next = db->addCommand(connection, CommandKind::Test);
  start();
  ASSERT_TRUE(commandDone(next));
  EXPECT_EQ(db->command(command).status, "pending");
  EXPECT_EQ(db->state(connection).participation, Participation::Active);
}

// ---------------------------------------------------------------------------
// Retiring: a disabled or archived connection is unregistered, not run.

namespace {
class RetiringRunnerTest : public RunnerTest {
protected:
  void disable() {
    std::lock_guard lock(db->mutex);
    db->connections.at(connection).enabled = false;
  }

  bool finished() {
    return db->waitFor(
        [&](const MemoryDatabase &) { return runner->finished(); });
  }
};
} // namespace

TEST_F(RetiringRunnerTest, DisabledConnectionIsUnregisteredAndStops) {
  registered();
  disable();
  start();

  ASSERT_TRUE(finished());
  EXPECT_EQ(runner->exitReason(), RunnerExit::Stopped);
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered);
  EXPECT_FALSE(db->state(connection).registrationId);
  const auto entry = db->activityEntry(
      connection,
      "Unregistered from the program because the connection was disabled");
  ASSERT_TRUE(entry);
  EXPECT_FALSE(entry->actor);

  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->resumeCalls, 1);
  EXPECT_EQ(script->unregisterCalls, 1);
  EXPECT_EQ(script->pollCalls, 0);
}

TEST_F(RetiringRunnerTest, ArchivedConnectionIsUnregistered) {
  registered();
  {
    std::lock_guard lock(db->mutex);
    db->connections.at(connection).archived = true;
  }
  start();

  ASSERT_TRUE(finished());
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered);
  EXPECT_TRUE(contains(
      db->activityMessages(connection),
      "Unregistered from the program because the connection was archived"));
}

TEST_F(RetiringRunnerTest, UnregisteredConnectionJustStops) {
  disable();
  const auto command = db->addCommand(connection, CommandKind::Register);
  start();

  ASSERT_TRUE(finished());
  EXPECT_EQ(runner->exitReason(), RunnerExit::Stopped);
  EXPECT_EQ(db->command(command).status, "pending")
      << "a retiring connection runs no commands";
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->unregisterCalls, 0);
  EXPECT_EQ(script->registerCalls, 0);
}

TEST_F(RetiringRunnerTest, KeepsTryingWhileTheProgramCannotBeReached) {
  registered();
  disable();
  script->unregisterErrors = {AdapterError::transient("connection refused"),
                              AdapterError::transient("connection refused")};
  start();

  ASSERT_TRUE(finished());
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered);
  const auto messages = db->activityMessages(connection);
  EXPECT_EQ(std::count_if(messages.begin(), messages.end(),
                          [](const auto &m) {
                            return m.find("Unregistering the disabled "
                                          "connection failed") !=
                                   std::string::npos;
                          }),
            1);
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->unregisterCalls, 3);
}

TEST_F(RetiringRunnerTest, RetriesAtTheAdaptersDelay) {
  registered();
  disable();
  options.minBackoff = 60s;
  options.maxBackoff = 60s;
  script->retryDelay = 30ms;
  script->unregisterErrors = {AdapterError::transient("connection refused")};
  start();

  ASSERT_TRUE(finished());
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->unregisterCalls, 2);
  EXPECT_EQ(script->retriesAsked.size(), 1u);
}

TEST_F(RetiringRunnerTest, GivesUpWhenTheProgramRejectsUs) {
  registered();
  disable();
  script->unregisterErrors = {AdapterError::permanent("certificate rejected")};
  start();

  ASSERT_TRUE(finished());
  EXPECT_EQ(db->state(connection).registration, Registration::Error);
  EXPECT_NE(db->stateExtras(connection).lastError->find("certificate rejected"),
            std::string::npos);
}

TEST_F(RetiringRunnerTest, ForgetsARegistrationTheProgramNoLongerHas) {
  registered();
  disable();
  script->resumeResult.reset();
  start();

  ASSERT_TRUE(finished());
  EXPECT_EQ(db->state(connection).registration, Registration::Unregistered);
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->unregisterCalls, 0);
}

TEST_F(RetiringRunnerTest, InvalidConfigurationCannotBeUnregistered) {
  registered();
  disable();
  script->failCreate = "no client key";
  start();

  ASSERT_TRUE(finished());
  EXPECT_EQ(db->state(connection).registration, Registration::Error);
  EXPECT_TRUE(contains(db->activityMessages(connection),
                       "could not be unregistered"));
}

// ---------------------------------------------------------------------------
// OpenADR 2.0b Profile Specification v1.1, section 11.2 conformance rules.

namespace {
class RunnerConformanceTest : public RunnerTest {};
} // namespace

// Rule 500: "At each polling, VENs SHOULD, without waiting until the next
// polling interval, continue sending oadrPoll messages and processing payloads
// until an oadrResponse payload is returned to signal an empty VTN message
// queue."

TEST_F(RunnerConformanceTest, Rule500_PollsAgainUntilTheQueueIsEmpty) {
  registered();
  options.defaultPollInterval = 1h;
  script->queuePoll([](ProgramEventSink &) {
    return PollResult{PollOutcome::Ok,
                      "Received events from the program (1 changed)", true};
  });
  start();

  EXPECT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 2; }, 2s));
  // The second poll found the queue empty, so the next waits the interval.
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(script->polled(), 2);
}

// Rule 500: "The polling interval ... SHOULD be as requested by the VTN during
// registration, but MAY be higher because of processing constraints of the VEN
// or other factors. The polling interval SHOULD NOT be lower than the
// requested duration in oadrRequestedOadrPollFreq, except when the VEN is
// emptying the VTN payload queue."
TEST_F(RunnerConformanceTest, Rule500_PollIntervalIsNoShorterThanRequested) {
  registered();
  db->updateConnection(connection, [](ConnectionConfig &config) {
    config.pollIntervalSeconds = 1;
  });
  script->pollInterval = std::chrono::hours(1);
  start();

  ASSERT_TRUE(db->waitFor(
      [&](const MemoryDatabase &) { return script->polled() >= 1; }));
  std::this_thread::sleep_for(1500ms);
  EXPECT_EQ(script->polled(), 1);
}

// Section 9.1.8: "VENs must also be configurable to induce some 'jitter' – a
// random offset from the absolute poll interval – in order to avoid request
// spikes on the VTN caused by many VENs initiating a PULL request at the exact
// same instant within the poll interval."
// Rule 500: the interval "SHOULD NOT be lower than the requested duration in
// oadrRequestedOadrPollFreq", so jitter only lengthens it.
TEST(PollJitterConformance, Section9_1_8_JitterSpreadsPollsWithoutShorteningThem) {
  std::mt19937 random(42);
  std::set<std::chrono::milliseconds::rep> waits;
  for (int i = 0; i < 100; ++i) {
    const auto wait = withJitter(10'000ms, 0.2, random);
    EXPECT_GE(wait, 10'000ms);
    EXPECT_LE(wait, 12'000ms);
    waits.insert(wait.count());
  }
  EXPECT_GT(waits.size(), 50u);
}

TEST(PollJitterConformance, Section9_1_8_ZeroJitterKeepsTheInterval) {
  std::mt19937 random(42);
  EXPECT_EQ(withJitter(10'000ms, 0.0, random), 10'000ms);
  EXPECT_EQ(ConnectionConfig{}.pollJitter, 0.1);
}
