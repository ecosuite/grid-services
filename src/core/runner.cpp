#include "core/runner.hpp"

#include <algorithm>

#include "core/logging.hpp"

namespace grid_services {

std::string toString(RunnerExit exit) {
  switch (exit) {
  case RunnerExit::Running:
    return "running";
  case RunnerExit::Stopped:
    return "stopped";
  case RunnerExit::RestartRequested:
    return "restart requested";
  case RunnerExit::Failed:
    return "failed";
  }
  return "unknown";
}

ConnectionRunner::ConnectionRunner(ConnectionConfig config,
                                   std::unique_ptr<RunnerStore> store,
                                   const AdapterRegistry &adapters,
                                   RunnerOptions options)
    : config_(std::move(config)), store_(std::move(store)), adapters_(adapters),
      options_(std::move(options)),
      log_(makeLogger("connection-" + std::to_string(config_.id))) {}

ConnectionRunner::~ConnectionRunner() {
  requestStop();
  join();
}

void ConnectionRunner::start() {
  thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void ConnectionRunner::requestStop() { thread_.request_stop(); }

void ConnectionRunner::join() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ConnectionRunner::wake() {
  {
    std::lock_guard lock(wakeMutex_);
    wakeRequested_ = true;
  }
  wakeCondition_.notify_all();
}

void ConnectionRunner::run(std::stop_token stop) {
  std::stop_callback onStop(stop, [this] { abort_ = true; });

  try {
    setUp();
    if (isActive(config_)) {
      loop(stop);
    } else {
      retire(stop);
    }
    exit_ = restartRequested_ ? RunnerExit::RestartRequested
                              : RunnerExit::Stopped;
    log_->info("stopped ({})", toString(exit_.load()));
  } catch (const std::exception &error) {
    log_->error("runner failed: {}", error.what());
    try {
      store_->recordError(std::string("Worker error: ") + error.what(), now());
    } catch (...) {
      // The store may be the thing that failed.
    }
    exit_ = RunnerExit::Failed;
  }
}

void ConnectionRunner::setUp() {
  const int interrupted = store_->failInterruptedCommands(
      "The worker stopped before this command finished.");
  if (interrupted > 0) {
    log_->warn("failed {} command(s) interrupted by a previous worker",
               interrupted);
  }

  state_ = store_->loadState();

  sink_ = std::make_unique<StoreEventSink>(*store_, config_.defaultOpt, *log_);
  sink_->setParticipation(state_.participation);

  try {
    adapter_ = adapters_.create({config_, *store_, abort_});
    adapter_->setParticipation(state_.participation);
  } catch (const std::exception &error) {
    adapterError_ = error.what();
  }

  store_->markStarted(config_.configVersion, options_.workerId,
                      adapter_ ? std::nullopt
                               : std::optional<std::string>(adapterError_));
  if (!adapter_) {
    log_->error("configuration is invalid: {}", adapterError_);
    store_->recordError("Configuration is invalid: " + adapterError_, now());
    activity(ActivityCategory::Connection,
             "Connection configuration is invalid: " + adapterError_);
  }

  needsResume_ = adapter_ != nullptr && state_.registrationId.has_value() &&
                 state_.registration != Registration::Unregistered;

  log_->info("started: protocol {}, config version {}, registration {}, "
             "participation {}",
             config_.protocol, config_.configVersion,
             toString(state_.registration), toString(state_.participation));
}

void ConnectionRunner::loop(std::stop_token stop) {
  nextPoll_ = Clock::now();

  while (!stop.stop_requested()) {
    processCommands(stop);
    if (restartRequested_ || stop.stop_requested()) {
      return;
    }

    if (polling() && Clock::now() >= nextPoll_) {
      pollNow();
    }
    const bool registered =
        adapter_ && state_.registration == Registration::Registered;
    if (registered) {
      runDueWork();
      if (adapter_->pollWanted()) {
        nextPoll_ = Clock::now();
      }
    }

    store_->heartbeat(now());

    auto deadline = Clock::now() + options_.heartbeatInterval;
    if (polling()) {
      deadline = std::min(deadline, nextPoll_);
    }
    if (const auto due = registered ? adapter_->dueWorkIn() : std::nullopt) {
      deadline = std::min(deadline,
                          Clock::now() + std::max(*due, std::chrono::milliseconds(0)));
    }
    waitUntil(stop, deadline);
  }
}

void ConnectionRunner::waitUntil(std::stop_token stop,
                                 Clock::time_point deadline) {
  std::unique_lock lock(wakeMutex_);
  wakeCondition_.wait_until(lock, stop, deadline,
                            [this] { return wakeRequested_; });
  wakeRequested_ = false;
}

void ConnectionRunner::retire(std::stop_token stop) {
  const std::string reason = config_.archived ? "archived" : "disabled";

  while (!stop.stop_requested() &&
         state_.registration == Registration::Registered) {
    if (!adapter_) {
      markPermanentError("The connection was " + reason +
                         " but could not be unregistered: its configuration "
                         "is invalid: " +
                         adapterError_);
      return;
    }

    std::chrono::milliseconds delay{};
    try {
      if (needsResume_ && !confirmRegistration()) {
        exchangeActivity("The connection was " + reason +
                         ". The program no longer had its registration.");
        return;
      }
      adapter_->unregisterConnection(*sink_);
      forgetRegistration();
      state_.clientId.reset();
      exchangeActivity("Unregistered from the program because the "
                       "connection was " +
                       reason);
      log_->info("unregistered: the connection was {}", reason);
      return;
    } catch (const AdapterError &error) {
      const auto message =
          "Unregistering the " + reason + " connection failed: " + error.what();
      switch (error.kind()) {
      case AdapterError::Kind::Stopped:
        return;
      case AdapterError::Kind::Permanent:
        markPermanentError(message);
        return;
      case AdapterError::Kind::Transient:
        delay = failedAttempt(message);
        if (consecutiveFailures_ == 1) {
          exchangeActivity(message + ". The worker will keep trying.");
        }
        log_->warn("{}; retrying in {}ms", message, delay.count());
        break;
      }
    }

    store_->heartbeat(now());
    waitUntil(stop, Clock::now() + delay);
  }
}

bool ConnectionRunner::polling() const {
  return adapter_ != nullptr &&
         (state_.registration == Registration::Registered || needsResume_);
}

void ConnectionRunner::processCommands(std::stop_token stop) {
  while (!stop.stop_requested()) {
    const auto command = store_->claimNextCommand();
    if (!command) {
      return;
    }
    log_->info("command {}: {}", command->id, toString(command->kind));

    CommandOutcome outcome;
    try {
      outcome = execute(*command);
    } catch (const AdapterError &error) {
      outcome = {false,
                 error.kind() == AdapterError::Kind::Stopped
                     ? "The worker stopped before this command finished."
                     : error.what(),
                 nullptr};
    } catch (const std::exception &error) {
      outcome = {false, error.what(), nullptr};
    }

    store_->completeCommand(command->id, outcome);
    log_->info("command {} {}: {}", command->id,
               outcome.ok ? "succeeded" : "failed", outcome.message);

    if (restartRequested_) {
      return;
    }
  }
}

CommandOutcome ConnectionRunner::execute(const Command &command) {
  switch (command.kind) {
  case CommandKind::Pause:
    return setParticipationNow(Participation::Paused, command);
  case CommandKind::Resume:
    return setParticipationNow(Participation::Active, command);
  case CommandKind::ReloadConfig:
    restartRequested_ = true;
    activity(ActivityCategory::Connection, "Connection restarted to reload "
                                           "its configuration",
             std::nullopt, command.requestedBy);
    return {true, "Restarting with the current configuration.", nullptr};
  default:
    break;
  }

  if (!adapter_) {
    return {false, "Connection configuration is invalid: " + adapterError_,
            nullptr};
  }

  switch (command.kind) {
  case CommandKind::Register:
    return registerNow(command.requestedBy);
  case CommandKind::Unregister:
    return unregisterNow(command.requestedBy);
  case CommandKind::Test:
    return testNow(command.requestedBy);
  case CommandKind::OptOut:
    return changeOpt(command, Opt::Out);
  case CommandKind::OptIn:
    return changeOpt(command, Opt::In);
  default:
    return {false, "Unsupported command.", nullptr};
  }
}

CommandOutcome ConnectionRunner::registerNow(const Actor &actor) {
  try {
    const auto info = adapter_->registerConnection(*sink_);
    store_->setRegistered(info);
    store_->recordPoll(now());
    state_.registration = Registration::Registered;
    state_.clientId = info.clientId;
    state_.registrationId = info.registrationId;
    state_.programPollIntervalSeconds = info.pollIntervalSeconds;
    needsResume_ = false;
    consecutiveFailures_ = 0;
    nextPoll_ = Clock::now();
    const auto client = adapter_->describeClient(info.clientId);
    exchangeActivity("Registered with the program as " + client, actor);
    nlohmann::json result = {{"client_id", info.clientId},
                             {"registration_id", info.registrationId},
                             {"poll_interval_seconds", nullptr}};
    if (info.pollIntervalSeconds) {
      result["poll_interval_seconds"] = *info.pollIntervalSeconds;
    }
    return {true, "Registered as " + client + ".", std::move(result)};
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Permanent) {
      markPermanentError(std::string("Registration failed: ") + error.what(),
                         actor);
    } else if (error.kind() == AdapterError::Kind::Transient) {
      const auto message = std::string("Registration failed: ") + error.what();
      store_->recordError(message, now());
      exchangeActivity(message, actor);
    }
    throw;
  }
}

CommandOutcome ConnectionRunner::unregisterNow(const Actor &actor) {
  // After a restart the adapter has yet to learn the registration it is to
  // cancel.
  if (needsResume_) {
    confirmRegistration();
  }
  if (state_.registrationId || state_.registration == Registration::Registered) {
    adapter_->unregisterConnection(*sink_);
  }
  forgetRegistration();
  state_.clientId.reset();
  exchangeActivity("Unregistered from the program", actor);
  return {true, "Unregistered.", nullptr};
}

CommandOutcome ConnectionRunner::testNow(const Actor &actor) {
  try {
    const auto tested = adapter_->test(*sink_);
    exchangeActivity("Connection test passed: " + tested.description, actor);
    nlohmann::json result = {{"registered", tested.registered},
                             {"client_id", nullptr}};
    if (tested.clientId) {
      result["client_id"] = *tested.clientId;
    }
    return {true, tested.description, std::move(result)};
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Stopped) {
      throw;
    }
    exchangeActivity(std::string("Connection test failed: ") + error.what(),
                     actor);
    return {false, error.what(), nullptr};
  }
}

CommandOutcome
ConnectionRunner::setParticipationNow(Participation participation,
                                      const Command &command) {
  const nlohmann::json result = {{"participation", toString(participation)}};
  if (state_.participation == participation) {
    return {true, "Participation is already " + toString(participation) + ".",
            result};
  }

  store_->setParticipation(participation);
  state_.participation = participation;
  sink_->setParticipation(participation);
  if (adapter_) {
    adapter_->setParticipation(participation);
  }

  // Without an adapter, as when the configuration is invalid, the
  // connection still records the choice.
  const auto message =
      adapter_ ? adapter_->describeParticipation(participation,
                                                  config_.defaultOpt)
               : "Participation " + toString(participation) + ".";
  activity(ActivityCategory::Connection, message, std::nullopt,
           command.requestedBy);
  return {true, message, result};
}

CommandOutcome ConnectionRunner::changeOpt(const Command &command, Opt opt) {
  if (!command.eventId) {
    return {false, "No event given.", nullptr};
  }
  const auto event = store_->findEventById(*command.eventId);
  if (!event) {
    return {false, "Event not found for this connection.", nullptr};
  }
  if (isTerminal(event->status)) {
    return {false, "Event is already " + toString(event->status) + ".",
            nullptr};
  }
  if (!event->declinable) {
    return {false,
            "The program applies this event whatever the site answers, so "
            "there is nothing to opt in or out of.",
            nullptr};
  }

  const bool in = opt == Opt::In;
  const std::string opted = in ? "Opted in" : "Opted out";
  nlohmann::json result = {{"opt", toString(opt)}, {"program_told", false}};
  if (event->opt == opt) {
    return {true, "Already " + std::string(in ? "opted in." : "opted out."),
            std::move(result)};
  }
  // Opting in is a commitment to the whole event.
  if (in && now() >= event->startAt) {
    return {false, "Event has already started.", nullptr};
  }

  const auto description =
      opted + (in ? " to event " : " of event ") +
      describeWindow(event->startAt, event->durationSeconds);
  if (!event->responseRequired) {
    // The program takes no answer for this event, so the change only
    // decides whether we act on it.
    store_->setOpt(event->id, opt, OptSource::Operator);
    activity(ActivityCategory::Events,
             description + ". The program takes no answers for this event, "
                           "so it was not told.",
             event->id, command.requestedBy);
    return {true,
            opted + ". The program takes no answers for this event, so it "
                    "was not told.",
            std::move(result)};
  }
  if (state_.registration != Registration::Registered) {
    return {false, "Connection is not registered with the program.", nullptr};
  }

  adapter_->respond(*event, opt, *sink_);
  store_->setOpt(event->id, opt, OptSource::Operator);
  activity(ActivityCategory::Events, description, event->id,
           command.requestedBy);
  result["program_told"] = true;
  return {true, opted + ".", std::move(result)};
}

void ConnectionRunner::pollNow() {
  try {
    if (needsResume_ && !confirmRegistration()) {
      exchangeActivity("The program no longer has this connection's "
                       "registration. Register again to resume.");
      return;
    }

    const auto result = adapter_->poll(*sink_);
    store_->recordPoll(now());
    if (result.pollIntervalSeconds &&
        result.pollIntervalSeconds != state_.programPollIntervalSeconds) {
      store_->setProgramPollInterval(*result.pollIntervalSeconds);
      state_.programPollIntervalSeconds = result.pollIntervalSeconds;
    }
    if (result.summary) {
      exchangeActivity(*result.summary);
    }
    if (consecutiveFailures_ > 0) {
      activity(ActivityCategory::Connection,
               "Contact with the program restored");
    }
    consecutiveFailures_ = 0;
    nextPoll_ = Clock::now();
    if (!result.moreQueued) {
      nextPoll_ += adapter_->jittersPolls()
                       ? withJitter(pollInterval(), config_.pollJitter, random_)
                       : pollInterval();
    }

    switch (result.outcome) {
    case PollOutcome::Ok:
      break;
    case PollOutcome::RegistrationCancelled:
      store_->clearRegistration();
      state_.registration = Registration::Unregistered;
      state_.registrationId.reset();
      activity(ActivityCategory::Connection,
               "The program cancelled this connection's registration. "
               "Register again to resume.");
      break;
    case PollOutcome::ReregistrationRequested:
      // The poll's summary says why.
    activity(ActivityCategory::Connection, "Registering again");
      try {
        registerNow();
      } catch (const AdapterError &error) {
        if (error.kind() == AdapterError::Kind::Transient) {
          nextPoll_ = Clock::now() +
                      failedAttempt(std::string("Registration failed: ") +
                                    error.what());
        }
      }
      break;
    }
  } catch (const AdapterError &error) {
    handleFailure(error, "Poll");
  }
}

bool ConnectionRunner::confirmRegistration() {
  const auto info = adapter_->resume(state_, *sink_);
  needsResume_ = false;

  if (!info) {
    log_->warn("the program no longer has this connection's registration");
    forgetRegistration();
    store_->recordError("The program no longer has this connection's "
                        "registration. Register again to resume.",
                        now());
    return false;
  }
  store_->setRegistered(*info);
  state_.registration = Registration::Registered;
  state_.clientId = info->clientId;
  state_.registrationId = info->registrationId;
  state_.programPollIntervalSeconds = info->pollIntervalSeconds;
  log_->info("resumed registration as {}",
             adapter_->describeClient(info->clientId));
  return true;
}

void ConnectionRunner::forgetRegistration() {
  store_->clearRegistration();
  state_.registration = Registration::Unregistered;
  state_.registrationId.reset();
  state_.programPollIntervalSeconds.reset();
  needsResume_ = false;
}

void ConnectionRunner::runDueWork() {
  try {
    adapter_->runDueWork(*sink_);
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Stopped) {
      return;
    }
    log_->warn("due work failed: {}", error.what());
    store_->recordError(std::string("Report delivery failed: ") + error.what(),
                        now());
  }
}

void ConnectionRunner::handleFailure(const AdapterError &error,
                                     const char *operation) {
  const auto message = std::string(operation) + " failed: " + error.what();

  switch (error.kind()) {
  case AdapterError::Kind::Stopped:
    return;
  case AdapterError::Kind::Transient: {
    const auto delay = failedAttempt(message);
    nextPoll_ = Clock::now() + delay;
    if (consecutiveFailures_ == 1) {
      exchangeActivity("Lost contact with the program: " +
                       std::string(error.what()));
    }
    log_->warn("{}; retrying in {}ms", message, delay.count());
    return;
  }
  case AdapterError::Kind::Permanent:
    markPermanentError(message);
    return;
  }
}

std::chrono::milliseconds
ConnectionRunner::failedAttempt(const std::string &message) {
  ++consecutiveFailures_;
  const auto delay = backoff();
  const auto at = now();
  store_->recordError(message, at);
  store_->recordFailures(consecutiveFailures_,
                         at + std::chrono::ceil<std::chrono::seconds>(delay));
  return delay;
}

void ConnectionRunner::markPermanentError(const std::string &message,
                                          const Actor &actor) {
  store_->setRegistration(Registration::Error);
  store_->recordError(message, now());
  // Nothing is retried from here.
  consecutiveFailures_ = 0;
  store_->recordFailures(0, std::nullopt);
  state_.registration = Registration::Error;
  needsResume_ = false;
  exchangeActivity(message + ". Polling stopped until the configuration "
                             "changes or the connection is registered again.",
                   actor);
  log_->error("{}", message);
}

std::chrono::milliseconds ConnectionRunner::pollInterval() const {
  std::optional<std::chrono::milliseconds> requested;
  if (adapter_) {
    if (const auto interval = adapter_->requestedPollInterval();
        interval && interval->count() > 0) {
      requested = *interval;
    }
  }
  if (!requested && state_.programPollIntervalSeconds &&
      *state_.programPollIntervalSeconds > 0) {
    requested = std::chrono::seconds(*state_.programPollIntervalSeconds);
  }

  if (config_.pollIntervalSeconds && *config_.pollIntervalSeconds > 0) {
    // An override may lengthen the program's requested interval but not
    // shorten it.
    const std::chrono::milliseconds override =
        std::chrono::seconds(*config_.pollIntervalSeconds);
    return requested ? std::max(override, *requested) : override;
  }
  return requested.value_or(options_.defaultPollInterval);
}

std::chrono::milliseconds withJitter(std::chrono::milliseconds interval,
                                     double jitter, std::mt19937 &random) {
  if (jitter <= 0 || interval.count() <= 0) {
    return interval;
  }
  using Rep = std::chrono::milliseconds::rep;
  std::uniform_real_distribution<double> extra(0.0, jitter);
  return interval + std::chrono::milliseconds(static_cast<Rep>(
                        static_cast<double>(interval.count()) * extra(random)));
}

std::chrono::milliseconds ConnectionRunner::backoff() {
  if (const auto delay =
          adapter_ ? adapter_->retryDelay(consecutiveFailures_, pollInterval())
                   : std::nullopt) {
    return *delay;
  }
  using Rep = std::chrono::milliseconds::rep;
  const int exponent = std::clamp(consecutiveFailures_ - 1, 0, 10);
  const auto base = std::min<Rep>(options_.minBackoff.count() * (1LL << exponent),
                                  options_.maxBackoff.count());
  std::uniform_real_distribution<double> jitter(0.8, 1.2);
  const auto jittered =
      static_cast<Rep>(static_cast<double>(base) * jitter(random_));
  return std::chrono::milliseconds(std::max<Rep>(1, jittered));
}

void ConnectionRunner::activity(ActivityCategory category, std::string message,
                                std::optional<EventRowId> eventId,
                                std::optional<std::string> actor,
                                nlohmann::json detail) {
  store_->appendActivity({category, std::move(message), eventId,
                          std::move(detail), std::move(actor)});
}

void ConnectionRunner::exchangeActivity(std::string message,
                                        std::optional<std::string> actor) {
  nlohmann::json detail = nullptr;
  if (adapter_) {
    auto exchange = adapter_->takeExchange();
    if (!exchange.is_null()) {
      detail = {{"exchange", std::move(exchange)}};
    }
  }
  activity(ActivityCategory::Connection, std::move(message), std::nullopt,
           std::move(actor), std::move(detail));
}

} // namespace grid_services
