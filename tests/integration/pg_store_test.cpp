#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <pqxx/pqxx>

#include "store/pg_store.hpp"
#include "supervisor/supervisor.hpp"
#include "support/fake_adapter.hpp"

using namespace std::chrono_literals;
using namespace grid_services;
using namespace grid_services::testing;

// These tests need a disposable Postgres database; they drop and recreate
// its public schema. Set GRID_SERVICES_TEST_DATABASE_URL to run them.

namespace {

std::string readFile(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::stringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

std::vector<std::filesystem::path> migrationFiles() {
  std::vector<std::filesystem::path> files;
  for (const auto &entry :
       std::filesystem::directory_iterator(GRID_SERVICES_MIGRATIONS_DIR)) {
    if (entry.path().extension() == ".sql") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// The "-- migrate:up" or "-- migrate:down" section of a dbmate migration.
std::string migrationSection(const std::filesystem::path &file,
                             const std::string &section) {
  const auto text = readFile(file);
  const auto up = text.find("-- migrate:up");
  const auto down = text.find("-- migrate:down");
  if (up == std::string::npos || down == std::string::npos || down < up) {
    throw std::runtime_error("malformed migration " + file.string());
  }
  return section == "up" ? text.substr(up, down - up) : text.substr(down);
}

void migrate(pqxx::connection &connection, const std::string &direction) {
  auto files = migrationFiles();
  if (direction == "down") {
    std::reverse(files.begin(), files.end());
  }
  pqxx::nontransaction tx(connection);
  for (const auto &file : files) {
    tx.exec(migrationSection(file, direction));
  }
}

// The migration whose file name contains `name`, and every later one.
std::vector<std::filesystem::path> migrationsFrom(const std::string &name) {
  const auto files = migrationFiles();
  const auto first =
      std::find_if(files.begin(), files.end(), [&](const auto &file) {
        return file.filename().string().find(name) != std::string::npos;
      });
  if (first == files.end()) {
    throw std::runtime_error("no migration named " + name);
  }
  return {first, files.end()};
}

// Takes the database back to before the migration named `name`.
void rollBackTo(pqxx::connection &connection, const std::string &name) {
  auto files = migrationsFrom(name);
  std::reverse(files.begin(), files.end());
  pqxx::nontransaction tx(connection);
  for (const auto &file : files) {
    tx.exec(migrationSection(file, "down"));
  }
}

// Ecosuite's id for the site with this code, as SQL.
std::string siteId(const std::string &site) {
  return "md5('" + site + "')::uuid";
}

// Collects the payloads notified on one channel.
class ChannelListener : public pqxx::notification_receiver {
public:
  ChannelListener(pqxx::connection &connection, const std::string &channel)
      : pqxx::notification_receiver(connection, channel),
        connection_(connection) {}

  void operator()(const std::string &payload, int) override {
    payloads_.push_back(payload);
  }

  // What has been notified since the last call, waiting a moment for it.
  // Notifications sent by separate statements arrive separately, so when
  // several are expected the wait goes on until they are all in.
  std::vector<std::string> heard(std::size_t expected = 1) {
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    do {
      connection_.await_notification(0, 300'000);
      connection_.get_notifs();
    } while (payloads_.size() < expected &&
             std::chrono::steady_clock::now() < deadline);
    std::vector<std::string> taken;
    taken.swap(payloads_);
    return taken;
  }

private:
  pqxx::connection &connection_;
  std::vector<std::string> payloads_;
};

// The role the ecosuite API connects as, with the grants the README gives
// it.
const char *clientGrants = R"(
    DO $$ BEGIN
        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'ecosuite_api') THEN
            CREATE ROLE ecosuite_api;
        END IF;
    END $$;
    GRANT USAGE ON SCHEMA public TO ecosuite_api;
    GRANT SELECT, INSERT, UPDATE, DELETE ON connections, openadr2b_connections, csipaus_connections, client_credentials TO ecosuite_api;
    GRANT SELECT, INSERT ON connection_commands TO ecosuite_api;
    GRANT SELECT ON connection_state, event_responses TO ecosuite_api;
    GRANT SELECT ON connection_health, event_state TO ecosuite_api;
    GRANT SELECT, INSERT, UPDATE, DELETE ON events TO ecosuite_api;
    GRANT SELECT, INSERT ON activity TO ecosuite_api;
    GRANT SELECT, INSERT, UPDATE, DELETE ON telemetry_points, openadr2b_report_offers, connection_data_credentials, csipaus_der, csipaus_readings TO ecosuite_api;
    GRANT SELECT ON openadr2b_report_requests, report_deliveries, csipaus_events, csipaus_limits TO ecosuite_api;
    GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO ecosuite_api;
)";

class PgStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char *url = std::getenv("GRID_SERVICES_TEST_DATABASE_URL");
    if (url == nullptr || url[0] == '\0') {
      GTEST_SKIP() << "GRID_SERVICES_TEST_DATABASE_URL is not set";
    }
    connectionString = url;
    admin = std::make_unique<pqxx::connection>(connectionString);
    {
      pqxx::nontransaction tx(*admin);
      tx.exec("DROP SCHEMA public CASCADE; CREATE SCHEMA public;");
    }
    migrate(*admin, "up");
  }

  pqxx::result sql(const std::string &query) {
    pqxx::nontransaction tx(*admin);
    return tx.exec(query);
  }

  std::string value(const std::string &query) {
    return sql(query)[0][0].as<std::string>();
  }

  // Runs a query as the ecosuite API's role.
  pqxx::result client(const std::string &query) {
    if (!clientSession) {
      sql(clientGrants);
      clientSession = std::make_unique<pqxx::connection>(connectionString);
      pqxx::nontransaction tx(*clientSession);
      tx.exec("SET ROLE ecosuite_api");
    }
    pqxx::nontransaction tx(*clientSession);
    return tx.exec(query);
  }

  EventRowId addManualEvent(const std::string &site = "ML") {
    return client("INSERT INTO events (site_code, site_id, source, start_at, "
                  "duration_seconds, created_by) VALUES ('" +
                  site + "', " + siteId(site) +
                  ", 'manual', now() + interval '1 day', 3600, 'ops') "
                  "RETURNING id")[0][0]
        .as<EventRowId>();
  }

  ConnectionId addConnection(const std::string &site = "ML",
                             bool enabled = true) {
    const auto result = sql(
        "INSERT INTO connections (site_code, site_id, protocol, enabled) "
        "VALUES ('" +
        site + "', " + siteId(site) + ", 'openadr2b', " +
        (enabled ? "true" : "false") + ") RETURNING id");
    const auto id = result[0][0].as<ConnectionId>();
    sql("INSERT INTO openadr2b_connections (connection_id, vtn_url, ven_name, "
        "ven_id) VALUES (" +
        std::to_string(id) + ", 'https://vtn.example.com', 'ven', 'ven-1')");
    return id;
  }

  ConnectionId addCsipausConnection(const std::string &site = "ML") {
    const auto id =
        sql("INSERT INTO connections (site_code, site_id, protocol) VALUES ('" +
            site + "', " + siteId(site) + ", 'csipaus') RETURNING id")[0][0]
            .as<ConnectionId>();
    sql("INSERT INTO csipaus_connections (connection_id, dcap_url) VALUES (" +
        std::to_string(id) + ", 'https://utility.example.com/dcap')");
    return id;
  }

  std::unique_ptr<PgRunnerStore> runnerStore(ConnectionId id) {
    return std::make_unique<PgRunnerStore>(connectionString, id);
  }

  NormalizedEvent event(const std::string &id, int modification = 0) {
    NormalizedEvent e;
    e.programEventId = id;
    e.modificationNumber = modification;
    e.startAt = now() + std::chrono::hours(24);
    e.durationSeconds = 3600;
    e.signals = {{"simple", "level", "s1", {{0, 3600, 1.0}}}};
    e.programRequestId = "req-1";
    return e;
  }

  std::string connectionString;
  std::unique_ptr<pqxx::connection> admin;
  std::unique_ptr<pqxx::connection> clientSession;
};

} // namespace

TEST_F(PgStoreTest, MigrationsRollBackCleanly) {
  migrate(*admin, "down");
  EXPECT_EQ(sql("SELECT count(*) FROM pg_tables WHERE schemaname = 'public'")[0][0]
                .as<int>(),
            0);
  EXPECT_EQ(sql("SELECT count(*) FROM pg_proc p JOIN pg_namespace n "
                "ON n.oid = p.pronamespace WHERE n.nspname = 'public'")[0][0]
                .as<int>(),
            0);
  migrate(*admin, "up");
  EXPECT_NO_THROW(addConnection());
}

TEST_F(PgStoreTest, LoadsConfigurationAndCreatesState) {
  const auto id = addConnection();
  auto store = runnerStore(id);

  const auto config = store->loadConfig();
  EXPECT_EQ(config.siteCode, "ML");
  EXPECT_EQ(config.protocol, "openadr2b");
  EXPECT_EQ(config.defaultOpt, Opt::In);
  EXPECT_EQ(config.configVersion, 2) << "adding settings bumps the version";

  const auto settings = store->loadOpenadr2bSettings();
  EXPECT_EQ(settings.vtnUrl, "https://vtn.example.com");
  EXPECT_EQ(settings.venId, "ven-1");
  EXPECT_EQ(settings.acceptedSignals, std::vector<std::string>{"SIMPLE"});
  EXPECT_FALSE(store->loadClientCredential());

  const auto state = store->loadState();
  EXPECT_EQ(state.registration, Registration::Unregistered);
  EXPECT_EQ(state.participation, Participation::Active);

  EXPECT_FALSE(config.archived);

  store->setRegistered({"ven-1", "reg-1", 10});
  store->markStarted(config.configVersion, "worker-a", std::nullopt);
  const auto reloaded = store->loadState();
  EXPECT_EQ(reloaded.registration, Registration::Registered);
  EXPECT_EQ(reloaded.clientId, "ven-1");
  EXPECT_EQ(value("SELECT client_id FROM connection_health"), "ven-1");
  EXPECT_EQ(reloaded.registrationId, "reg-1");
  EXPECT_EQ(reloaded.programPollIntervalSeconds, 10);
  EXPECT_EQ(sql("SELECT worker_id FROM connection_state")[0][0].as<std::string>(),
            "worker-a");

  store->clearRegistration();
  EXPECT_FALSE(store->loadState().registrationId);
}

TEST_F(PgStoreTest, ThePollIntervalAPollFindsReplacesRegisterings) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  store->loadState();
  store->setRegistered({"ven-1", "reg-1", std::nullopt});
  EXPECT_FALSE(store->loadState().programPollIntervalSeconds);

  store->setProgramPollInterval(60);
  const auto state = store->loadState();
  EXPECT_EQ(state.programPollIntervalSeconds, 60);
  EXPECT_EQ(state.registration, Registration::Registered);
  EXPECT_EQ(state.registrationId, "reg-1");

  store->clearRegistration();
  EXPECT_FALSE(store->loadState().programPollIntervalSeconds);
}

TEST_F(PgStoreTest, ConnectionLockIsExclusiveAcrossSessions) {
  const auto id = addConnection();
  auto first = runnerStore(id);
  auto second = runnerStore(id);
  EXPECT_TRUE(first->tryLock());
  EXPECT_FALSE(second->tryLock());
  first.reset();
  EXPECT_TRUE(second->tryLock());
}

TEST_F(PgStoreTest, CommandsAreClaimedOnceInOrder) {
  const auto id = addConnection();
  sql("INSERT INTO connection_commands (connection_id, kind, requested_by) "
      "VALUES (" +
      std::to_string(id) + ", 'test', 'ops'), (" + std::to_string(id) +
      ", 'pause', NULL)");
  auto store = runnerStore(id);

  const auto first = store->claimNextCommand();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->kind, CommandKind::Test);
  EXPECT_EQ(first->requestedBy, "ops");
  const auto second = store->claimNextCommand();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->kind, CommandKind::Pause);
  EXPECT_FALSE(store->claimNextCommand());

  store->completeCommand(first->id,
                         {true, "done", {{"ven_id", "ven-1"}}});
  EXPECT_EQ(store->failInterruptedCommands("worker restarted"), 1);
  const auto statuses = sql("SELECT status, message, result ->> 'ven_id', "
                            "result IS NULL FROM connection_commands "
                            "ORDER BY id");
  EXPECT_EQ(statuses[0][0].as<std::string>(), "succeeded");
  EXPECT_EQ(statuses[0][2].as<std::string>(), "ven-1");
  EXPECT_EQ(statuses[1][0].as<std::string>(), "failed");
  EXPECT_EQ(statuses[1][1].as<std::string>(), "worker restarted");
  EXPECT_TRUE(statuses[1][3].as<bool>());
}

TEST_F(PgStoreTest, ExpiredCommandsAreFailedNotRun) {
  const auto id = addConnection();
  const auto connection = std::to_string(id);
  sql("INSERT INTO connection_commands (connection_id, kind, expires_at) "
      "VALUES (" +
      connection + ", 'register', now() - interval '1 second'), (" +
      connection + ", 'test', DEFAULT)");
  EXPECT_EQ(value("SELECT round(extract(epoch FROM expires_at - created_at)) "
                  "FROM connection_commands WHERE kind = 'test'"),
            "120");

  auto store = runnerStore(id);
  const auto claimed = store->claimNextCommand();
  ASSERT_TRUE(claimed);
  EXPECT_EQ(claimed->kind, CommandKind::Test);
  EXPECT_FALSE(store->claimNextCommand());

  PgSupervisorStore supervisor(connectionString);
  EXPECT_EQ(supervisor.failExpiredCommands(), 1);
  EXPECT_EQ(supervisor.failExpiredCommands(), 0);
  const auto expired = sql("SELECT status, message, completed_at IS NOT NULL "
                           "FROM connection_commands WHERE kind = 'register'");
  EXPECT_EQ(expired[0][0].as<std::string>(), "failed");
  EXPECT_EQ(expired[0][1].as<std::string>(),
            "The command expired before a worker picked it up.");
  EXPECT_TRUE(expired[0][2].as<bool>());
}

TEST_F(PgStoreTest, CommandResultsAreSignalled) {
  const auto id = addConnection();
  pqxx::connection listening(connectionString);
  ChannelListener results(listening, "grid_services_command_results");

  const auto command =
      sql("INSERT INTO connection_commands (connection_id, kind) VALUES (" +
          std::to_string(id) + ", 'test') RETURNING id")[0][0]
          .as<std::string>();
  auto store = runnerStore(id);
  ASSERT_TRUE(store->claimNextCommand());
  EXPECT_TRUE(results.heard().empty()) << "claiming is not a result";

  store->completeCommand(std::stoll(command), {false, "unreachable"});
  EXPECT_EQ(results.heard(), std::vector<std::string>{command});

  sql("UPDATE connection_commands SET message = 'edited'");
  EXPECT_TRUE(results.heard().empty());
}

TEST_F(PgStoreTest, HealthChangesAreSignalled) {
  pqxx::connection listening(connectionString);
  ChannelListener health(listening, "grid_services_health");
  // Whether the connection was signalled, and nothing else was.
  const auto signalled = [&](ConnectionId id) {
    const auto heard = health.heard();
    return !heard.empty() &&
           std::all_of(heard.begin(), heard.end(), [&](const auto &payload) {
             return payload == std::to_string(id);
           });
  };

  const auto id = addConnection();
  EXPECT_TRUE(signalled(id)) << "a new connection";
  auto store = runnerStore(id);
  store->loadState();
  EXPECT_TRUE(signalled(id)) << "its state";

  store->markStarted(store->loadConfig().configVersion, "worker-a",
                     std::nullopt);
  EXPECT_TRUE(signalled(id)) << "the configuration was applied";
  store->setRegistered({"ven-1", "reg-1", 10});
  EXPECT_TRUE(signalled(id)) << "registered";

  store->heartbeat(now());
  store->recordPoll(now());
  EXPECT_TRUE(health.heard().empty()) << "routine work is not a change";

  const auto retry = now() + std::chrono::seconds(40);
  store->recordFailures(1, retry);
  EXPECT_TRUE(signalled(id)) << "lost contact";
  store->recordFailures(2, retry);
  EXPECT_TRUE(health.heard().empty()) << "still failing is not a change";
  store->recordPoll(now());
  EXPECT_TRUE(signalled(id)) << "contact restored";

  store->setParticipation(Participation::Paused);
  EXPECT_TRUE(signalled(id)) << "paused";
  sql("UPDATE connections SET enabled = false");
  EXPECT_TRUE(signalled(id)) << "disabled";
  sql("UPDATE openadr2b_connections SET market_context = 'mc'");
  EXPECT_TRUE(signalled(id)) << "a change of settings is pending";
}

TEST_F(PgStoreTest, OptCommandsNeedAnEvent) {
  const auto id = addConnection();
  const auto connection = std::to_string(id);
  auto store = runnerStore(id);
  const auto event =
      store->upsertProgramEvent(this->event("e1"), Opt::Out, OptSource::Paused);

  EXPECT_THROW(sql("INSERT INTO connection_commands (connection_id, kind) "
                   "VALUES (" +
                   connection + ", 'opt_in')"),
               pqxx::check_violation);
  EXPECT_THROW(sql("INSERT INTO connection_commands (connection_id, kind, "
                   "event_id) VALUES (" +
                   connection + ", 'pause', " + std::to_string(event.id) + ")"),
               pqxx::check_violation);
  sql("INSERT INTO connection_commands (connection_id, kind, event_id) "
      "VALUES (" +
      connection + ", 'opt_in', " + std::to_string(event.id) + ")");

  const auto claimed = store->claimNextCommand();
  ASSERT_TRUE(claimed);
  EXPECT_EQ(claimed->kind, CommandKind::OptIn);
  EXPECT_EQ(claimed->eventId, event.id);
}

TEST_F(PgStoreTest, ProgramEventsKeepTheirOptAndIgnoreStaleUpdates) {
  const auto id = addConnection();
  auto store = runnerStore(id);

  const auto inserted = store->upsertProgramEvent(event("e1"), Opt::In,
                                                  OptSource::Default);
  EXPECT_EQ(inserted.programEventId, "e1");
  EXPECT_EQ(inserted.programRequestId, "req-1");
  EXPECT_FALSE(inserted.respondedModification);

  store->setOpt(inserted.id, Opt::Out, OptSource::Operator);
  const auto updated = store->upsertProgramEvent(event("e1", 2), Opt::In,
                                                 OptSource::Default);
  EXPECT_EQ(updated.id, inserted.id);
  EXPECT_EQ(updated.modificationNumber, 2);
  EXPECT_EQ(updated.opt, Opt::Out);
  EXPECT_EQ(updated.optSource, OptSource::Operator);

  const auto stale = store->upsertProgramEvent(event("e1", 1), Opt::In,
                                               OptSource::Default);
  EXPECT_EQ(stale.modificationNumber, 2);

  store->recordResponse(inserted.id, EventAnswer::OptOut, 2, true, "");
  store->recordResponse(inserted.id, EventAnswer::OptOut, 3, false, "timeout");
  EXPECT_EQ(store->findProgramEvent("e1")->respondedModification, 2);

  const auto stored = sql("SELECT site_code, signals, source FROM events");
  EXPECT_EQ(stored[0][0].as<std::string>(), "ML");
  EXPECT_NE(stored[0][1].as<std::string>().find("\"offset_seconds\": 0"),
            std::string::npos);
}

TEST_F(PgStoreTest, ProgramEventsKeepTheirRandomizationWindow) {
  const auto id = addConnection();
  auto store = runnerStore(id);

  auto randomized = event("e1");
  randomized.startAfterSeconds = 600;
  store->upsertProgramEvent(randomized, Opt::In, OptSource::Default);
  EXPECT_EQ(sql("SELECT start_after_seconds FROM events")[0][0].as<int>(), 600);

  store->upsertProgramEvent(event("e1", 1), Opt::In, OptSource::Default);
  EXPECT_TRUE(sql("SELECT start_after_seconds FROM events")[0][0].is_null());
}

TEST_F(PgStoreTest, EventQueriesAreScopedToTheConnection) {
  const auto mine = addConnection("ML");
  const auto other = addConnection("OT");
  auto store = runnerStore(mine);
  auto otherStore = runnerStore(other);

  const auto open = store->upsertProgramEvent(event("open"), Opt::In,
                                              OptSource::Default);
  const auto done = store->upsertProgramEvent(event("done"), Opt::In,
                                              OptSource::Default);
  store->setProgramStatus(done.id, ProgramStatus::Completed);
  const auto foreign = otherStore->upsertProgramEvent(
      event("open"), Opt::In, OptSource::Default);

  const auto openEvents = store->openProgramEvents();
  ASSERT_EQ(openEvents.size(), 1u);
  EXPECT_EQ(openEvents[0].id, open.id);
  EXPECT_TRUE(store->findEventById(done.id));
  EXPECT_FALSE(store->findEventById(foreign.id));
}

TEST_F(PgStoreTest, ActivityCarriesSiteActorAndDetail) {
  const auto id = addConnection("ML");
  auto store = runnerStore(id);
  store->appendActivity({.category = ActivityCategory::Connection,
                         .message = "Registered",
                         .detail = {{"exchange", nlohmann::json::array()}},
                         .actor = "ops"});
  store->appendActivity({.category = ActivityCategory::Events,
                         .message = "Event received"});

  const auto rows = sql("SELECT site_code, category, actor, detail IS NULL "
                        "FROM activity ORDER BY id");
  EXPECT_EQ(rows[0][0].as<std::string>(), "ML");
  EXPECT_EQ(rows[0][2].as<std::string>(), "ops");
  EXPECT_FALSE(rows[0][3].as<bool>());
  EXPECT_EQ(rows[1][1].as<std::string>(), "events");
  EXPECT_EQ(rows[1][2].as<std::string>(), "system");
  EXPECT_TRUE(rows[1][3].as<bool>());
}

TEST_F(PgStoreTest, SupervisorStoreListsAndHearsChanges) {
  const auto id = addConnection();
  PgSupervisorStore store(connectionString);

  const auto connections = store.listConnections();
  ASSERT_EQ(connections.size(), 1u);
  EXPECT_EQ(connections[0].configVersion, 2);
  store.waitForNotifications(10ms); // subscribes

  sql("UPDATE openadr2b_connections SET market_context = 'mc'");
  auto notifications = store.waitForNotifications(2s);
  ASSERT_FALSE(notifications.empty());
  EXPECT_EQ(notifications[0].channel, Notification::Channel::Config);
  EXPECT_EQ(notifications[0].connectionId, id);
  EXPECT_EQ(store.listConnections()[0].configVersion, 3);

  sql("INSERT INTO connection_commands (connection_id, kind) VALUES (" +
      std::to_string(id) + ", 'test')");
  notifications = store.waitForNotifications(2s);
  ASSERT_FALSE(notifications.empty());
  EXPECT_EQ(notifications[0].channel, Notification::Channel::Commands);
}

TEST_F(PgStoreTest, FailsPendingCommandsForDisabledConnections) {
  const auto disabled = addConnection("ML", false);
  const auto enabled = addConnection("OT", true);
  sql("INSERT INTO connection_commands (connection_id, kind) VALUES (" +
      std::to_string(disabled) + ", 'test'), (" + std::to_string(enabled) +
      ", 'test')");
  PgSupervisorStore store(connectionString);
  EXPECT_EQ(store.failCommandsForDisabledConnections(), 1);
}

TEST_F(PgStoreTest, RecordsFailuresAndConfigurationErrors) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  store->loadState();

  store->markStarted(2, "worker-a", "no client key");
  EXPECT_EQ(value("SELECT config_error FROM connection_state"),
            "no client key");

  const auto retry = now() + std::chrono::seconds(40);
  store->recordFailures(3, retry);
  auto row = sql("SELECT consecutive_failures, "
                 "extract(epoch FROM next_retry_at)::bigint "
                 "FROM connection_state");
  EXPECT_EQ(row[0][0].as<int>(), 3);
  EXPECT_EQ(row[0][1].as<long long>(), retry.time_since_epoch().count());

  store->recordPoll(now());
  row = sql("SELECT consecutive_failures, next_retry_at IS NULL "
            "FROM connection_state");
  EXPECT_EQ(row[0][0].as<int>(), 0);
  EXPECT_TRUE(row[0][1].as<bool>());

  store->recordFailures(2, retry);
  store->markStarted(2, "worker-a", std::nullopt);
  row = sql("SELECT consecutive_failures, next_retry_at IS NULL, "
            "config_error IS NULL FROM connection_state");
  EXPECT_EQ(row[0][0].as<int>(), 0);
  EXPECT_TRUE(row[0][1].as<bool>());
  EXPECT_TRUE(row[0][2].as<bool>());
}

TEST_F(PgStoreTest, ConnectionHealthResolvesToOneValue) {
  const auto id = addConnection();
  const auto health = [&] {
    return value("SELECT health FROM connection_health WHERE connection_id = " +
                 std::to_string(id));
  };
  const auto state = [&](const std::string &assignments) {
    sql("UPDATE connection_state SET " + assignments);
  };

  // No worker has picked the new connection up yet.
  EXPECT_EQ(health(), "unregistered");
  EXPECT_EQ(value("SELECT config_pending FROM connection_health"), "t");

  auto store = runnerStore(id);
  store->loadState();
  store->markStarted(store->loadConfig().configVersion, "worker-a",
                     std::nullopt);
  EXPECT_EQ(health(), "unregistered");
  EXPECT_EQ(value("SELECT config_pending FROM connection_health"), "f");

  store->setRegistered({"ven-1", "reg-1", 10});
  store->recordPoll(now());
  EXPECT_EQ(health(), "healthy");

  store->recordError("Poll failed: connection refused", now());
  store->recordFailures(1, now() + std::chrono::seconds(5));
  EXPECT_EQ(health(), "degraded");
  EXPECT_EQ(value("SELECT consecutive_failures FROM connection_health"), "1");

  store->setRegistration(Registration::Error);
  EXPECT_EQ(health(), "error");

  store->markStarted(2, "worker-a", "no client key");
  EXPECT_EQ(health(), "invalid_config");

  // The worker stopped reporting, and nothing changed for it to pick up.
  state("heartbeat_at = now() - interval '3 minutes'");
  EXPECT_EQ(health(), "invalid_config")
      << "a configuration saved moments ago is given time to be applied";
  sql("ALTER TABLE connections DISABLE TRIGGER USER");
  sql("UPDATE connections SET updated_at = now() - interval '3 minutes'");
  sql("ALTER TABLE connections ENABLE TRIGGER USER");
  EXPECT_EQ(health(), "worker_down");

  sql("UPDATE connections SET enabled = false");
  EXPECT_EQ(health(), "disabled");
  sql("UPDATE connections SET enabled = true, archived_at = now()");
  EXPECT_EQ(health(), "disabled");
  EXPECT_EQ(value("SELECT count(*) FROM connection_health"), "1");
}

TEST_F(PgStoreTest, EventStateResolvesToOneValue) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  const auto stored =
      store->upsertProgramEvent(event("e1"), Opt::In, OptSource::Default);
  const auto state = [&] {
    return value("SELECT state FROM event_state WHERE id = " +
                 std::to_string(stored.id));
  };

  EXPECT_EQ(state(), "received");
  sql("UPDATE events SET lifecycle = 'control'");
  EXPECT_EQ(state(), "control");

  store->setOpt(stored.id, Opt::Out, OptSource::Operator);
  EXPECT_EQ(state(), "opted_out");

  store->setProgramStatus(stored.id, ProgramStatus::Superseded);
  EXPECT_EQ(state(), "superseded");

  store->setProgramStatus(stored.id, ProgramStatus::Cancelled);
  EXPECT_EQ(state(), "cancelled");

  const auto window = sql("SELECT end_at = start_at + interval '1 hour', "
                          "created_by IS NULL FROM event_state");
  EXPECT_TRUE(window[0][0].as<bool>());
  EXPECT_TRUE(window[0][1].as<bool>());

  auto openEnded = event("open-ended");
  openEnded.durationSeconds = 0;
  openEnded.signals = {{"simple", "level", "s1", {{0, 0, 1.0}}}};
  store->upsertProgramEvent(openEnded, Opt::In, OptSource::Default);
  EXPECT_EQ(value("SELECT end_at IS NULL FROM event_state "
                  "WHERE program_event_id = 'open-ended'"),
            "t");
}

TEST_F(PgStoreTest, EventChangesAreSignalled) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  pqxx::connection listening(connectionString);
  ChannelListener events(listening, "grid_services_events");

  const auto stored =
      store->upsertProgramEvent(event("e1"), Opt::In, OptSource::Default);
  const auto row = std::to_string(stored.id);
  EXPECT_EQ(events.heard(), std::vector<std::string>{row}) << "insert";

  store->upsertProgramEvent(event("e1", 1), Opt::In, OptSource::Default);
  EXPECT_EQ(events.heard(), std::vector<std::string>{row}) << "modification";

  store->upsertProgramEvent(event("e1", 1), Opt::In, OptSource::Default);
  EXPECT_TRUE(events.heard().empty()) << "a repeated modification";

  store->setOpt(stored.id, Opt::Out, OptSource::Operator);
  EXPECT_EQ(events.heard(), std::vector<std::string>{row}) << "opt";

  store->setProgramStatus(stored.id, ProgramStatus::Cancelled);
  EXPECT_EQ(events.heard(), std::vector<std::string>{row}) << "cancellation";

  client("UPDATE events SET lifecycle = 'cancelled' WHERE id = " + row);
  EXPECT_TRUE(events.heard().empty()) << "the planner's own change";

  const auto manual = std::to_string(addManualEvent());
  EXPECT_EQ(events.heard(), std::vector<std::string>{manual});
  client("UPDATE events SET opt = 'out', opt_source = 'operator' WHERE id = " +
         manual);
  EXPECT_EQ(events.heard(), std::vector<std::string>{manual});
  client("DELETE FROM events WHERE id = " + manual);
  EXPECT_EQ(events.heard(), std::vector<std::string>{manual});
}

TEST_F(PgStoreTest, ClientsOwnManualEventsAndOnlyTheLifecycleOfProgramEvents) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  const auto program = std::to_string(
      store->upsertProgramEvent(event("e1"), Opt::In, OptSource::Default).id);

  // Program events: lifecycle only.
  client("UPDATE events SET lifecycle = 'planned' WHERE id = " + program);
  EXPECT_EQ(value("SELECT lifecycle FROM events WHERE id = " + program),
            "planned");
  for (const auto *change :
       {"opt = 'out'", "start_at = now()", "signals = '[]'",
        "program_status = 'cancelled'", "source = 'manual'",
        "lifecycle = 'prep', duration_seconds = 60"}) {
    EXPECT_THROW(client(std::string("UPDATE events SET ") + change +
                        " WHERE id = " + program),
                 pqxx::insufficient_privilege)
        << change;
  }
  EXPECT_THROW(client("DELETE FROM events WHERE id = " + program),
               pqxx::insufficient_privilege);
  EXPECT_THROW(
      client("INSERT INTO events (site_code, site_id, source, connection_id, "
             "program_event_id, start_at, duration_seconds) VALUES ('ML', " +
             siteId("ML") + ", 'program', " +
             std::to_string(id) + ", 'forged', now(), 60)"),
      pqxx::insufficient_privilege);

  // Manual events: everything.
  const auto manual = std::to_string(addManualEvent());
  auto row = sql("SELECT created_by, test_event, opt, updated_at FROM events "
                 "WHERE id = " +
                 manual);
  EXPECT_EQ(row[0][0].as<std::string>(), "ops");
  const auto insertedAt = row[0][3].as<std::string>();

  client("UPDATE events SET test_event = true, duration_seconds = 900, "
         "start_at = now() + interval '2 hours', opt = 'out', "
         "opt_source = 'operator', lifecycle = 'opted_out', "
         "signals = '[{\"name\": \"SIMPLE\", \"type\": \"level\", "
         "\"id\": \"s1\", \"intervals\": [{\"offset_seconds\": 0, "
         "\"duration_seconds\": 900, \"value\": 2}]}]' WHERE id = " +
         manual);
  row = sql("SELECT test_event, duration_seconds, opt, opt_source, updated_at "
            "FROM events WHERE id = " +
            manual);
  EXPECT_TRUE(row[0][0].as<bool>());
  EXPECT_EQ(row[0][1].as<int>(), 900);
  EXPECT_EQ(row[0][2].as<std::string>(), "out");
  EXPECT_EQ(row[0][3].as<std::string>(), "operator");
  EXPECT_NE(row[0][4].as<std::string>(), insertedAt)
      << "updated_at follows the change";
  EXPECT_THROW(client("UPDATE events SET source = 'program', connection_id = " +
                      std::to_string(id) +
                      ", program_event_id = 'forged' WHERE id = " + manual),
               pqxx::insufficient_privilege);

  // Deleting a manual event keeps what was logged about it.
  client("INSERT INTO activity (site_code, site_id, event_id, category, actor, "
         "message) VALUES ('ML', " +
         siteId("ML") + ", " + manual +
         ", 'events', 'ops', 'Manual event created')");
  client("DELETE FROM events WHERE id = " + manual);
  EXPECT_EQ(value("SELECT count(*) FROM events"), "1");
  EXPECT_EQ(value("SELECT event_id IS NULL FROM activity "
                  "WHERE message = 'Manual event created'"),
            "t");

  // The worker is not held to any of this.
  store->setOpt(std::stoll(program), Opt::Out, OptSource::Operator);
  EXPECT_EQ(value("SELECT opt FROM events WHERE id = " + program), "out");
}

TEST_F(PgStoreTest, ClientsAddToTheActivityLogButCannotRewriteIt) {
  addConnection();
  client("INSERT INTO activity (site_code, site_id, category, actor, message) "
         "VALUES ('ML', '6f1c1f0e-6a59-4f0b-9f57-0d7b0a6f3b11', "
         "'instructions', 'planner', 'Queued 3 instructions for PREP')");
  client("INSERT INTO activity (site_code, site_id, category, actor, message) "
         "VALUES ('ML', " +
         siteId("ML") + ", 'configuration', 'ops', 'Changed the VTN URL')");
  EXPECT_EQ(client("SELECT count(*) FROM activity")[0][0].as<int>(), 2);

  EXPECT_THROW(client("UPDATE activity SET message = 'rewritten'"),
               pqxx::insufficient_privilege);
  EXPECT_THROW(client("DELETE FROM activity"), pqxx::insufficient_privilege);
}

TEST_F(PgStoreTest, SiteIdIsCopiedToEventsAndActivity) {
  const auto id = addConnection();
  const std::string site = "6f1c1f0e-6a59-4f0b-9f57-0d7b0a6f3b11";
  sql("UPDATE connections SET site_id = '" + site + "'");
  auto store = runnerStore(id);

  store->upsertProgramEvent(event("e1"), Opt::In, OptSource::Default);
  store->appendActivity({.category = ActivityCategory::Events,
                         .message = "Event received"});
  EXPECT_EQ(value("SELECT site_id FROM events"), site);
  EXPECT_EQ(value("SELECT site_id FROM activity"), site);

  // History keeps the code it was written under; the id finds it.
  sql("UPDATE connections SET site_code = 'ML2'");
  store->upsertProgramEvent(event("e2"), Opt::In, OptSource::Default);
  const auto events = sql("SELECT site_code FROM events WHERE site_id = '" +
                          site + "' ORDER BY id");
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0][0].as<std::string>(), "ML");
  EXPECT_EQ(events[1][0].as<std::string>(), "ML2");
}

TEST_F(PgStoreTest, AcceptedSignalsAreOpenadrConfiguration) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  const auto version = store->loadConfig().configVersion;

  sql("UPDATE openadr2b_connections SET accepted_signals = "
      "ARRAY['SIMPLE', 'LOAD_DISPATCH']");
  EXPECT_EQ(store->loadOpenadr2bSettings().acceptedSignals,
            (std::vector<std::string>{"SIMPLE", "LOAD_DISPATCH"}));
  EXPECT_EQ(store->loadConfig().configVersion, version + 1);

  EXPECT_THROW(sql("UPDATE openadr2b_connections "
                   "SET accepted_signals = ARRAY['BID_LOAD']"),
               pqxx::check_violation);
}

TEST_F(PgStoreTest, PollJitterIsConfiguration) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  const auto before = store->loadConfig();
  EXPECT_DOUBLE_EQ(before.pollJitter, 0.1);

  sql("UPDATE connections SET poll_jitter = 0.25");
  const auto config = store->loadConfig();
  EXPECT_DOUBLE_EQ(config.pollJitter, 0.25);
  EXPECT_EQ(config.configVersion, before.configVersion + 1);

  EXPECT_THROW(sql("UPDATE connections SET poll_jitter = 1.5"),
               pqxx::check_violation);
  EXPECT_THROW(sql("UPDATE connections SET poll_jitter = -0.1"),
               pqxx::check_violation);
}

TEST_F(PgStoreTest, ArchivedConnectionsAreListedAsSuch) {
  const auto live = addConnection("ML");
  const auto archived = addConnection("OT");
  runnerStore(archived)->loadState();
  sql("UPDATE connection_state SET registration = 'registered', "
      "registration_id = 'reg-1'");
  const auto version = runnerStore(archived)->loadConfig().configVersion;
  sql("UPDATE connections SET archived_at = now() WHERE id = " +
      std::to_string(archived));
  sql("INSERT INTO connection_commands (connection_id, kind) VALUES (" +
      std::to_string(live) + ", 'test'), (" + std::to_string(archived) +
      ", 'test')");

  const auto config = runnerStore(archived)->loadConfig();
  EXPECT_TRUE(config.archived);
  EXPECT_EQ(config.configVersion, version + 1);

  PgSupervisorStore store(connectionString);
  const auto connections = store.listConnections();
  ASSERT_EQ(connections.size(), 2u);
  EXPECT_FALSE(connections[0].archived);
  EXPECT_FALSE(connections[0].registered);
  EXPECT_TRUE(connections[1].archived);
  EXPECT_TRUE(connections[1].registered);

  EXPECT_EQ(store.failCommandsForDisabledConnections(), 1);
  EXPECT_EQ(value("SELECT message FROM connection_commands "
                  "WHERE status = 'failed'"),
            "The connection is archived.");
}

TEST_F(PgStoreTest, ConnectionsWithHistoryAreArchivedNotDeleted) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  store->upsertProgramEvent(event("e1"), Opt::In, OptSource::Default);
  store.reset();

  EXPECT_THROW(sql("DELETE FROM connections"), pqxx::foreign_key_violation);
  sql("UPDATE connections SET archived_at = now()");
  EXPECT_EQ(value("SELECT count(*) FROM events"), "1");
}

TEST_F(PgStoreTest, RegisteredConnectionKeepsItsProgram) {
  const auto id = addConnection();
  auto store = runnerStore(id);
  store->loadState();
  store->setRegistered({"ven-1", "reg-1", 10});

  try {
    sql("UPDATE openadr2b_connections SET vtn_url = 'https://other.example.com'");
    FAIL() << "expected the change to be refused";
  } catch (const pqxx::sql_error &error) {
    EXPECT_EQ(error.sqlstate(), "55000");
    EXPECT_NE(std::string(error.what()).find("unregister it before"),
              std::string::npos);
  }
  // Other settings, and the same program written another way, are fine.
  sql("UPDATE openadr2b_connections SET market_context = 'mc', "
      "vtn_url = 'https://vtn.example.com/'");

  store->clearRegistration();
  sql("UPDATE openadr2b_connections SET vtn_url = 'https://other.example.com'");
  EXPECT_EQ(store->loadOpenadr2bSettings().vtnUrl, "https://other.example.com");
}

TEST_F(PgStoreTest, LoadsCsipausSettings) {
  const auto id = addCsipausConnection();
  auto store = runnerStore(id);

  EXPECT_EQ(store->loadConfig().protocol, "csipaus");
  auto settings = store->loadCsipausSettings();
  EXPECT_EQ(settings.dcapUrl, "https://utility.example.com/dcap");
  EXPECT_FALSE(settings.caCertPem);
  EXPECT_TRUE(settings.verifyServerCertificate);
  EXPECT_FALSE(settings.connectionPointId);
  EXPECT_FALSE(settings.registrationPin);

  const auto version = store->loadConfig().configVersion;
  client("UPDATE csipaus_connections SET ca_cert_pem = 'CA', "
         "verify_server_certificate = false, "
         "connection_point_id = '4102345678', registration_pin = 111115");
  EXPECT_GT(store->loadConfig().configVersion, version)
      << "changing settings bumps the version";
  settings = store->loadCsipausSettings();
  EXPECT_EQ(settings.caCertPem, "CA");
  EXPECT_FALSE(settings.verifyServerCertificate);
  EXPECT_EQ(settings.connectionPointId, "4102345678");
  EXPECT_EQ(settings.registrationPin, 111115u);
}

TEST_F(PgStoreTest, CsipausEventsAreStoredWithTheirDetail) {
  const auto id = addCsipausConnection();
  auto store = runnerStore(id);

  auto control = event("C1");
  control.declinable = false;
  control.signals = {{"export", "limit", "export", {{0, 3600, 1500}}},
                     {"connect", "limit", "connect", {{0, 3600, 0}}}};
  const auto stored =
      store->upsertProgramEvent(control, Opt::In, OptSource::Default);
  EXPECT_FALSE(stored.declinable);
  EXPECT_EQ(stored.signals, control.signals);
  EXPECT_EQ(value("SELECT declinable FROM event_state"), "f");

  csipaus::CsipausEventDetail detail;
  detail.href = "/derp/1/derc/1";
  detail.mrid = "C1";
  detail.programMrid = "B1";
  detail.programDescription = "Site limits";
  detail.primacy = 2;
  detail.creationTime = Timestamp(std::chrono::seconds(1789990000));
  detail.randomizeStartSeconds = -60;
  detail.replyTo = "/rsps/1/rsp";
  detail.responseRequired = 3;
  store->recordEventDetail(stored.id, detail);
  detail.primacy = 1;
  detail.effectiveStartAt = Timestamp(std::chrono::seconds(1789999970));
  detail.effectiveEndAt = Timestamp(std::chrono::seconds(1790003570));
  store->recordEventDetail(stored.id, detail);

  const auto row = client(
      "SELECT kind, primacy, extract(epoch FROM creation_time)::bigint, "
      "randomize_start_seconds, randomize_duration_seconds IS NULL, "
      "response_required, extract(epoch FROM effective_start_at)::bigint, "
      "extract(epoch FROM effective_end_at)::bigint "
      "FROM csipaus_events WHERE event_id = " +
      std::to_string(stored.id));
  ASSERT_EQ(row.size(), 1u);
  EXPECT_EQ(row[0][0].as<std::string>(), "control");
  EXPECT_EQ(row[0][1].as<int>(), 1);
  EXPECT_EQ(row[0][2].as<long long>(), 1789990000);
  EXPECT_EQ(row[0][3].as<int>(), -60);
  EXPECT_TRUE(row[0][4].as<bool>());
  EXPECT_EQ(row[0][5].as<int>(), 3);
  EXPECT_EQ(row[0][6].as<long long>(), 1789999970);
  EXPECT_EQ(row[0][7].as<long long>(), 1790003570);
  EXPECT_THROW(client("DELETE FROM csipaus_events"), pqxx::sql_error);
  EXPECT_THROW(sql("UPDATE csipaus_events SET kind = 'default'"),
               pqxx::check_violation)
      << "a default has no window";
  EXPECT_THROW(sql("UPDATE csipaus_events SET effective_end_at = NULL"),
               pqxx::check_violation)
      << "a window has both bounds";

  // Another connection's store cannot write it.
  auto other = runnerStore(addCsipausConnection("OT"));
  detail.primacy = 9;
  other->recordEventDetail(stored.id, detail);
  EXPECT_EQ(value("SELECT primacy FROM csipaus_events"), "1");

  store->recordResponse(stored.id, EventAnswer::Received, 0, true, "");
  EXPECT_EQ(value("SELECT response FROM event_responses"), "received");
  EXPECT_EQ(store->findProgramEvent("C1")->respondedModification, 0);

  store->setProgramStatus(stored.id, ProgramStatus::Superseded);
  EXPECT_TRUE(store->openProgramEvents().empty());
}

TEST_F(PgStoreTest, CsipausClockAndCapsInForceAreRecorded) {
  const auto id = addCsipausConnection();
  auto store = runnerStore(id);
  store->loadState();
  store->recordClockOffset(-12, Timestamp(std::chrono::seconds(1790000000)));
  EXPECT_EQ(client("SELECT clock_offset_seconds, "
                   "extract(epoch FROM clock_checked_at)::bigint "
                   "FROM connection_state")[0][0]
                .as<int>(),
            -12);

  auto control = event("C1");
  control.declinable = false;
  store->upsertProgramEvent(control, Opt::In, OptSource::Default);
  const Timestamp start(std::chrono::seconds(1790000000));
  store->replaceLimits(
      {{"export", start, start + std::chrono::hours(1), 1500,
        csipaus::CsipausEventKind::Control, "C1"},
       {"export", start + std::chrono::hours(1), std::nullopt, 5000,
        csipaus::CsipausEventKind::Default, "default:P1"}});
  store->replaceLimits(
      {{"export", start, start + std::chrono::hours(1), 1500,
        csipaus::CsipausEventKind::Control, "C1"},
       {"export", start + std::chrono::hours(1), std::nullopt, 4000,
        csipaus::CsipausEventKind::Default, "default:P1"}});

  const auto rows = client(
      "SELECT limit_name, value, source, event_id IS NOT NULL, end_at IS NULL "
      "FROM csipaus_limits ORDER BY start_at");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0][0].as<std::string>(), "export");
  EXPECT_EQ(rows[0][2].as<std::string>(), "control");
  EXPECT_TRUE(rows[0][3].as<bool>());
  EXPECT_EQ(rows[1][1].as<double>(), 4000);
  EXPECT_FALSE(rows[1][3].as<bool>()) << "no event is named default:P1";
  EXPECT_TRUE(rows[1][4].as<bool>());
  EXPECT_THROW(client("DELETE FROM csipaus_limits"), pqxx::sql_error);
}

TEST_F(PgStoreTest, CsipausLimitChangesAreSignalled) {
  const auto id = addCsipausConnection();
  const auto otherId = addCsipausConnection("OT");
  pqxx::connection listening(connectionString);
  ChannelListener limits(listening, "grid_services_limits");
  const auto connection = std::vector<std::string>{std::to_string(id)};

  auto store = runnerStore(id);
  const Timestamp start(std::chrono::seconds(1790000000));
  const std::vector<csipaus::LimitSegment> caps = {
      {"export", start, start + std::chrono::hours(1), 1500,
       csipaus::CsipausEventKind::Default, "default:P1"},
      {"import", start, std::nullopt, 5000, csipaus::CsipausEventKind::Default,
       "default:P1"}};
  store->replaceLimits(caps);
  EXPECT_EQ(limits.heard(), connection);
  store->replaceLimits(caps);
  EXPECT_EQ(limits.heard(), connection)
      << "rows deleted and inserted in one transaction, signalled once";

  runnerStore(otherId)->replaceLimits(caps);
  EXPECT_EQ(limits.heard(), std::vector<std::string>{std::to_string(otherId)});

  store->replaceLimits({});
  EXPECT_EQ(limits.heard(), connection);
  store->replaceLimits({});
  EXPECT_TRUE(limits.heard().empty()) << "nothing changed";
}

TEST_F(PgStoreTest, CsipausLimitsCarryTheirRamps) {
  const auto id = addCsipausConnection();
  auto store = runnerStore(id);
  const Timestamp start(std::chrono::seconds(1790000000));
  const auto at = [&start](int minutes) {
    return start + std::chrono::minutes(minutes);
  };
  using csipaus::CsipausEventKind;
  store->replaceLimits(
      {{"export", at(0), at(10), 1500, CsipausEventKind::Control, "C1", 6000,
        std::nullopt},
       {"export", at(10), at(20), 1000, CsipausEventKind::Control, "C2", 1,
        std::nullopt},
       {"export", at(20), at(30), 500, CsipausEventKind::Control, "C3",
        std::nullopt, 100},
       {"export", at(30), std::nullopt, 0, CsipausEventKind::Default,
        "default:P1", std::nullopt, 100},
       {"import", at(0), std::nullopt, 0, CsipausEventKind::Default,
        "default:P1", std::nullopt, std::nullopt}});

  const auto rows = client(
      "SELECT ramp_seconds::text, set_grad_w FROM csipaus_limits "
      "ORDER BY limit_name, start_at");
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0][0].as<std::string>(), "60.00") << "rampTms is in 0.01 s";
  EXPECT_TRUE(rows[0][1].is_null());
  EXPECT_EQ(rows[1][0].as<std::string>(), "0.01");
  EXPECT_TRUE(rows[2][0].is_null());
  EXPECT_EQ(rows[2][1].as<int>(), 100);
  EXPECT_EQ(rows[3][1].as<int>(), 100);
  EXPECT_TRUE(rows[4][0].is_null());
  EXPECT_TRUE(rows[4][1].is_null()) << "no ramp";

  const auto insert = [&](const std::string &source, const std::string &ramp,
                          const std::string &gradW, int minutes) {
    sql("INSERT INTO csipaus_limits (connection_id, limit_name, start_at, "
        "value, source, ramp_seconds, set_grad_w) VALUES (" +
        std::to_string(id) + ", 'load', to_timestamp(" +
        std::to_string(1790000000 + minutes * 60) + "), 0, '" + source +
        "', " + ramp + ", " + gradW + ")");
  };
  EXPECT_THROW(insert("control", "60", "100", 0), pqxx::sql_error)
      << "ramp_seconds and set_grad_w together";
  EXPECT_THROW(insert("default", "60", "NULL", 1), pqxx::sql_error)
      << "ramp_seconds on a default's row";
  EXPECT_THROW(insert("control", "-1", "NULL", 2), pqxx::sql_error);
  EXPECT_THROW(insert("control", "655.36", "NULL", 3), pqxx::sql_error);
  EXPECT_THROW(insert("default", "NULL", "65536", 4), pqxx::sql_error);
  EXPECT_THROW(insert("default", "NULL", "-1", 5), pqxx::sql_error);
  EXPECT_NO_THROW(insert("control", "655.35", "NULL", 6));
  EXPECT_NO_THROW(insert("default", "NULL", "65535", 7));
  EXPECT_NO_THROW(insert("default", "NULL", "0", 8)) << "0 is no limit";

  const auto ramps = migrationsFrom("csipaus_limits_ramps").front();
  sql(migrationSection(ramps, "down"));
  EXPECT_EQ(value("SELECT count(*) FROM information_schema.columns WHERE "
                  "table_name = 'csipaus_limits' AND column_name IN "
                  "('ramp_seconds', 'set_grad_w')"),
            "0");
  sql(migrationSection(ramps, "up"));
  EXPECT_NO_THROW(store->replaceLimits({}));
}

TEST_F(PgStoreTest, ProgressIsDueOnceThePlannerActs) {
  const auto id = addCsipausConnection();
  auto store = runnerStore(id);
  auto control = event("C1");
  control.declinable = false;
  const auto stored =
      store->upsertProgramEvent(control, Opt::In, OptSource::Default);
  csipaus::CsipausEventDetail detail;
  detail.href = "/derp/1/derc/1";
  detail.mrid = "C1";
  detail.programMrid = "B1";
  detail.replyTo = "/rsps/1/rsp";
  detail.responseRequired = 3;
  store->recordEventDetail(stored.id, detail);
  EXPECT_TRUE(store->progressDue().empty());

  sql("UPDATE events SET lifecycle = 'control'");
  store->recordResponse(stored.id, EventAnswer::Received, 0, false, "refused");
  EXPECT_TRUE(store->progressDue().empty())
      << "the server is told it was received first";

  store->recordResponse(stored.id, EventAnswer::Received, 0, true, "");
  auto due = store->progressDue();
  ASSERT_EQ(due.size(), 1u);
  EXPECT_EQ(due[0].detail.replyTo, "/rsps/1/rsp");
  EXPECT_EQ(due[0].answers, std::vector<EventAnswer>{EventAnswer::Started});

  store->recordResponse(stored.id, EventAnswer::Started, 0, true, "");
  sql("UPDATE events SET lifecycle = 'completed'");
  due = store->progressDue();
  ASSERT_EQ(due.size(), 1u);
  EXPECT_EQ(due[0].answers, std::vector<EventAnswer>{EventAnswer::Completed});

  store->recordResponse(stored.id, EventAnswer::Completed, 0, true, "");
  EXPECT_TRUE(store->progressDue().empty());
}

TEST_F(PgStoreTest, MigrationKeepsOptAnswersAsAnswers) {
  const auto csipausEvents = migrationsFrom("csipaus_events").front();
  rollBackTo(*admin, "csipaus_events");
  const auto id = addConnection();
  sql("INSERT INTO events (site_code, site_id, source, connection_id, "
      "program_event_id, start_at, duration_seconds) VALUES ('ML', " +
      siteId("ML") + ", 'program', " + std::to_string(id) +
      ", 'e1', now(), 3600)");
  sql("INSERT INTO event_responses (event_id, opt, modification_number, "
      "status) SELECT id, 'out', 0, 'sent' FROM events");

  sql(migrationSection(csipausEvents, "up"));
  EXPECT_EQ(value("SELECT response FROM event_responses"), "opt_out");
  EXPECT_EQ(value("SELECT declinable FROM events"), "t");

  sql("INSERT INTO event_responses (event_id, response, modification_number, "
      "status) SELECT id, 'received', 0, 'sent' FROM events");
  sql(migrationSection(csipausEvents, "down"));
  EXPECT_EQ(value("SELECT string_agg(opt, ',') FROM event_responses"), "out");
  sql(migrationSection(csipausEvents, "up"));
}

TEST_F(PgStoreTest, CsipausDerAndReadingsAreLoaded) {
  const auto id = addCsipausConnection();
  auto store = runnerStore(id);
  EXPECT_FALSE(store->loadDer().has_value());
  EXPECT_TRUE(store->loadReadings().empty());

  const auto connection = std::to_string(id);
  sql("INSERT INTO telemetry_points (connection_id, point_id, kind, units, "
      "source) VALUES (" + connection + ", 'site-power', 'power_real', 'W', "
      "'{\"node_id\": 604, \"source_id\": \"/M/1\", \"property\": \"watts\"}'), "
      "(" + connection + ", 'site-volts', 'voltage', 'V', "
      "'{\"node_id\": 604, \"source_id\": \"/M/1\", \"property\": \"voltage\"}')");
  sql("INSERT INTO csipaus_der (connection_id, der_type, rtg_max_w, rtg_v_nom, "
      "set_max_w, modes_supported, modes_enabled) VALUES (" + connection +
      ", 4, 5000, 230, 4000, '{export,import,connect}', '{export}')");
  sql("INSERT INTO csipaus_readings (connection_id, mirror, reading, phase, "
      "point_id) VALUES (" + connection + ", 'site', 'real_power', 'none', "
      "'site-power'), (" + connection + ", 'site', 'voltage', 'a', 'site-volts')");

  const auto der = store->loadDer();
  ASSERT_TRUE(der.has_value());
  EXPECT_EQ(der->type, 4);
  EXPECT_DOUBLE_EQ(der->rtgMaxW, 5000);
  EXPECT_EQ(der->rtgVNom, 230);
  EXPECT_FALSE(der->rtgMaxVa.has_value());
  EXPECT_EQ(der->setMaxW, 4000);
  EXPECT_EQ(der->setGradW, 27);
  EXPECT_EQ(der->modesSupported,
            (std::vector<std::string>{"export", "import", "connect"}));
  EXPECT_EQ(der->modesEnabled, std::vector<std::string>{"export"});

  const auto readings = store->loadReadings();
  ASSERT_EQ(readings.size(), 2u);
  EXPECT_EQ(readings[1].reading, csipaus::ReadingKind::Voltage);
  EXPECT_EQ(readings[1].phase, csipaus::Phase::A);
  EXPECT_EQ(readings[1].pointId, "site-volts");

  // Renaming a point keeps its readings.
  sql("UPDATE telemetry_points SET point_id = 'site-voltage' "
      "WHERE point_id = 'site-volts'");
  EXPECT_EQ(store->loadReadings()[1].pointId, "site-voltage");

  EXPECT_THROW(sql("INSERT INTO csipaus_readings (connection_id, mirror, "
                   "reading, phase, point_id) VALUES (" + connection +
                   ", 'der', 'real_power', 'a', 'site-power')"),
               pqxx::check_violation)
      << "only voltage is per phase";
  EXPECT_THROW(sql("INSERT INTO csipaus_readings (connection_id, mirror, "
                   "reading, point_id) VALUES (" + connection +
                   ", 'der', 'real_power', 'nowhere')"),
               pqxx::foreign_key_violation);
  EXPECT_THROW(sql("UPDATE csipaus_der SET modes_enabled = '{load}'"),
               pqxx::check_violation)
      << "enabled modes are supported ones";
  EXPECT_THROW(sql("UPDATE csipaus_der SET set_max_w = 6000"),
               pqxx::check_violation);

  // A reading goes with its point.
  sql("DELETE FROM telemetry_points WHERE point_id = 'site-voltage'");
  EXPECT_EQ(store->loadReadings().size(), 1u);

  ReportDelivery delivery;
  delivery.report = "site_readings";
  delivery.ok = true;
  delivery.exchange = nlohmann::json::array({{{"direction", "to_program"}}});
  store->recordReportDelivery(delivery);
  const auto rows = sql("SELECT report, exchange::text, request_id IS NULL "
                        "FROM report_deliveries");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0][0].as<std::string>(), "site_readings");
  EXPECT_EQ(rows[0][1].as<std::string>(), "[{\"direction\": \"to_program\"}]");
  EXPECT_TRUE(rows[0][2].as<bool>());

  // And everything with its connection.
  sql("DELETE FROM connections WHERE id = " + connection);
  EXPECT_EQ(value("SELECT count(*) FROM csipaus_readings"), "0");
  EXPECT_EQ(value("SELECT count(*) FROM csipaus_der"), "0");
}

TEST_F(PgStoreTest, CsipausDerStateIsBoundToStatePoints) {
  const auto id = addCsipausConnection();
  const auto connection = std::to_string(id);
  const auto statePoint = [&](const std::string &pointId,
                              const std::string &units,
                              const std::string &scale) {
    return "INSERT INTO telemetry_points (connection_id, point_id, kind, "
           "units, scale, source) VALUES (" + connection + ", '" + pointId +
           "', 'state', '" + units + "', '" + scale +
           "', '{\"node_id\": 1, \"source_id\": \"/DER/INV\", "
           "\"property\": \"" + pointId + "\", \"reduce\": \"last\"}')";
  };
  EXPECT_THROW(client(statePoint("connected", "W", "none")),
               pqxx::check_violation)
      << "a state has no units";
  EXPECT_THROW(client(statePoint("connected", "none", "k")),
               pqxx::check_violation);
  client(statePoint("connected", "none", "none"));
  client(statePoint("operating", "none", "none"));

  auto store = runnerStore(id);
  const auto points = store->loadTelemetryPoints();
  ASSERT_EQ(points.size(), 2u);
  EXPECT_EQ(points[0].item.kind, "state");
  EXPECT_EQ(points[0].item.units, "none");
  EXPECT_EQ(points[0].source.reduce, datum::Reduce::Last);

  EXPECT_THROW(client("INSERT INTO csipaus_der (connection_id, der_type, "
                      "rtg_max_w, connected_point_id) VALUES (" +
                      connection + ", 4, 5000, 'nowhere')"),
               pqxx::foreign_key_violation);
  client("INSERT INTO csipaus_der (connection_id, der_type, rtg_max_w, "
         "connected_point_id, operating_point_id) VALUES (" + connection +
         ", 4, 5000, 'connected', 'operating')");
  auto der = store->loadDer();
  ASSERT_TRUE(der.has_value());
  EXPECT_EQ(der->connectedPointId, "connected");
  EXPECT_EQ(der->operatingPointId, "operating");

  // Renaming a point keeps its binding.
  client("UPDATE telemetry_points SET point_id = 'der-operating' "
         "WHERE point_id = 'operating'");
  EXPECT_EQ(store->loadDer()->operatingPointId, "der-operating");

  // Deleting one unbinds it, and keeps the DER.
  client("DELETE FROM telemetry_points WHERE point_id = 'connected'");
  der = store->loadDer();
  ASSERT_TRUE(der.has_value());
  EXPECT_FALSE(der->connectedPointId.has_value());
  EXPECT_EQ(der->operatingPointId, "der-operating");
}

// DERSettings' updatedTime is the DER's settings', which its state bindings
// are not.
TEST_F(PgStoreTest, CsipausDerUpdatedAtIgnoresStateBindings) {
  const auto id = addCsipausConnection();
  const auto connection = std::to_string(id);
  for (const auto *pointId : {"connected", "operating"}) {
    client("INSERT INTO telemetry_points (connection_id, point_id, kind, "
           "units, source) VALUES (" + connection + ", '" + pointId +
           "', 'state', 'none', '{\"node_id\": 1, \"source_id\": "
           "\"/DER/INV\", \"property\": \"" + pointId + "\"}')");
  }
  client("INSERT INTO csipaus_der (connection_id, der_type, rtg_max_w, "
         "connected_point_id, operating_point_id) VALUES (" + connection +
         ", 4, 5000, 'connected', 'operating')");
  client("UPDATE csipaus_der SET updated_at = '2000-01-01 00:00:00+00'");
  const auto updatedAt = [&] {
    return value("SELECT extract(epoch FROM updated_at)::bigint "
                 "FROM csipaus_der");
  };
  ASSERT_EQ(updatedAt(), "946684800");

  client("UPDATE telemetry_points SET point_id = 'der-operating' "
         "WHERE point_id = 'operating'");
  EXPECT_EQ(value("SELECT operating_point_id FROM csipaus_der"),
            "der-operating");
  EXPECT_EQ(updatedAt(), "946684800") << "renaming a bound point";
  client("DELETE FROM telemetry_points WHERE point_id = 'connected'");
  EXPECT_EQ(value("SELECT count(*) FROM csipaus_der "
                  "WHERE connected_point_id IS NULL"),
            "1");
  EXPECT_EQ(updatedAt(), "946684800") << "deleting a bound point";
  EXPECT_EQ(runnerStore(id)->loadDer()->updatedAt,
            Timestamp(std::chrono::seconds(946684800)));

  client("UPDATE csipaus_der SET set_grad_w = 100");
  EXPECT_EQ(value("SELECT updated_at > now() - interval '1 minute' "
                  "FROM csipaus_der"),
            "t")
      << "a setting changed";
}

// Power readings' power of ten is ecosuite's, 1 when it gives none;
// voltage's and frequency's are fixed.
TEST_F(PgStoreTest, CsipausPowerReadingsHaveAPowerOfTen) {
  const auto id = addCsipausConnection();
  const auto connection = std::to_string(id);
  const auto source = std::string("'{\"node_id\": 604, \"source_id\": "
                                  "\"/M/1\", \"property\": \"watts\"}'");
  client("INSERT INTO telemetry_points (connection_id, point_id, kind, units, "
         "source) VALUES (" + connection + ", 'site-power', 'power_real', "
         "'W', " + source + "), (" + connection + ", 'site-vars', "
         "'power_reactive', 'var', " + source + "), (" + connection +
         ", 'site-volts', 'voltage', 'V', " + source + ")");
  client("INSERT INTO csipaus_readings (connection_id, mirror, reading, "
         "point_id) VALUES (" + connection + ", 'site', 'real_power', "
         "'site-power'), (" + connection + ", 'site', 'voltage', "
         "'site-volts')");
  client("INSERT INTO csipaus_readings (connection_id, mirror, reading, "
         "point_id, power_of_ten) VALUES (" + connection +
         ", 'site', 'reactive_power', 'site-vars', 3)");
  const auto powerOfTen = [&](const std::string &reading) {
    const auto rows = sql("SELECT power_of_ten FROM csipaus_readings "
                          "WHERE reading = '" + reading + "'");
    return rows[0][0].is_null() ? std::nullopt
                                : std::optional<int>(rows[0][0].as<int>());
  };
  EXPECT_EQ(powerOfTen("real_power"), 1) << "ecosuite gave none";
  EXPECT_EQ(powerOfTen("reactive_power"), 3);
  EXPECT_FALSE(powerOfTen("voltage").has_value());

  const auto readings = runnerStore(id)->loadReadings();
  ASSERT_EQ(readings.size(), 3u);
  EXPECT_EQ(readings[0].reading, csipaus::ReadingKind::ReactivePower);
  EXPECT_EQ(readings[0].powerOfTen, 3);
  EXPECT_EQ(readings[1].powerOfTen, 1);
  EXPECT_FALSE(readings[2].powerOfTen.has_value());

  EXPECT_THROW(client("UPDATE csipaus_readings SET power_of_ten = -1 "
                      "WHERE reading = 'voltage'"),
               pqxx::check_violation)
      << "voltage's is fixed";
  EXPECT_THROW(client("UPDATE csipaus_readings SET power_of_ten = 10 "
                      "WHERE reading = 'real_power'"),
               pqxx::check_violation);
  EXPECT_THROW(client("UPDATE csipaus_readings SET power_of_ten = -10 "
                      "WHERE reading = 'real_power'"),
               pqxx::check_violation);
  client("UPDATE csipaus_readings SET power_of_ten = NULL "
         "WHERE reading = 'reactive_power'");
  EXPECT_EQ(powerOfTen("reactive_power"), 1);
}

TEST_F(PgStoreTest, ChecksCsipausSettings) {
  addCsipausConnection();
  EXPECT_THROW(sql("UPDATE csipaus_connections SET registration_pin = 1000000"),
               pqxx::check_violation);
  EXPECT_THROW(sql("UPDATE csipaus_connections SET connection_point_id = ''"),
               pqxx::check_violation);
  EXPECT_THROW(sql("UPDATE csipaus_connections SET dcap_url = ''"),
               pqxx::check_violation);
  EXPECT_THROW(sql("INSERT INTO connections (site_code, site_id, protocol) "
                   "VALUES ('XX', " + siteId("XX") + ", 'ieee2030_5')"),
               pqxx::check_violation);
}

TEST_F(PgStoreTest, RegisteredCsipausConnectionKeepsItsServer) {
  const auto id = addCsipausConnection();
  auto store = runnerStore(id);
  store->loadState();
  store->setRegistered({"C995EBDDFCFA53DE23DA8EA2BFFF91834D51D617", "/edev/3",
                        std::nullopt});

  try {
    sql("UPDATE csipaus_connections SET dcap_url = 'https://other.example.com/dcap'");
    FAIL() << "expected the change to be refused";
  } catch (const pqxx::sql_error &error) {
    EXPECT_EQ(error.sqlstate(), "55000");
  }
  sql("UPDATE csipaus_connections SET connection_point_id = '4102345678'");

  store->clearRegistration();
  sql("UPDATE csipaus_connections SET dcap_url = 'https://other.example.com/dcap'");
  EXPECT_EQ(store->loadCsipausSettings().dcapUrl,
            "https://other.example.com/dcap");
}

// Activity written before the exchange took its current shape is rewritten
// by the migration, and restored by rolling it back.
TEST_F(PgStoreTest, MigrationRewritesStoredExchanges) {
  const auto integration = migrationsFrom("ecosuite_integration").front();
  rollBackTo(*admin, "ecosuite_integration");
  sql("INSERT INTO activity (site_code, category, message, detail, created_at) "
      "VALUES ('ML', 'connection', 'Registered', "
      "'{\"exchange\": [{\"direction\": \"to_program\", \"xml\": \"<a/>\"}, "
      "{\"direction\": \"from_program\", \"xml\": \"<b/>\"}]}', "
      "'2026-09-29 10:00:00.25+00'), "
      "('ML', 'events', 'Event received', NULL, now())");

  sql(migrationSection(integration, "up"));
  const auto detail = nlohmann::json::parse(
      value("SELECT detail FROM activity WHERE message = 'Registered'"));
  EXPECT_EQ(detail,
            (nlohmann::json{
                {"exchange",
                 {{{"direction", "to_program"},
                   {"at", "2026-09-29T10:00:00.250Z"},
                   {"format", "xml"},
                   {"body", "<a/>"}},
                  {{"direction", "from_program"},
                   {"at", "2026-09-29T10:00:00.250Z"},
                   {"format", "xml"},
                   {"body", "<b/>"}}}}}));

  sql(migrationSection(integration, "down"));
  const auto restored = nlohmann::json::parse(
      value("SELECT detail FROM activity WHERE message = 'Registered'"));
  EXPECT_EQ(restored["exchange"][1],
            (nlohmann::json{{"direction", "from_program"}, {"xml", "<b/>"}}));
  for (const auto &file : migrationsFrom("ecosuite_integration")) {
    sql(migrationSection(file, "up"));
  }
}

TEST_F(PgStoreTest, SiteIdIsRequired) {
  EXPECT_THROW(sql("INSERT INTO connections (site_code, protocol) "
                   "VALUES ('ML', 'openadr2b')"),
               pqxx::not_null_violation);
  EXPECT_THROW(client("INSERT INTO events (site_code, source, start_at, "
                      "duration_seconds) VALUES ('ML', 'manual', now(), 60)"),
               pqxx::not_null_violation);
  EXPECT_THROW(client("INSERT INTO activity (site_code, category, message) "
                      "VALUES ('ML', 'configuration', 'Changed')"),
               pqxx::not_null_violation);
}

// Codes repeat across projects, so two sites may share one; a site has one
// connection until that connection is archived.
TEST_F(PgStoreTest, ASiteHasOneConnection) {
  const auto first = addConnection("ML");
  const auto insert = [&](const std::string &site) {
    sql("INSERT INTO connections (site_code, site_id, protocol) VALUES ('ML', " +
        siteId(site) + ", 'openadr2b')");
  };

  EXPECT_NO_THROW(insert("the same code in another project"));
  EXPECT_THROW(insert("ML"), pqxx::unique_violation);
  sql("UPDATE connections SET enabled = false WHERE id = " +
      std::to_string(first));
  EXPECT_THROW(insert("ML"), pqxx::unique_violation)
      << "a disabled connection is still the site's";

  sql("UPDATE connections SET archived_at = now() WHERE id = " +
      std::to_string(first));
  EXPECT_NO_THROW(insert("ML"));
  EXPECT_EQ(value("SELECT count(*) FROM connections WHERE site_id = " +
                  siteId("ML")),
            "2");
}

// Rows written before site_id existed are kept, under generated ids that
// hold each connection's history together.
TEST_F(PgStoreTest, RowsFromBeforeSiteIdAreGivenOne) {
  rollBackTo(*admin, "require_site_id");
  const auto legacy = [&](const std::string &site) {
    const auto id =
        sql("INSERT INTO connections (site_code, protocol) VALUES ('" + site +
            "', 'openadr2b') RETURNING id")[0][0]
            .as<ConnectionId>();
    sql("INSERT INTO events (site_code, source, connection_id, "
        "program_event_id, start_at, duration_seconds) VALUES ('" +
        site + "', 'program', " + std::to_string(id) + ", 'e1', now(), 60)");
    sql("INSERT INTO activity (site_code, connection_id, category, message) "
        "VALUES ('" +
        site + "', " + std::to_string(id) + ", 'connection', 'Registered')");
    return std::to_string(id);
  };
  const auto first = legacy("ML");
  const auto second = legacy("ML");
  sql("INSERT INTO activity (site_code, category, message) "
      "VALUES ('ML', 'configuration', 'Changed'), "
      "('ZZ', 'events', 'Manual event created')");
  sql("INSERT INTO events (site_code, source, start_at, duration_seconds) "
      "VALUES ('ZZ', 'manual', now(), 60)");
  sql("INSERT INTO connections (site_code, site_id, protocol) VALUES ('KP', " +
      siteId("KP") + ", 'openadr2b')");

  sql(migrationSection(migrationsFrom("require_site_id").front(), "up"));

  const auto siteOf = [&](const std::string &table,
                          const std::string &condition) {
    return value("SELECT DISTINCT site_id FROM " + table + " WHERE " +
                 condition);
  };
  const auto firstSite = siteOf("connections", "id = " + first);
  const auto secondSite = siteOf("connections", "id = " + second);
  EXPECT_NE(firstSite, secondSite);
  EXPECT_EQ(siteOf("events", "connection_id = " + first), firstSite);
  EXPECT_EQ(siteOf("activity", "connection_id = " + second), secondSite);
  EXPECT_EQ(siteOf("activity", "message = 'Changed'"), firstSite)
      << "takes the oldest connection with its code";

  const auto orphans = siteOf("events", "site_code = 'ZZ'");
  EXPECT_EQ(siteOf("activity", "site_code = 'ZZ'"), orphans);
  EXPECT_NE(orphans, firstSite);
  EXPECT_EQ(siteOf("connections", "site_code = 'KP'"),
            value("SELECT " + siteId("KP")))
      << "an id ecosuite set is left alone";
}

// The whole downward and upward path through the database: a command row
// inserted the way the API would, picked up by the supervisor and runner,
// with the result and state written back.
TEST_F(PgStoreTest, WorkerExecutesCommandsFromTheDatabase) {
  const auto id = addConnection();

  auto script = std::make_shared<FakeAdapterScript>();
  AdapterRegistry adapters;
  adapters.add("openadr2b", fakeAdapters(script));
  RunnerOptions runnerOptions;
  runnerOptions.defaultPollInterval = 50ms;
  runnerOptions.heartbeatInterval = 50ms;
  SupervisorOptions options;
  options.notificationWait = 20ms;

  PgSupervisorStore supervisorStore(connectionString);
  const auto url = connectionString;
  Supervisor supervisor(
      supervisorStore,
      [url](ConnectionId connection) {
        return std::make_unique<PgRunnerStore>(url, connection);
      },
      adapters, runnerOptions, options);

  std::stop_source stop;
  std::thread loop([&] { supervisor.run(stop.get_token()); });

  sql("INSERT INTO connection_commands (connection_id, kind, requested_by) "
      "VALUES (" +
      std::to_string(id) + ", 'register', 'ops')");

  bool registered = false;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!registered && std::chrono::steady_clock::now() < deadline) {
    const auto rows = sql(
        "SELECT s.registration, c.status FROM connection_state s "
        "JOIN connection_commands c USING (connection_id) "
        "WHERE s.last_poll_at IS NOT NULL");
    registered = !rows.empty() &&
                 rows[0][0].as<std::string>() == "registered" &&
                 rows[0][1].as<std::string>() == "succeeded";
    std::this_thread::sleep_for(20ms);
  }

  stop.request_stop();
  loop.join();

  EXPECT_TRUE(registered);
  const auto activity = sql(
      "SELECT message, actor FROM activity WHERE message LIKE 'Registered%'");
  ASSERT_EQ(activity.size(), 1u);
  EXPECT_EQ(activity[0][1].as<std::string>(), "ops");
  EXPECT_EQ(value("SELECT result ->> 'client_id' FROM connection_commands"),
            "ven-1");
}

// ---------------------------------------------------------------------------
// Reports

namespace {

// Two telemetry points on one stream, as rows for telemetry_points.
std::string usagePoints(ConnectionId id) {
  const auto connection = std::to_string(id);
  return "INSERT INTO telemetry_points (connection_id, point_id, kind, units, "
         "source) VALUES (" +
         connection +
         ", 'site-power', 'power_real', 'W', '{\"node_id\": 604, "
         "\"source_id\": \"/ML/1\", \"property\": \"watts\", "
         "\"property_class\": \"instantaneous\", \"multiplier\": -1}'), (" +
         connection +
         ", 'site-energy', 'energy_real', 'Wh', '{\"node_id\": 604, "
         "\"source_id\": \"/ML/1\", \"property\": \"wattHours\", "
         "\"property_class\": \"accumulating\", "
         "\"stale_after_seconds\": 900}')";
}

// A TELEMETRY_USAGE offer carrying both points.
const char *usageDataPoints = R"([
  {"rid": "power", "point_id": "site-power", "resource_id": "ML",
   "report_type": "usage", "reading_type": "Direct Read"},
  {"rid": "energy", "point_id": "site-energy", "resource_id": "ML"}
])";

std::string usageOffer(ConnectionId id) {
  return "INSERT INTO openadr2b_report_offers (connection_id, report_name, "
         "specifier_id, min_sampling_seconds, max_sampling_seconds, "
         "history_seconds, data_points) VALUES (" +
         std::to_string(id) +
         ", 'TELEMETRY_USAGE', 'usage-1', 60, 3600, 86400, '" +
         usageDataPoints + "'::jsonb)";
}

} // namespace

TEST_F(PgStoreTest, TelemetryOffersAndCredentialsAreReadFromTheDatabase) {
  const auto id = addConnection();
  const auto before = std::stoi(value("SELECT config_version FROM connections"));
  client(usagePoints(id));
  client(usageOffer(id));
  client("INSERT INTO connection_data_credentials (connection_id, host, token, "
         "secret_sealed) VALUES (" +
         std::to_string(id) + ", 'data.solarnetwork.net', 'tok', 'sealed')");

  auto store = runnerStore(id);
  const auto points = store->loadTelemetryPoints();
  ASSERT_EQ(points.size(), 2u);
  EXPECT_EQ(points[0].pointId, "site-power");
  EXPECT_EQ(points[0].item.scale, "none");
  EXPECT_DOUBLE_EQ(points[0].source.multiplier, -1);
  EXPECT_EQ(points[1].item.kind, "energy_real");
  EXPECT_EQ(points[1].source.propertyClass, datum::PropertyClass::Accumulating);
  EXPECT_EQ(points[1].source.reduce, datum::Reduce::Difference);
  EXPECT_EQ(points[1].source.staleAfter, 900s);

  const auto rows = store->loadReportOffers();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].reportName, "TELEMETRY_USAGE");
  EXPECT_EQ(rows[0].specifierId, "usage-1");
  EXPECT_EQ(rows[0].historySeconds, 86400);
  const auto offer = openadr2b::bindOffer(rows[0], points);
  ASSERT_EQ(offer.dataPoints.size(), 2u);
  EXPECT_EQ(offer.dataPoints[0].rid, "power");
  EXPECT_EQ(offer.dataPoints[0].pointId, "site-power");
  EXPECT_DOUBLE_EQ(offer.dataPoints[0].source.multiplier, -1);
  EXPECT_EQ(offer.dataPoints[1].item.units, "Wh");
  EXPECT_EQ(offer.dataPoints[1].readingType, "Direct Read");

  const auto credential = store->loadDataCredential();
  ASSERT_TRUE(credential.has_value());
  EXPECT_EQ(credential->host, "data.solarnetwork.net");
  EXPECT_EQ(credential->secretSealed, "sealed");

  // All of it is configuration: each row moved the connection's version.
  EXPECT_EQ(std::stoi(value("SELECT config_version FROM connections")),
            before + 4);
}

TEST_F(PgStoreTest, MalformedTelemetryAndOffersAreAConfigurationError) {
  const auto id = addConnection();
  const auto connection = std::to_string(id);
  sql("INSERT INTO telemetry_points (connection_id, point_id, source) "
      "VALUES (" + connection + ", 'power', '{\"node_id\": 604}')");
  EXPECT_THROW(runnerStore(id)->loadTelemetryPoints(), std::invalid_argument)
      << "a source needs a stream and a property";
  sql("DELETE FROM telemetry_points");

  sql("INSERT INTO openadr2b_report_offers (connection_id, report_name, "
      "specifier_id, min_sampling_seconds, max_sampling_seconds, data_points) "
      "VALUES (" + connection + ", 'TELEMETRY_USAGE', 'usage-1', 60, 3600, "
      "'[{\"point_id\": \"power\", \"resource_id\": \"ML\"}]'::jsonb)");
  auto store = runnerStore(id);
  EXPECT_THROW(openadr2b::bindOffer(store->loadReportOffers().at(0),
                                    store->loadTelemetryPoints()),
               std::invalid_argument)
      << "the offer names a point the connection does not have";

  EXPECT_THROW(sql("INSERT INTO telemetry_points (connection_id, point_id, "
                   "kind, source) VALUES (" + connection +
                   ", 'custom', 'custom', '{}')"),
               pqxx::check_violation)
      << "a custom point needs a description";
  EXPECT_THROW(sql("INSERT INTO telemetry_points (connection_id, point_id, "
                   "kind, units, source) VALUES (" + connection +
                   ", 'temp', 'temperature', 'K', '{}')"),
               pqxx::check_violation);
  EXPECT_THROW(sql("INSERT INTO openadr2b_report_offers (connection_id, "
                   "report_name, specifier_id, min_sampling_seconds, "
                   "max_sampling_seconds) VALUES (" + connection +
                   ", 'TELEMETRY_STATUS', 'status-2', 60, 3600)"),
               pqxx::check_violation)
      << "the status report is not an offer";
}

TEST_F(PgStoreTest, ClientCredentialsAreSharedConfiguration) {
  const auto first = addConnection("ML");
  const auto second = addConnection("ML2");
  EXPECT_FALSE(runnerStore(first)->loadClientCredential());

  const auto credential = client(
      "INSERT INTO client_credentials (name, cert_pem, key_sealed) "
      "VALUES ('aggregator', 'CERT', 'SEALED') RETURNING id")[0][0]
                              .as<long long>();
  client("UPDATE connections SET client_credential_id = " +
         std::to_string(credential));
  const auto loaded = runnerStore(second)->loadClientCredential();
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->certPem, "CERT");
  EXPECT_EQ(loaded->keySealed, "SEALED");

  // Renewing the certificate changes every connection that uses it.
  const auto versions = [&] {
    return value("SELECT string_agg(config_version::text, ',' ORDER BY id) "
                 "FROM connections");
  };
  const auto before = versions();
  client("UPDATE client_credentials SET cert_pem = 'RENEWED'");
  EXPECT_NE(versions(), before);
  EXPECT_EQ(runnerStore(first)->loadClientCredential()->certPem, "RENEWED");
  const auto after = versions();
  client("UPDATE client_credentials SET cert_pem = 'RENEWED'");
  EXPECT_EQ(versions(), after) << "an update that changes nothing is not one";

  EXPECT_THROW(client("DELETE FROM client_credentials"),
               pqxx::foreign_key_violation)
      << "a credential in use stays";
  EXPECT_THROW(client("INSERT INTO client_credentials (name, cert_pem, "
                      "key_sealed) VALUES ('half', 'CERT', '')"),
               pqxx::check_violation);
}

TEST_F(PgStoreTest, ReportRequestsAndDeliveriesAreRecorded) {
  const auto id = addConnection();
  sql(usagePoints(id));
  sql(usageOffer(id));
  const auto offerId =
      std::stoll(value("SELECT id FROM openadr2b_report_offers"));
  auto store = runnerStore(id);

  openadr2b::ReportRequest request;
  request.offerId = offerId;
  request.programRequestId = "rr-1";
  request.specifierId = "usage-1";
  request.reportName = "TELEMETRY_USAGE";
  request.kind = openadr2b::ReportRequestKind::Periodic;
  request.granularitySeconds = 60;
  request.reportBackSeconds = 300;
  request.startAt = Timestamp(std::chrono::seconds(1'800'000'000));
  request.durationSeconds = 0;
  request.rids = {"power", "energy"};
  store->recordReportRequest(request);

  auto rows = client("SELECT kind, status, offer_id, rids[2], "
                     "extract(epoch FROM start_at)::bigint "
                     "FROM openadr2b_report_requests");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0][0].as<std::string>(), "periodic");
  EXPECT_EQ(rows[0][1].as<std::string>(), "active");
  EXPECT_EQ(rows[0][2].as<long long>(), offerId);
  EXPECT_EQ(rows[0][3].as<std::string>(), "energy");
  EXPECT_EQ(rows[0][4].as<long long>(), 1'800'000'000);

  ReportDelivery delivery;
  delivery.programRequestId = "rr-1";
  delivery.intervalStart = Timestamp(std::chrono::seconds(1'800'000'000));
  delivery.intervalEnd = Timestamp(std::chrono::seconds(1'800'000'300));
  delivery.payload = nlohmann::json::array({{{"rid", "power"},
                                             {"point_id", "site-power"},
                                             {"value", 12.5},
                                             {"quality", "good"}}});
  delivery.reads = nlohmann::json::array(
      {{{"node_id", 604},
        {"source_id", "/S/1"},
        {"kind", "list"},
        {"path", "/solarquery/api/v1/sec/datum/list"},
        {"query", {{"nodeId", "604"}, {"sourceId", "/S/1"}}},
        {"datum_count", 20},
        {"error", nullptr}}});
  delivery.ok = true;
  delivery.responseCode = "200";
  store->recordReportDelivery(delivery);
  delivery.ok = false;
  delivery.responseCode = "450";
  delivery.message = "the VTN answered 450";
  store->recordReportDelivery(delivery);

  rows = client("SELECT d.status, d.response_code, d.message, "
                "d.payload -> 0 ->> 'point_id', r.report_request_id, "
                "d.reads -> 0 ->> 'path' "
                "FROM report_deliveries d JOIN openadr2b_report_requests r "
                "ON r.id = d.request_id ORDER BY d.id");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0][0].as<std::string>(), "sent");
  EXPECT_EQ(rows[0][1].as<std::string>(), "200");
  EXPECT_TRUE(rows[0][2].is_null());
  EXPECT_EQ(rows[0][3].as<std::string>(), "site-power");
  EXPECT_EQ(rows[0][4].as<std::string>(), "rr-1");
  EXPECT_EQ(rows[0][5].as<std::string>(), "/solarquery/api/v1/sec/datum/list");
  EXPECT_EQ(rows[1][0].as<std::string>(), "failed");
  EXPECT_EQ(rows[1][2].as<std::string>(), "the VTN answered 450");

  // A delivery for a request the program never made is not recorded; one
  // that answers no request at all, as other protocols' do, is.
  delivery.programRequestId = "unknown";
  store->recordReportDelivery(delivery);
  EXPECT_EQ(value("SELECT count(*) FROM report_deliveries"), "2");
  delivery.programRequestId.reset();
  store->recordReportDelivery(delivery);
  EXPECT_EQ(value("SELECT count(*) FROM report_deliveries "
                  "WHERE request_id IS NULL"),
            "1");
  sql("DELETE FROM report_deliveries WHERE request_id IS NULL");

  // The program reuses the id: the earlier request is over, the new one
  // active, and the unique index is content.
  request.kind = openadr2b::ReportRequestKind::OneShot;
  request.startAt.reset();
  request.durationSeconds.reset();
  store->recordReportRequest(request);
  rows = sql("SELECT status FROM openadr2b_report_requests ORDER BY id");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0][0].as<std::string>(), "completed");
  EXPECT_EQ(rows[1][0].as<std::string>(), "active");

  store->endReportRequest("rr-1", openadr2b::ReportRequestStatus::Cancelled);
  EXPECT_EQ(value("SELECT status FROM openadr2b_report_requests "
                  "ORDER BY id DESC LIMIT 1"),
            "cancelled");
  EXPECT_EQ(value("SELECT count(*) FROM openadr2b_report_requests "
                  "WHERE ended_at IS NULL"),
            "0");

  store->recordReportRequest(request);
  request.programRequestId = "rr-2";
  store->recordReportRequest(request);
  EXPECT_EQ(
      store->endAllReportRequests(openadr2b::ReportRequestStatus::Completed),
      2);
  EXPECT_EQ(
      store->endAllReportRequests(openadr2b::ReportRequestStatus::Completed),
      0);

  // Deleting the offer keeps the history.
  client("DELETE FROM openadr2b_report_offers");
  EXPECT_EQ(value("SELECT count(*) FROM openadr2b_report_requests"), "4");
  EXPECT_EQ(value("SELECT count(*) FROM openadr2b_report_requests "
                  "WHERE offer_id IS NULL"),
            "4");
}

TEST_F(PgStoreTest, ReportChangesAreSignalled) {
  const auto id = addConnection();
  pqxx::connection listening(connectionString);
  ChannelListener reports(listening, "grid_services_reports");
  reports.heard();

  auto store = runnerStore(id);
  openadr2b::ReportRequest request;
  request.programRequestId = "rr-1";
  request.specifierId = "TELEMETRY_STATUS";
  request.reportName = "TELEMETRY_STATUS";
  request.rids = {"status"};
  store->recordReportRequest(request);
  EXPECT_EQ(reports.heard(), std::vector<std::string>{std::to_string(id)});

  ReportDelivery delivery;
  delivery.programRequestId = "rr-1";
  delivery.ok = true;
  store->recordReportDelivery(delivery);
  store->endReportRequest("rr-1", openadr2b::ReportRequestStatus::Completed);
  EXPECT_EQ(reports.heard(2).size(), 2u);
}

TEST_F(PgStoreTest, ClientCannotWriteReportHistory) {
  const auto id = addConnection();
  EXPECT_THROW(client("INSERT INTO openadr2b_report_requests (connection_id, "
                      "report_request_id, specifier_id, report_name, kind, "
                      "granularity_seconds, report_back_seconds, rids) VALUES (" +
                      std::to_string(id) +
                      ", 'rr', 'x', 'X', 'one_shot', 0, 0, '{}')"),
               pqxx::insufficient_privilege);
}

TEST_F(PgStoreTest, OldReportHistoryIsPruned) {
  const auto id = addConnection();
  sql("INSERT INTO openadr2b_report_requests (connection_id, "
      "report_request_id, specifier_id, report_name, kind, granularity_seconds, "
      "report_back_seconds, rids, status, ended_at, created_at) VALUES (" +
      std::to_string(id) +
      ", 'old', 'TELEMETRY_STATUS', 'TELEMETRY_STATUS', 'periodic', 60, 60, "
      "'{status}', 'completed', now() - interval '40 days', "
      "now() - interval '41 days'), (" +
      std::to_string(id) +
      ", 'live', 'TELEMETRY_STATUS', 'TELEMETRY_STATUS', 'periodic', 60, 60, "
      "'{status}', 'active', NULL, now())");
  sql("INSERT INTO report_deliveries (request_id, connection_id, sent_at, "
      "payload, status) "
      "SELECT r.id, r.connection_id, now() - interval '40 days', '[]', 'sent' "
      "FROM openadr2b_report_requests r WHERE r.report_request_id = 'old'");
  sql("INSERT INTO report_deliveries (request_id, connection_id, sent_at, "
      "payload, status) "
      "SELECT r.id, r.connection_id, now(), '[]', 'sent' "
      "FROM openadr2b_report_requests r "
      "WHERE r.report_request_id = 'live'");

  PgSupervisorStore store(connectionString);
  EXPECT_EQ(store.pruneReportHistory(std::chrono::hours(24 * 30)), 1);
  EXPECT_EQ(value("SELECT count(*) FROM report_deliveries"), "1");
  EXPECT_EQ(value("SELECT report_request_id FROM openadr2b_report_requests"),
            "live")
      << "an ended request with nothing left goes too";
  EXPECT_EQ(store.pruneReportHistory(std::chrono::hours(24 * 30)), 0);
}

// ---------------------------------------------------------------------------
// The protocol-neutral contract, from the OpenADR-only one

namespace {

// Applies the protocol_neutral migration and every later one.
void migrateProtocolNeutral(pqxx::connection &connection) {
  pqxx::nontransaction tx(connection);
  for (const auto &file : migrationsFrom("protocol_neutral")) {
    tx.exec(migrationSection(file, "up"));
  }
}

} // namespace

TEST_F(PgStoreTest, MigrationSeparatesWhatIsOpenadrsOwn) {
  rollBackTo(*admin, "protocol_neutral");
  const auto id = addConnection("ML");
  const auto plain = addConnection("ML2");
  const auto connection = std::to_string(id);
  sql("UPDATE openadr2b_connections SET client_cert_pem = 'CERT', "
      "client_key_sealed = 'SEALED', cert_fingerprint = 'ab:cd' "
      "WHERE connection_id = " + connection);
  sql("UPDATE connections SET accepted_signals = ARRAY['SIMPLE', 'LOAD_DISPATCH'] "
      "WHERE id = " + connection);
  sql("INSERT INTO connection_state (connection_id, registration, ven_id) "
      "VALUES (" + connection + ", 'registered', 'ven-1')");
  // Two reports measure power from different streams under the same rID,
  // and energy from the same one; one data point could never be read.
  sql("INSERT INTO report_offers (connection_id, report_name, specifier_id, "
      "min_sampling_seconds, max_sampling_seconds, data_points) VALUES (" +
      connection + ", 'TELEMETRY_USAGE', 'usage-1', 60, 3600, '[" +
      R"({"rid": "power", "resource_id": "ML",
          "item": {"kind": "power_real", "units": "W"},
          "source": {"node_id": 1, "source_id": "/meter", "property": "watts"}},
         {"rid": "energy", "resource_id": "ML",
          "item": {"kind": "energy_real", "units": "Wh"},
          "source": {"node_id": 1, "source_id": "/meter", "property": "wattHours"}})" +
      "]'), (" + connection + ", 'SITE_LOAD', 'load-1', 60, 3600, '[" +
      R"({"rid": "power", "resource_id": "ML",
          "source": {"node_id": 1, "source_id": "/inverter", "property": "watts"}},
         {"rid": "energy", "resource_id": "ML",
          "item": {"kind": "energy_real", "units": "Wh"},
          "source": {"node_id": 1, "source_id": "/meter", "property": "wattHours"}},
         {"rid": "broken", "resource_id": "ML"})" +
      "]')");
  sql("INSERT INTO report_requests (connection_id, offer_id, report_request_id, "
      "specifier_id, report_name, kind, granularity_seconds, "
      "report_back_seconds, rids) SELECT connection_id, id, 'rr-1', "
      "specifier_id, report_name, 'periodic', 60, 300, '{power,energy}' "
      "FROM report_offers WHERE specifier_id = 'usage-1'");
  sql(std::string("INSERT INTO report_deliveries (request_id, connection_id, "
                  "payload, status) SELECT id, connection_id, '[") +
      R"({"rid": "power", "value": 5, "quality": "Quality Good - Non Specific"},
         {"rid": "energy", "value": null, "quality": "No Quality - No Value"},
         {"rid": "status", "online": true, "manual_override": false,
          "quality": "Quality Bad - Comm Failure"})" +
      "]', 'sent' FROM report_requests");

  migrateProtocolNeutral(*admin);

  EXPECT_EQ(value("SELECT client_id FROM connection_state"), "ven-1");
  EXPECT_EQ(value("SELECT client_id FROM connection_health WHERE connection_id = " +
                  connection),
            "ven-1");

  // The certificate is a credential of the connection's own.
  EXPECT_EQ(value("SELECT count(*) FROM client_credentials"), "1");
  EXPECT_EQ(value("SELECT k.name || ' ' || k.cert_pem || ' ' || k.key_sealed "
                  "|| ' ' || k.cert_fingerprint FROM connections c "
                  "JOIN client_credentials k ON k.id = c.client_credential_id"),
            "ML (connection " + connection + ") CERT SEALED ab:cd");
  EXPECT_EQ(value("SELECT count(*) FROM connections "
                  "WHERE client_credential_id IS NULL AND id = " +
                  std::to_string(plain)),
            "1");
  const auto credential = runnerStore(id)->loadClientCredential();
  ASSERT_TRUE(credential);
  EXPECT_EQ(credential->keySealed, "SEALED");

  EXPECT_EQ(runnerStore(id)->loadOpenadr2bSettings().acceptedSignals,
            (std::vector<std::string>{"SIMPLE", "LOAD_DISPATCH"}));
  EXPECT_EQ(runnerStore(plain)->loadOpenadr2bSettings().acceptedSignals,
            std::vector<std::string>{"SIMPLE"});

  // Data points became telemetry points, shared where they measured the
  // same thing.
  EXPECT_EQ(value("SELECT string_agg(point_id || '=' || (source ->> 'source_id'), "
                  "',' ORDER BY point_id) FROM telemetry_points"),
            "energy=/meter,load-1/power=/inverter,usage-1/power=/meter");
  EXPECT_EQ(value("SELECT kind || ' ' || units || ' ' || scale "
                  "FROM telemetry_points WHERE point_id = 'load-1/power'"),
            "power_real W none");
  auto store = runnerStore(id);
  const auto points = store->loadTelemetryPoints();
  const auto rows = store->loadReportOffers();
  ASSERT_EQ(rows.size(), 2u);
  const auto usage = openadr2b::bindOffer(rows[0], points);
  ASSERT_EQ(usage.dataPoints.size(), 2u);
  EXPECT_EQ(usage.dataPoints[0].rid, "power");
  EXPECT_EQ(usage.dataPoints[0].pointId, "usage-1/power");
  EXPECT_EQ(usage.dataPoints[1].pointId, "energy");
  EXPECT_EQ(usage.dataPoints[1].source.property, "wattHours");
  EXPECT_FALSE(rows[0].dataPoints[0].contains("source"));
  EXPECT_EQ(rows[1].dataPoints[2],
            (nlohmann::json{{"rid", "broken"}, {"resource_id", "ML"}}))
      << "a data point that could never be read stays as it was";

  const auto payload = nlohmann::json::parse(
      value("SELECT payload FROM report_deliveries"));
  EXPECT_EQ(payload[0]["point_id"], "usage-1/power");
  EXPECT_EQ(payload[0]["quality"], "good");
  EXPECT_EQ(payload[1]["point_id"], "energy");
  EXPECT_EQ(payload[1]["quality"], "no_value");
  EXPECT_FALSE(payload[2].contains("point_id"));
  EXPECT_EQ(payload[2]["quality"], "unreadable");

  // And back again.
  rollBackTo(*admin, "protocol_neutral");
  EXPECT_EQ(value("SELECT ven_id FROM connection_health WHERE connection_id = " +
                  connection),
            "ven-1");
  EXPECT_EQ(value("SELECT client_cert_pem || ' ' || client_key_sealed "
                  "FROM openadr2b_connections WHERE connection_id = " +
                  connection),
            "CERT SEALED");
  EXPECT_EQ(value("SELECT accepted_signals::text FROM connections "
                  "WHERE id = " + connection),
            "{SIMPLE,LOAD_DISPATCH}");
  const auto restored = nlohmann::json::parse(value(
      "SELECT data_points FROM report_offers WHERE specifier_id = 'usage-1'"));
  EXPECT_EQ(restored[0]["rid"], "power");
  EXPECT_EQ(restored[0]["source"]["source_id"], "/meter");
  EXPECT_EQ(restored[0]["item"]["kind"], "power_real");
  EXPECT_FALSE(restored[0].contains("point_id"));
  const auto restoredPayload = nlohmann::json::parse(
      value("SELECT payload FROM report_deliveries"));
  EXPECT_EQ(restoredPayload[0]["quality"], "Quality Good - Non Specific");
  EXPECT_FALSE(restoredPayload[0].contains("point_id"));
}

TEST_F(PgStoreTest, MigrationRefusesHalfACertificate) {
  rollBackTo(*admin, "protocol_neutral");
  const auto id = addConnection();
  sql("UPDATE openadr2b_connections SET client_cert_pem = 'CERT' "
      "WHERE connection_id = " + std::to_string(id));
  EXPECT_THROW(migrateProtocolNeutral(*admin), pqxx::sql_error);
}
