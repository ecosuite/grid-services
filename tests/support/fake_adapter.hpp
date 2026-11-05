#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/adapter.hpp"
#include "core/errors.hpp"

namespace grid_services::testing {

// Shared between a test and every FakeAdapter the factory creates, so a test
// can script behaviour and observe calls across runner restarts.
struct FakeAdapterScript {
  using PollStep = std::function<PollResult(ProgramEventSink &)>;

  mutable std::mutex mutex;

  // Behaviour
  std::optional<std::string> failCreate;
  std::optional<RegistrationInfo> resumeResult =
      RegistrationInfo{"ven-1", "reg-1", std::nullopt};
  RegistrationInfo registration{"ven-1", "reg-1", std::nullopt};
  std::optional<AdapterError> registerError;
  // Failures for successive attempts to resume; then it succeeds.
  std::deque<AdapterError> resumeErrors;
  // Failures for successive attempts to unregister; then it succeeds.
  std::deque<AdapterError> unregisterErrors;
  std::optional<AdapterError> respondError;
  std::optional<AdapterError> testError;
  TestResult testResult = {"reached the fake VTN", false, std::nullopt};
  std::deque<PollStep> polls;
  std::optional<std::chrono::seconds> pollInterval;
  std::optional<std::chrono::milliseconds> dueWorkIn;
  // Asks for a poll after due work; the next poll clears it.
  bool pollWanted = false;
  // What retryDelay gives; nothing keeps the runner's backoff.
  std::optional<std::chrono::milliseconds> retryDelay;
  bool jittersPolls = true;

  // Observations
  int created = 0;
  int pollCalls = 0;
  std::vector<std::chrono::steady_clock::time_point> pollTimes;
  int dueWorkRuns = 0;
  int registerCalls = 0;
  int resumeCalls = 0;
  int unregisterCalls = 0;
  std::vector<std::pair<EventRowId, Opt>> responses;
  std::vector<Participation> participation;
  // The failures and poll interval each retryDelay was asked with.
  std::vector<std::pair<int, std::chrono::milliseconds>> retriesAsked;

  void queuePoll(PollStep step);
  void queuePollError(AdapterError error);
  int polled() const;
};

class FakeAdapter : public Adapter {
public:
  explicit FakeAdapter(std::shared_ptr<FakeAdapterScript> script);

  std::optional<RegistrationInfo> resume(const ConnectionState &state,
                                         ProgramEventSink &sink) override;
  RegistrationInfo registerConnection(ProgramEventSink &sink) override;
  void unregisterConnection(ProgramEventSink &sink) override;
  TestResult test(ProgramEventSink &sink) override;
  PollResult poll(ProgramEventSink &sink) override;
  void respond(const KnownEvent &event, Opt opt,
               ProgramEventSink &sink) override;
  void runDueWork(ProgramEventSink &sink) override;
  std::optional<std::chrono::milliseconds> dueWorkIn() const override;
  bool pollWanted() const override;
  bool jittersPolls() const override;
  std::optional<std::chrono::milliseconds>
  retryDelay(int failures, std::chrono::milliseconds pollInterval) override;
  void setParticipation(Participation participation) override;
  std::optional<std::chrono::seconds> requestedPollInterval() const override;
  std::string describeClient(const std::string &clientId) const override {
    return "client " + clientId;
  }
  nlohmann::json takeExchange() override;

private:
  std::shared_ptr<FakeAdapterScript> script_;
};

AdapterFactory fakeAdapters(std::shared_ptr<FakeAdapterScript> script);

} // namespace grid_services::testing
