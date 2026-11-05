#include "support/fake_adapter.hpp"

#include <stdexcept>

namespace grid_services::testing {

void FakeAdapterScript::queuePoll(PollStep step) {
  std::lock_guard lock(mutex);
  polls.push_back(std::move(step));
}

void FakeAdapterScript::queuePollError(AdapterError error) {
  queuePoll([error](ProgramEventSink &) -> PollResult { throw error; });
}

int FakeAdapterScript::polled() const {
  std::lock_guard lock(mutex);
  return pollCalls;
}

FakeAdapter::FakeAdapter(std::shared_ptr<FakeAdapterScript> script)
    : script_(std::move(script)) {}

std::optional<RegistrationInfo> FakeAdapter::resume(const ConnectionState &,
                                                    ProgramEventSink &) {
  std::lock_guard lock(script_->mutex);
  ++script_->resumeCalls;
  if (!script_->resumeErrors.empty()) {
    const auto error = script_->resumeErrors.front();
    script_->resumeErrors.pop_front();
    throw error;
  }
  return script_->resumeResult;
}

RegistrationInfo FakeAdapter::registerConnection(ProgramEventSink &) {
  std::lock_guard lock(script_->mutex);
  ++script_->registerCalls;
  if (script_->registerError) {
    throw *script_->registerError;
  }
  return script_->registration;
}

void FakeAdapter::unregisterConnection(ProgramEventSink &) {
  std::lock_guard lock(script_->mutex);
  ++script_->unregisterCalls;
  if (!script_->unregisterErrors.empty()) {
    const auto error = script_->unregisterErrors.front();
    script_->unregisterErrors.pop_front();
    throw error;
  }
}

TestResult FakeAdapter::test(ProgramEventSink &) {
  std::lock_guard lock(script_->mutex);
  if (script_->testError) {
    throw *script_->testError;
  }
  return script_->testResult;
}

PollResult FakeAdapter::poll(ProgramEventSink &sink) {
  FakeAdapterScript::PollStep step;
  {
    std::lock_guard lock(script_->mutex);
    ++script_->pollCalls;
    script_->pollTimes.push_back(std::chrono::steady_clock::now());
    script_->pollWanted = false;
    if (script_->polls.empty()) {
      return {};
    }
    step = std::move(script_->polls.front());
    script_->polls.pop_front();
  }
  // Called without the lock: the step may use the sink, which uses the store.
  return step(sink);
}

void FakeAdapter::respond(const KnownEvent &event, Opt opt,
                          ProgramEventSink &sink) {
  std::optional<AdapterError> error;
  {
    std::lock_guard lock(script_->mutex);
    error = script_->respondError;
    if (!error) {
      script_->responses.emplace_back(event.id, opt);
    }
  }
  sink.responseSent(event, optAnswer(opt), event.modificationNumber, !error,
                    error ? error->what() : "");
  if (error) {
    throw *error;
  }
}

void FakeAdapter::setParticipation(Participation participation) {
  std::lock_guard lock(script_->mutex);
  script_->participation.push_back(participation);
}

std::optional<std::chrono::seconds> FakeAdapter::requestedPollInterval() const {
  std::lock_guard lock(script_->mutex);
  return script_->pollInterval;
}

void FakeAdapter::runDueWork(ProgramEventSink &) {
  std::lock_guard lock(script_->mutex);
  ++script_->dueWorkRuns;
}

std::optional<std::chrono::milliseconds> FakeAdapter::dueWorkIn() const {
  std::lock_guard lock(script_->mutex);
  return script_->dueWorkIn;
}

bool FakeAdapter::pollWanted() const {
  std::lock_guard lock(script_->mutex);
  return script_->pollWanted;
}

bool FakeAdapter::jittersPolls() const {
  std::lock_guard lock(script_->mutex);
  return script_->jittersPolls;
}

std::optional<std::chrono::milliseconds>
FakeAdapter::retryDelay(int failures, std::chrono::milliseconds pollInterval) {
  std::lock_guard lock(script_->mutex);
  script_->retriesAsked.emplace_back(failures, pollInterval);
  return script_->retryDelay;
}

nlohmann::json FakeAdapter::takeExchange() { return nullptr; }

AdapterFactory fakeAdapters(std::shared_ptr<FakeAdapterScript> script) {
  return [script](const AdapterContext &) -> std::unique_ptr<Adapter> {
    {
      std::lock_guard lock(script->mutex);
      ++script->created;
      if (script->failCreate) {
        throw AdapterError::permanent(*script->failCreate);
      }
    }
    return std::make_unique<FakeAdapter>(script);
  };
}

} // namespace grid_services::testing
