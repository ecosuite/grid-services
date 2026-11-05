#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

#include "supervisor/supervisor.hpp"
#include "support/fake_adapter.hpp"
#include "support/memory_store.hpp"

using namespace std::chrono_literals;
using namespace grid_services;
using namespace grid_services::testing;

namespace {

class SupervisorTest : public ::testing::Test {
protected:
  void SetUp() override {
    adapters.add("fake", fakeAdapters(script));
    runnerOptions.defaultPollInterval = 20ms;
    runnerOptions.heartbeatInterval = 20ms;
    options.reconcileInterval = 50ms;
    options.restartDelay = 20ms;
    options.notificationWait = 10ms;
    supervisor = std::make_unique<Supervisor>(
        store, memoryRunnerStores(db), adapters, runnerOptions, options);
  }

  std::vector<ConnectionId> running() {
    auto ids = supervisor->running();
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  int created() {
    std::lock_guard lock(script->mutex);
    return script->created;
  }

  // Adapters are created on runner threads, so wait for them.
  bool waitForCreated(int count) {
    return db->waitFor([&](const MemoryDatabase &) { return created() == count; });
  }

  // Reconciles until `predicate` holds.
  bool reconcileUntil(const std::function<bool()> &predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      supervisor->reconcile();
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  std::shared_ptr<MemoryDatabase> db = std::make_shared<MemoryDatabase>();
  MemorySupervisorStore store{db};
  std::shared_ptr<FakeAdapterScript> script =
      std::make_shared<FakeAdapterScript>();
  AdapterRegistry adapters;
  RunnerOptions runnerOptions;
  SupervisorOptions options;
  std::unique_ptr<Supervisor> supervisor;
};

} // namespace

TEST_F(SupervisorTest, RunsOneRunnerPerEnabledConnection) {
  const auto a = db->addConnection({});
  const auto b = db->addConnection({});
  ConnectionConfig disabled;
  disabled.enabled = false;
  db->addConnection(disabled);

  supervisor->reconcile();
  EXPECT_EQ(running(), (std::vector<ConnectionId>{a, b}));
  ASSERT_TRUE(waitForCreated(2));

  supervisor->reconcile();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(created(), 2) << "reconciling again does not restart runners";
}

TEST_F(SupervisorTest, StopsARunnerWhenItsConnectionIsDisabled) {
  const auto id = db->addConnection({});
  supervisor->reconcile();
  ASSERT_EQ(running().size(), 1u);

  db->updateConnection(id, [](ConnectionConfig &c) { c.enabled = false; });
  supervisor->reconcile();
  EXPECT_TRUE(running().empty());
}

TEST_F(SupervisorTest, UnregistersADisabledConnectionBeforeLettingItGo) {
  const auto id = db->addConnection({});
  {
    std::lock_guard lock(db->mutex);
    db->states[id].registration = Registration::Registered;
    db->states[id].registrationId = "reg-1";
  }
  supervisor->reconcile();
  ASSERT_TRUE(waitForCreated(1));

  db->updateConnection(id, [](ConnectionConfig &c) { c.enabled = false; });
  EXPECT_TRUE(reconcileUntil([&] {
    return db->state(id).registration == Registration::Unregistered &&
           running().empty();
  }));
  std::lock_guard lock(script->mutex);
  EXPECT_EQ(script->unregisterCalls, 1);
}

TEST_F(SupervisorTest, UnregistersAConnectionDisabledWhileNoWorkerRan) {
  ConnectionConfig disabled;
  disabled.enabled = false;
  const auto id = db->addConnection(disabled);
  {
    std::lock_guard lock(db->mutex);
    db->states[id].registration = Registration::Registered;
    db->states[id].registrationId = "reg-1";
  }

  EXPECT_TRUE(reconcileUntil([&] {
    return db->state(id).registration == Registration::Unregistered &&
           running().empty();
  }));
}

TEST_F(SupervisorTest, DoesNotRunArchivedConnections) {
  ConnectionConfig archived;
  archived.archived = true;
  const auto id = db->addConnection(archived);
  const auto command = db->addCommand(id, CommandKind::Test);

  supervisor->reconcile();
  EXPECT_TRUE(running().empty());
  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_EQ(db->command(command).message, "The connection is archived.");
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(created(), 0);
}

TEST_F(SupervisorTest, StopsARunnerWhenItsConnectionIsArchived) {
  const auto id = db->addConnection({});
  supervisor->reconcile();
  ASSERT_EQ(running().size(), 1u);

  db->updateConnection(id, [](ConnectionConfig &c) { c.archived = true; });
  supervisor->reconcile();
  EXPECT_TRUE(running().empty());
}

TEST_F(SupervisorTest, FailsExpiredCommands) {
  const auto id = db->addConnection({});
  const auto command = db->addCommand(id, CommandKind::Test);
  {
    std::lock_guard lock(db->mutex);
    db->commands.at(0).expired = true;
  }

  supervisor->reconcile();
  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_EQ(db->command(command).message,
            "The command expired before a worker picked it up.");
}

TEST_F(SupervisorTest, RestartsARunnerWhenItsConfigurationChanges) {
  const auto id = db->addConnection({});
  supervisor->reconcile();
  ASSERT_TRUE(waitForCreated(1));

  db->updateConnection(id, [](ConnectionConfig &c) { c.defaultOpt = Opt::Out; });
  supervisor->reconcile();
  EXPECT_TRUE(waitForCreated(2));
  EXPECT_EQ(running(), (std::vector<ConnectionId>{id}));
}

TEST_F(SupervisorTest, FailsCommandsForDisabledConnections) {
  ConnectionConfig disabled;
  disabled.enabled = false;
  const auto id = db->addConnection(disabled);
  const auto command = db->addCommand(id, CommandKind::Test);

  supervisor->reconcile();
  EXPECT_EQ(db->command(command).status, "failed");
  EXPECT_EQ(db->command(command).message, "The connection is disabled.");
}

TEST_F(SupervisorTest, LeavesAConnectionHeldByAnotherWorker) {
  const auto id = db->addConnection({});
  {
    std::lock_guard lock(db->mutex);
    db->locks.insert(id);
  }
  supervisor->reconcile();
  EXPECT_TRUE(running().empty());
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(created(), 0);

  {
    std::lock_guard lock(db->mutex);
    db->locks.erase(id);
  }
  EXPECT_TRUE(reconcileUntil([&] { return running().size() == 1; }));
}

TEST_F(SupervisorTest, RestartsAFailedRunnerAfterADelay) {
  const auto id = db->addConnection({});
  {
    std::lock_guard lock(db->mutex);
    db->states[id].registration = Registration::Registered;
    db->states[id].registrationId = "reg-1";
  }
  script->queuePoll([](ProgramEventSink &) -> PollResult {
    throw std::runtime_error("boom");
  });

  supervisor->reconcile();
  EXPECT_TRUE(reconcileUntil([&] { return created() == 2; }));
  EXPECT_EQ(running(), (std::vector<ConnectionId>{id}));
}

TEST_F(SupervisorTest, RestartsARunnerThatAskedToReload) {
  const auto id = db->addConnection({});
  supervisor->reconcile();
  db->addCommand(id, CommandKind::ReloadConfig);
  EXPECT_TRUE(reconcileUntil([&] { return created() == 2; }));
}

TEST_F(SupervisorTest, RunLoopReactsToNotificationsAndStopsRunners) {
  const auto id = db->addConnection({});
  // A long heartbeat, so only the command notification can wake the runner.
  runnerOptions.heartbeatInterval = 60s;
  supervisor = std::make_unique<Supervisor>(
      store, memoryRunnerStores(db), adapters, runnerOptions, options);

  std::stop_source stop;
  std::thread loop([&] { supervisor->run(stop.get_token()); });

  ASSERT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.extras.count(id) && d.extras.at(id).heartbeats > 0;
  }));

  const auto command = db->addCommand(id, CommandKind::Pause);
  EXPECT_TRUE(db->waitFor(
      [&](const MemoryDatabase &d) {
        return d.commands.at(static_cast<size_t>(command) - 1).status ==
               "succeeded";
      },
      2s));

  const auto second = db->addConnection({});
  db->notify({Notification::Channel::Config, second});
  EXPECT_TRUE(db->waitFor([&](const MemoryDatabase &d) {
    return d.extras.count(second) > 0;
  }));

  stop.request_stop();
  loop.join();
  EXPECT_TRUE(supervisor->running().empty());
}
