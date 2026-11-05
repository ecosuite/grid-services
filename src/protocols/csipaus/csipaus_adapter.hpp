#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/adapter.hpp"
#include "http/http_client.hpp"
#include "protocols/csipaus/controls.hpp"
#include "protocols/csipaus/identity.hpp"
#include "protocols/csipaus/limits.hpp"
#include "protocols/csipaus/reporter.hpp"
#include "protocols/csipaus/resource_schedule.hpp"
#include "protocols/csipaus/sep_client.hpp"
#include "protocols/csipaus/store.hpp"

namespace grid_services {
class SecretOpener;
}

namespace grid_services::csipaus {

// How far the server's clock may be from ours before activity says so.
inline constexpr std::chrono::seconds clockTolerance{10};
// How long after the server did not take a started or completed answer it
// is sent again, at the soonest (CSIP-AUS v1.2 explainer, section 8.2).
inline constexpr std::chrono::seconds progressRetry{10};
// The least time between attempts to reach the server (explainer 8.2).
inline constexpr std::chrono::seconds retrySpacing{10};

// How long to wait after `failures` attempts in a row to reach the server
// have failed (CSIP-AUS v1.2 explainer, section 8.2): 10 s, then 20 s, each
// lengthened by up to half and at most half `pollRate`, so both fit within
// it; from the third, `pollRate` exactly, as a poll is. Never under
// retrySpacing.
std::chrono::milliseconds retryDelay(int failures,
                                     std::chrono::milliseconds pollRate,
                                     std::mt19937 &random);

struct CsipausOptions {
  std::chrono::seconds connectTimeout{10};
  std::chrono::seconds requestTimeout{30};
  // The IANA Private Enterprise Number that ends the mRIDs the worker
  // makes for mirror usage points and their readings.
  std::uint32_t pen = 0;
};

// Offered over TLS 1.2: first the cipher suite IEEE 2030.5 requires every
// device to support, then the common ECDHE AES-GCM suites, for servers
// behind TLS terminators that do not offer it.
inline constexpr const char *cipherList =
    "ECDHE-ECDSA-AES128-CCM8:ECDHE-ECDSA-AES128-GCM-SHA256:"
    "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES256-GCM-SHA384";

// A CSIP-AUS utility client for one site: an IEEE 2030.5 EndDevice known to
// the server by the LFDI of the connection's certificate.
//
// Registering finds the EndDevice in the server's EndDeviceList, or creates
// it, then checks the PIN against its Registration, when the server gives
// one, and sends the ConnectionPoint when the settings have them. An
// EndDevice the server has lost is created again the same way, on resuming
// or polling. Polling walks from DeviceCapability, through the
// EndDeviceList, the EndDevice, Time and its FunctionSetAssignments, to each
// DERProgram's default and scheduled controls, fetching each resource only
// when the server's poll rates say it is due, and describes what it found
// when that changes. It asks to be polled again when the next is due.
//
// Each DERControl is stored as an event the site cannot decline, with the
// window the device keeps after its randomization, and each program's
// DefaultDERControl as an open-ended one. The server is told a control was
// received, cancelled or superseded when the control's responseRequired asks
// for it. A control is superseded when the server says so, or when, by this
// device's own resolution, controls that outrank it hold every cap it sets
// until it ends; once superseded it stays so, and out of the caps in force.
// A control that ended before the worker first saw it is not stored.
// It is told a control started and completed as the planner's lifecycle
// says, on polls and between them: one status per control at a time, never
// two in one second, and one the server did not take is sent again
// progressRetry later at the soonest.
//
// The device keeps time by the server's Time resource: controls are judged
// by the server's clock, and how far it is from ours is recorded, with
// activity when that is more than clockTolerance. Each walk also works out
// the caps in force, and stores them when they change.
//
// Between polls, the Reporter tells the server about the site: its DER and
// the DER's state, and its readings at the server's post rate. When the DER
// disconnects from the grid while no connect or energize cap of 0 is in
// force, the adapter asks to be polled at once, and that poll fetches the
// programs again whatever their schedule (explainer 5.3.1). If it fails, the
// caps in force are the last walk's defaults alone until a walk succeeds.
class CsipausAdapter : public Adapter {
public:
  // `clock` schedules fetches; `wallClock` says when controls have ended.
  CsipausAdapter(ConnectionConfig config, CsipausSettings settings,
                 DeviceIdentity identity, std::unique_ptr<http::HttpClient> http,
                 RunnerStore &store,
                 std::function<Clock::time_point()> clock = nullptr,
                 std::function<Timestamp()> wallClock = nullptr,
                 ReportingSetup reporting = {});

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
  // Whether the DER disconnected and its programs are to be polled again.
  bool pollWanted() const override;
  // False: the server spreads its clients by its own randomization, so
  // polls keep its rates exactly.
  bool jittersPolls() const override;
  // By retryDelay.
  std::optional<std::chrono::milliseconds>
  retryDelay(int failures, std::chrono::milliseconds pollInterval) override;
  void setParticipation(Participation participation) override;
  std::optional<std::chrono::seconds> requestedPollInterval() const override;
  std::string describeParticipation(Participation participation,
                                    Opt defaultOpt) const override;
  std::string describeClient(const std::string &clientId) const override;
  nlohmann::json takeExchange() override;

private:
  struct Found {
    std::string href;
    std::unique_ptr<sep::EndDevice> device;
  };

  struct ProgramList {
    std::shared_ptr<sep::DERProgramList> list;
    std::chrono::seconds pollRate;
  };

  // Starts an operation with a fresh exchange record.
  void begin();
  RegistrationInfo establish(ProgramEventSink &sink);
  RegistrationInfo registrationInfo();
  // Fetches DeviceCapability, which the walk starts from and its links are
  // read from; refreshing fetches it when due.
  const sep::DeviceCapability &fetchDeviceCapability();
  void refreshDeviceCapability();
  std::string endDeviceListHref() const;
  // Fetches the EndDeviceList, whose poll rate the EndDevice is polled at;
  // refreshing fetches it when due.
  std::unique_ptr<sep::EndDeviceList> fetchEndDeviceList(const std::string &href);
  void refreshEndDeviceList();
  // Our EndDevice in the server's list, if it has one.
  std::optional<Found> findEndDevice(const std::string &listHref);
  bool isOurs(const sep::EndDevice &device) const;
  void checkRegistrationPin(const sep::EndDevice &device,
                            ProgramEventSink &sink);
  void sendConnectionPoint(const sep::EndDevice &device);
  // Fetches the EndDevice when due. Returns a result when the server no
  // longer has it.
  std::optional<PollResult> refreshEndDevice();
  // The device's DERProgramLists, each with its poll rate. Sets the poll
  // rate of the device's controls.
  std::vector<ProgramList> programLists(const sep::EndDevice &device);
  std::string walkPrograms(const sep::EndDevice &device);
  std::string describeProgram(const sep::DERProgram &program,
                              std::chrono::seconds pollRate);
  // Reads the server's Time when it is due.
  void refreshTime(ProgramEventSink &sink);
  // Now, by the server's clock.
  Timestamp serverNow() const;
  // Stores what the walk observed, and ends the events it no longer lists.
  void syncEvents(ProgramEventSink &sink);
  // Writes an event's detail unless it is what this worker last wrote.
  void recordDetail(const KnownEvent &event, const CsipausEventDetail &detail);
  // Stores the caps in force when they have changed.
  void updateLimits();
  // The DER disconnected from the grid: unless a cap switched it off, its
  // programs are polled again.
  void disconnected(ProgramEventSink &sink);
  // The programs could not be polled again after a disconnection: the caps
  // in force are the last walk's defaults alone.
  void keepDefaultsOnly(ProgramEventSink &sink);
  // Tells the server of controls the site has started or completed, a status
  // per control, unless an answer it did not take is not yet due again.
  void answerProgress(ProgramEventSink &sink,
                      std::vector<std::string> &failures);
  // Whether the server was told something of the event in this second of
  // its clock.
  bool answeredThisSecond(EventRowId eventId);
  // Tells the server what became of a control. Returns false, and adds to
  // `failures`, when it could not be told.
  bool answer(const KnownEvent &event, const CsipausEventDetail &detail,
              EventAnswer answer, ProgramEventSink &sink,
              std::vector<std::string> &failures);
  void forget();

  template <typename T>
  std::shared_ptr<T> fetchList(const std::string &href,
                               std::map<std::string, std::shared_ptr<T>> &cache);
  template <typename T>
  std::shared_ptr<T> fetch(const std::string &href,
                           std::chrono::seconds pollRate,
                           std::map<std::string, std::shared_ptr<T>> &cache);

  ConnectionConfig config_;
  CsipausSettings settings_;
  DeviceIdentity identity_;
  SepClient client_;
  RunnerStore &runnerStore_;
  CsipausStore &store_;
  std::function<Timestamp()> wallClock_;
  ResourceSchedule schedule_;
  Participation participation_ = Participation::Active;

  // The latest DeviceCapability.
  std::unique_ptr<sep::DeviceCapability> dcap_;
  std::optional<std::string> endDeviceHref_;
  // The EndDeviceList's poll rate, which the EndDevice is polled at.
  std::chrono::seconds endDevicePollRate_{900};
  std::unique_ptr<sep::EndDevice> endDevice_;
  // The resources the current poll visited, and those the last one did,
  // which say when to poll next.
  std::set<std::string> visited_;
  std::set<std::string> walked_;
  std::map<std::string, std::shared_ptr<sep::FunctionSetAssignmentsList>>
      assignments_;
  std::map<std::string, std::shared_ptr<sep::DERProgramList>> programLists_;
  std::map<std::string, std::shared_ptr<sep::DefaultDERControl>> defaults_;
  std::map<std::string, std::shared_ptr<sep::DERControlList>> controls_;
  // The shortest DERProgramList poll rate, which the runner polls at.
  std::optional<std::chrono::seconds> controlPollRate_;
  std::string lastDescription_;
  // The controls the current walk found, and those the last complete walk
  // did.
  std::vector<ObservedEvent> observed_;
  std::optional<std::vector<ObservedEvent>> lastWalk_;
  // Whether the next poll fetches the programs again, after the DER
  // disconnected.
  bool repollWanted_ = false;
  // How far the server's clock is ahead of ours.
  std::chrono::seconds clockOffset_{0};
  bool clockOutOfTolerance_ = false;
  // What was last stored, so unchanged caps are not written again.
  std::optional<std::vector<LimitSegment>> storedLimits_;
  // The detail last written for each event the last walk listed, by program
  // event id.
  std::map<std::string, CsipausEventDetail> recordedDetails_;
  // When the server was last told something of each event, by its clock.
  std::map<EventRowId, Timestamp> lastAnswered_;
  // When started and completed answers are sent again after the server did
  // not take one.
  std::optional<Timestamp> progressRetryAt_;
  std::mt19937 random_{std::random_device{}()};
  Reporter reporter_;
};

// Builds adapters for 'csipaus' connections. `opener` opens sealed client
// keys; a csipaus connection always has one.
AdapterFactory makeCsipausFactory(const SecretOpener *opener,
                                  CsipausOptions options = {});

} // namespace grid_services::csipaus
