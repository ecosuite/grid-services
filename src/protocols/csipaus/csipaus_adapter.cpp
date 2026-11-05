#include "protocols/csipaus/csipaus_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/errors.hpp"
#include "crypto/client_credential.hpp"
#include "datum/solarnetwork_client.hpp"
#include "http/curl_http_client.hpp"
#include "protocols/csipaus/controls.hpp"

namespace grid_services::csipaus {

namespace {

bool sameHex(const std::string &a, const std::string &b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::toupper(static_cast<unsigned char>(x)) ==
                  std::toupper(static_cast<unsigned char>(y));
         });
}

std::chrono::seconds seconds(unsigned long value) {
  return std::chrono::seconds(static_cast<long>(value));
}

long long epochNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// EventStatus.currentStatus.
std::string describeStatus(int status) {
  switch (status) {
  case 0:
    return "scheduled";
  case 1:
    return "active";
  case 2:
    return "cancelled";
  case 3:
    return "cancelled with randomization";
  case 4:
    return "superseded";
  default:
    return "status " + std::to_string(status);
  }
}

std::string join(const std::vector<std::string> &parts,
                 const std::string &separator) {
  std::string text;
  for (const auto &part : parts) {
    text += (text.empty() ? "" : separator) + part;
  }
  return text;
}

bool hasEnded(Timestamp start, int durationSeconds, Timestamp at) {
  return durationSeconds > 0 &&
         start + std::chrono::seconds(durationSeconds) <= at;
}

bool asksFor(const CsipausEventDetail &detail, int bit) {
  return detail.replyTo.has_value() && (detail.responseRequired & bit) != 0;
}

int responseStatus(EventAnswer answer) {
  switch (answer) {
  case EventAnswer::Started:
    return responseStarted;
  case EventAnswer::Completed:
    return responseCompleted;
  case EventAnswer::Cancelled:
    return responseCancelled;
  case EventAnswer::Superseded:
    return responseSuperseded;
  default:
    return responseReceived;
  }
}

ProgramInfo programInfo(const sep::DERProgram &program) {
  return {hexString(program.mRID()),
          program.description()
              ? std::optional<std::string>(std::string(*program.description()))
              : std::nullopt,
          static_cast<int>(program.primacy())};
}

} // namespace

std::chrono::milliseconds retryDelay(int failures,
                                     std::chrono::milliseconds pollRate,
                                     std::mt19937 &random) {
  using std::chrono::milliseconds;
  if (failures > 2) {
    return std::max<milliseconds>(pollRate, retrySpacing);
  }
  const milliseconds base = failures <= 1 ? retrySpacing : 2 * retrySpacing;
  std::uniform_real_distribution<double> extra(0.0, 0.5);
  const auto lengthened =
      base + milliseconds(static_cast<milliseconds::rep>(
                 static_cast<double>(base.count()) * extra(random)));
  return std::max<milliseconds>(std::min(lengthened, pollRate / 2),
                                retrySpacing);
}

CsipausAdapter::CsipausAdapter(ConnectionConfig config,
                               CsipausSettings settings,
                               DeviceIdentity identity,
                               std::unique_ptr<http::HttpClient> http,
                               RunnerStore &store,
                               std::function<Clock::time_point()> clock,
                               std::function<Timestamp()> wallClock,
                               ReportingSetup reporting)
    : config_(std::move(config)), settings_(std::move(settings)),
      identity_(std::move(identity)),
      client_(settings_.dcapUrl, std::move(http)), runnerStore_(store),
      store_(csipausStore(store)),
      wallClock_(wallClock ? std::move(wallClock) : [] { return now(); }),
      schedule_(std::move(clock)),
      reporter_(std::move(reporting), client_, identity_.lfdi, schedule_,
                wallClock_, clockOffset_) {}

void CsipausAdapter::begin() { client_.takeExchange(); }

nlohmann::json CsipausAdapter::takeExchange() {
  return client_.takeExchange();
}

void CsipausAdapter::forget() {
  dcap_.reset();
  endDeviceHref_.reset();
  endDevice_.reset();
  visited_.clear();
  walked_.clear();
  assignments_.clear();
  programLists_.clear();
  defaults_.clear();
  controls_.clear();
  schedule_.clear();
  controlPollRate_.reset();
  lastDescription_.clear();
  storedLimits_.reset();
  lastWalk_.reset();
  repollWanted_ = false;
  recordedDetails_.clear();
  progressRetryAt_.reset();
  reporter_.reset();
}

bool CsipausAdapter::isOurs(const sep::EndDevice &device) const {
  if (device.lFDI()) {
    return sameHex(hexString(*device.lFDI()), identity_.lfdi);
  }
  return device.sFDI() == identity_.sfdi;
}

const sep::DeviceCapability &CsipausAdapter::fetchDeviceCapability() {
  const auto &href = client_.dcapHref();
  dcap_ = client_.get<sep::DeviceCapability>(href);
  schedule_.fetched(href, seconds(dcap_->pollRate()));
  return *dcap_;
}

void CsipausAdapter::refreshDeviceCapability() {
  visited_.insert(client_.dcapHref());
  if (!dcap_ || schedule_.due(client_.dcapHref())) {
    fetchDeviceCapability();
  }
}

std::string CsipausAdapter::endDeviceListHref() const {
  if (!dcap_->EndDeviceListLink()) {
    throw AdapterError::permanent(
        "the server's DeviceCapability has no EndDeviceListLink");
  }
  return dcap_->EndDeviceListLink()->href();
}

std::unique_ptr<sep::EndDeviceList>
CsipausAdapter::fetchEndDeviceList(const std::string &href) {
  auto list = client_.getList<sep::EndDeviceList>(href);
  endDevicePollRate_ = seconds(list->pollRate());
  schedule_.fetched(href, endDevicePollRate_);
  return list;
}

void CsipausAdapter::refreshEndDeviceList() {
  const auto href = endDeviceListHref();
  visited_.insert(href);
  if (schedule_.due(href)) {
    fetchEndDeviceList(href);
  }
}

std::optional<CsipausAdapter::Found>
CsipausAdapter::findEndDevice(const std::string &listHref) {
  const auto list = fetchEndDeviceList(listHref);
  for (const auto &device : list->EndDevice()) {
    if (!isOurs(device)) {
      continue;
    }
    if (!device.href()) {
      throw AdapterError::permanent(
          "the server lists this device's EndDevice without an href");
    }
    return Found{*device.href(), std::make_unique<sep::EndDevice>(device)};
  }
  return std::nullopt;
}

void CsipausAdapter::checkRegistrationPin(const sep::EndDevice &device,
                                          ProgramEventSink &sink) {
  if (!settings_.registrationPin) {
    return;
  }
  if (!device.RegistrationLink()) {
    sink.activity({.category = ActivityCategory::Connection,
                   .message = "The server gives this device no Registration "
                              "to check the PIN against, so it was not "
                              "checked."});
    return;
  }
  const auto registration =
      client_.get<sep::Registration>(device.RegistrationLink()->href());
  if (registration->pIN() != *settings_.registrationPin) {
    throw AdapterError::permanent(
        "the server's Registration PIN for this device does not match the "
        "connection's registration_pin");
  }
}

void CsipausAdapter::sendConnectionPoint(const sep::EndDevice &device) {
  if (!settings_.connectionPointId) {
    return;
  }
  if (!device.ConnectionPointLink()) {
    throw AdapterError::permanent(
        "the server gives this device no ConnectionPointLink to send the "
        "connection point to");
  }
  client_.put(device.ConnectionPointLink()->href(),
              ext::ConnectionPointType(*settings_.connectionPointId));
}

RegistrationInfo CsipausAdapter::registerConnection(ProgramEventSink &sink) {
  begin();
  forget();
  return establish(sink);
}

// Finds our EndDevice in the server's list, or creates it, and completes its
// registration. A server may lose an EndDevice, or remove it as a matter of
// course, and expects the client to create it again (CSIP-AUS v1.2
// explainer, sections 5.1.1 and 5.1.2.1).
RegistrationInfo CsipausAdapter::establish(ProgramEventSink &sink) {
  fetchDeviceCapability();
  const auto listHref = endDeviceListHref();
  auto found = findEndDevice(listHref);
  if (!found) {
    // In-band registration: the server creates our EndDevice.
    sep::EndDevice device(identity_.sfdi, epochNow());
    device.lFDI(hexBinary(identity_.lfdi));
    const auto href = client_.post(listHref, device);
    auto created = client_.get<sep::EndDevice>(href);
    if (!isOurs(*created)) {
      throw AdapterError::permanent("the EndDevice the server created at " +
                                    href + " is for another device");
    }
    found = Found{href, std::move(created)};
  }
  checkRegistrationPin(*found->device, sink);
  sendConnectionPoint(*found->device);

  endDeviceHref_ = found->href;
  endDevice_ = std::move(found->device);
  schedule_.fetched(*endDeviceHref_, endDevicePollRate_);
  return registrationInfo();
}

// The registration, with the poll interval of the device's controls.
RegistrationInfo CsipausAdapter::registrationInfo() {
  programLists(*endDevice_);
  RegistrationInfo info{identity_.lfdi, *endDeviceHref_, std::nullopt};
  if (controlPollRate_) {
    info.pollIntervalSeconds = static_cast<int>(controlPollRate_->count());
  }
  return info;
}

std::optional<RegistrationInfo>
CsipausAdapter::resume(const ConnectionState &state, ProgramEventSink &sink) {
  begin();
  forget();
  if (!state.registrationId) {
    return std::nullopt;
  }
  auto device = client_.find<sep::EndDevice>(*state.registrationId);
  if (!device || !isOurs(*device)) {
    // The server lost it, or the certificate changed: register again.
    return establish(sink);
  }
  // Not scheduled: its poll rate is its list's, which the first poll reads
  // before fetching it again.
  endDeviceHref_ = *state.registrationId;
  endDevice_ = std::move(device);
  return registrationInfo();
}

void CsipausAdapter::unregisterConnection(ProgramEventSink &) {
  // CSIP-AUS gives clients no way to remove an EndDevice; the utility does
  // that out of band. The worker only stops polling.
  begin();
  forget();
}

TestResult CsipausAdapter::test(ProgramEventSink &) {
  begin();
  fetchDeviceCapability();
  const auto found = findEndDevice(endDeviceListHref());
  TestResult result;
  result.clientId = identity_.lfdi;
  result.registered = found.has_value();
  result.description =
      found ? "The server has EndDevice " + found->href + " for " +
                  describeClient(identity_.lfdi)
            : "Reached the server. It has no EndDevice for " +
                  describeClient(identity_.lfdi) +
                  " yet; registering will create one.";
  return result;
}

PollResult CsipausAdapter::poll(ProgramEventSink &sink) {
  begin();
  if (!endDeviceHref_) {
    throw AdapterError::permanent("the connection is not registered");
  }
  const bool repoll = std::exchange(repollWanted_, false);
  if (repoll) {
    assignments_.clear();
    programLists_.clear();
    defaults_.clear();
    controls_.clear();
  }
  visited_.clear();
  try {
    refreshDeviceCapability();
    refreshEndDeviceList();
    if (auto cancelled = refreshEndDevice()) {
      return *cancelled;
    }
    refreshTime(sink);
    PollResult result;
    observed_.clear();
    auto description = walkPrograms(*endDevice_);
    reporter_.walked(setGradWInForce(observed_), serverNow());
    if (description != lastDescription_) {
      lastDescription_ = description;
      result.summary = std::move(description);
    }
    if (controlPollRate_) {
      result.pollIntervalSeconds = static_cast<int>(controlPollRate_->count());
    }
    walked_ = visited_;
    syncEvents(sink);
    lastWalk_ = observed_;
    updateLimits();
    return result;
  } catch (const AdapterError &error) {
    // Walk everything again next time, in case a link has gone stale.
    schedule_.clear();
    if (repoll && error.kind() != AdapterError::Kind::Stopped) {
      keepDefaultsOnly(sink);
    }
    throw;
  } catch (...) {
    schedule_.clear();
    throw;
  }
}

std::optional<PollResult> CsipausAdapter::refreshEndDevice() {
  visited_.insert(*endDeviceHref_);
  if (endDevice_ && !schedule_.due(*endDeviceHref_)) {
    return std::nullopt;
  }
  auto device = client_.find<sep::EndDevice>(*endDeviceHref_);
  if (!device || !isOurs(*device)) {
    // The runner registers again, which creates it again.
    PollResult result;
    result.outcome = PollOutcome::ReregistrationRequested;
    result.summary = "The server no longer has EndDevice " + *endDeviceHref_ +
                     " for " + describeClient(identity_.lfdi);
    forget();
    return result;
  }
  endDevice_ = std::move(device);
  schedule_.fetched(*endDeviceHref_, endDevicePollRate_);
  return std::nullopt;
}

template <typename T>
std::shared_ptr<T>
CsipausAdapter::fetch(const std::string &href, std::chrono::seconds pollRate,
                      std::map<std::string, std::shared_ptr<T>> &cache) {
  visited_.insert(href);
  const auto cached = cache.find(href);
  if (cached != cache.end() && !schedule_.due(href)) {
    return cached->second;
  }
  std::shared_ptr<T> document;
  if constexpr (PagedList<T>) {
    document = client_.getList<T>(href);
  } else {
    document = client_.get<T>(href);
  }
  schedule_.fetched(href, pollRate);
  cache[href] = document;
  return document;
}

// A list with a poll rate of its own.
template <typename T>
std::shared_ptr<T>
CsipausAdapter::fetchList(const std::string &href,
                          std::map<std::string, std::shared_ptr<T>> &cache) {
  visited_.insert(href);
  const auto cached = cache.find(href);
  if (cached != cache.end() && !schedule_.due(href)) {
    return cached->second;
  }
  std::shared_ptr<T> list = client_.getList<T>(href);
  schedule_.fetched(href, seconds(list->pollRate()));
  cache[href] = list;
  return list;
}

std::vector<CsipausAdapter::ProgramList>
CsipausAdapter::programLists(const sep::EndDevice &device) {
  std::vector<ProgramList> lists;
  std::optional<std::chrono::seconds> shortest;
  if (device.FunctionSetAssignmentsListLink()) {
    const auto assignments = fetchList(
        device.FunctionSetAssignmentsListLink()->href(), assignments_);
    for (const auto &assignment : assignments->FunctionSetAssignments()) {
      if (!assignment.DERProgramListLink()) {
        continue;
      }
      auto list =
          fetchList(assignment.DERProgramListLink()->href(), programLists_);
      const auto pollRate = seconds(list->pollRate());
      shortest = shortest ? std::min(*shortest, pollRate) : pollRate;
      lists.push_back({std::move(list), pollRate});
    }
  }
  controlPollRate_ = shortest;
  return lists;
}

std::string CsipausAdapter::walkPrograms(const sep::EndDevice &device) {
  if (!device.FunctionSetAssignmentsListLink()) {
    controlPollRate_.reset();
    return "The server assigns this device no function sets";
  }
  std::vector<std::string> programs;
  for (const auto &[list, pollRate] : programLists(device)) {
    for (const auto &program : list->DERProgram()) {
      programs.push_back(describeProgram(program, pollRate));
    }
  }
  if (programs.empty()) {
    return "The server assigns this device no DER programs";
  }
  return join(programs, "\n");
}

std::string CsipausAdapter::describeProgram(const sep::DERProgram &program,
                                            std::chrono::seconds pollRate) {
  const auto info = programInfo(program);
  std::string text = "DERProgram " + info.description.value_or(info.mrid) +
                     " (primacy " + std::to_string(info.primacy) + "): ";

  if (program.DefaultDERControlLink()) {
    const auto &href = program.DefaultDERControlLink()->href();
    const auto defaults = fetch(href, pollRate, defaults_);
    text += "default " + describeControlBase(defaults->DERControlBase());
    observed_.push_back(observeDefault(*defaults, href, info, serverNow()));
  } else {
    text += "no default control";
  }

  if (program.DERControlListLink()) {
    // DERControlLists have no poll rate of their own.
    const auto list =
        fetch(program.DERControlListLink()->href(), pollRate, controls_);
    for (const auto &control : list->DERControl()) {
      const auto status =
          static_cast<int>(control.EventStatus().currentStatus());
      text += "; " + describeStatus(status) + " " +
              describeWindow(Timestamp(std::chrono::seconds(
                                 static_cast<long long>(control.interval().start()))),
                             static_cast<int>(control.interval().duration())) +
              ": " + describeControlBase(control.DERControlBase());
      auto observed = observeControl(control, info);
      const auto window =
          effectiveWindow(observed.event, observed.detail, identity_.lfdi);
      observed.detail.effectiveStartAt = window.start;
      observed.detail.effectiveEndAt = window.end;
      observed_.push_back(std::move(observed));
    }
  }
  return text;
}

Timestamp CsipausAdapter::serverNow() const {
  return wallClock_() + clockOffset_;
}

void CsipausAdapter::refreshTime(ProgramEventSink &sink) {
  if (!dcap_->TimeLink()) {
    return;
  }
  const auto &href = dcap_->TimeLink()->href();
  visited_.insert(href);
  if (!schedule_.due(href)) {
    return;
  }
  const auto before = wallClock_();
  const auto time = client_.get<sep::Time>(href);
  const auto after = wallClock_();
  schedule_.fetched(href, seconds(time->pollRate()));

  // The server read its clock about halfway through the exchange.
  const auto ours = before + (after - before) / 2;
  const auto theirs = Timestamp(
      std::chrono::seconds(static_cast<long long>(time->currentTime())));
  clockOffset_ = theirs - ours;
  runnerStore_.recordClockOffset(static_cast<int>(clockOffset_.count()), after);

  const bool outOfTolerance = std::chrono::abs(clockOffset_) > clockTolerance;
  if (outOfTolerance != clockOutOfTolerance_) {
    clockOutOfTolerance_ = outOfTolerance;
    const auto amount = std::to_string(std::chrono::abs(clockOffset_).count());
    sink.activity(
        {.category = ActivityCategory::Connection,
         .message = outOfTolerance
                        ? "The server's clock is " + amount + " s " +
                              (clockOffset_.count() > 0 ? "ahead of" : "behind") +
                              " the worker's. Controls are timed by the "
                              "server's clock."
                        : "The server's clock agrees with the worker's again."});
  }
}

void CsipausAdapter::updateLimits() {
  auto limits = limitsInForce(observed_, identity_.lfdi, serverNow());
  if (storedLimits_ && *storedLimits_ == limits) {
    return;
  }
  store_.replaceLimits(limits);
  storedLimits_ = std::move(limits);
}

void CsipausAdapter::disconnected(ProgramEventSink &sink) {
  if (storedLimits_ && switchedOff(*storedLimits_, serverNow())) {
    sink.activity({.category = ActivityCategory::Connection,
                   .message = "The DER disconnected while a connect or "
                              "energize cap of 0 is in force: its controls "
                              "were kept."});
    return;
  }
  repollWanted_ = true;
  sink.activity({.category = ActivityCategory::Connection,
                 .message = "The DER disconnected from the grid: its programs "
                            "are polled again now, whatever their schedule, "
                            "and the caps in force are worked out afresh "
                            "from what they list (explainer 5.3.1)."});
}

void CsipausAdapter::keepDefaultsOnly(ProgramEventSink &sink) {
  if (!lastWalk_) {
    sink.activity({.category = ActivityCategory::Connection,
                   .message = "The programs could not be polled again after "
                              "the DER disconnected, and have not been "
                              "walked since the worker started: the caps in "
                              "force were left as they were."});
    return;
  }
  auto limits = limitsInForce(defaultsOf(*lastWalk_), identity_.lfdi,
                              serverNow());
  if (!storedLimits_ || *storedLimits_ != limits) {
    store_.replaceLimits(limits);
    storedLimits_ = std::move(limits);
  }
  sink.activity({.category = ActivityCategory::Connection,
                 .message = "The programs could not be polled again after the "
                            "DER disconnected: the caps in force are the "
                            "defaults' alone until they can be."});
}

void CsipausAdapter::syncEvents(ProgramEventSink &sink) {
  const auto at = serverNow();
  // A control once superseded stays so, whoever superseded it and whatever
  // the server lists it as since.
  std::map<std::string, std::optional<KnownEvent>> stored;
  for (auto &[event, detail, ramp] : observed_) {
    auto [entry, added] = stored.try_emplace(event.programEventId);
    if (added) {
      entry->second = sink.knownEvent(event.programEventId);
    }
    if (entry->second && entry->second->status == ProgramStatus::Superseded) {
      event.status = ProgramStatus::Superseded;
    }
  }
  const auto superseded = supersededControls(observed_, identity_.lfdi, at);

  std::set<std::string> listed;
  std::vector<std::string> failures;
  for (auto &[event, detail, ramp] : observed_) {
    if (!listed.insert(event.programEventId).second) {
      continue;
    }
    const auto observedStatus = event.status;
    auto known = stored.at(event.programEventId);
    if (known && isTerminal(known->status)) {
      recordDetail(*known, detail);
      continue;
    }
    if (!known) {
      if (isTerminal(observedStatus) ||
          hasEnded(event.startAt, event.durationSeconds, at)) {
        continue;
      }
      known = sink.eventReceived(event).event;
      recordDetail(*known, detail);
      if (asksFor(detail, requiresReceived)) {
        answer(*known, detail, EventAnswer::Received, sink, failures);
      }
    } else {
      if (detail.kind == CsipausEventKind::Default) {
        event.startAt = known->startAt;
      }
      if (event.signals != known->signals || event.startAt != known->startAt ||
          event.durationSeconds != known->durationSeconds) {
        // IEEE 2030.5 controls do not change once sent, but a default does.
        event.modificationNumber = known->modificationNumber + 1;
        event.status = known->status;
        known = sink.eventReceived(event).event;
      } else if (!known->respondedModification &&
                 asksFor(detail, requiresReceived)) {
        // The server was not told before.
        answer(*known, detail, EventAnswer::Received, sink, failures);
      }
      recordDetail(*known, detail);
    }

    // Superseded by this device's own resolution only once the server has
    // the received it asks for, so the 7 never comes first.
    const bool supersededHere =
        !isTerminal(observedStatus) &&
        superseded.count(event.programEventId) != 0 &&
        (!asksFor(detail, requiresReceived) ||
         known->respondedModification.has_value());
    const auto status =
        !isTerminal(observedStatus) &&
                hasEnded(known->startAt, known->durationSeconds, at)
            ? ProgramStatus::Completed
        : supersededHere ? ProgramStatus::Superseded
                         : observedStatus;
    if (status == known->status) {
      continue;
    }
    if ((status == ProgramStatus::Cancelled ||
         status == ProgramStatus::Superseded) &&
        asksFor(detail, requiresProgress) &&
        (answeredThisSecond(known->id) ||
         !answer(*known, detail,
                 status == ProgramStatus::Cancelled ? EventAnswer::Cancelled
                                                    : EventAnswer::Superseded,
                 sink, failures))) {
      // Left as it was, so the server is told on the next poll.
      continue;
    }
    sink.eventStatusChanged(*known, status);
    if (supersededHere && status == ProgramStatus::Superseded) {
      event.status = status;
      sink.activity({.category = ActivityCategory::Events,
                     .message = "This device superseded the control: controls "
                                "that outrank it hold every cap it sets until "
                                "it ends.",
                     .eventId = known->id});
    }
  }
  std::erase_if(recordedDetails_, [&listed](const auto &recorded) {
    return listed.count(recorded.first) == 0;
  });

  answerProgress(sink, failures);

  // The server lists the controls it still has, so an open event it no
  // longer lists has ended, or been withdrawn.
  for (const auto &open : sink.openEvents()) {
    if (listed.count(open.programEventId) == 0) {
      sink.eventEnded(open,
                      hasEnded(open.startAt, open.durationSeconds, at)
                          ? ProgramStatus::Completed
                          : ProgramStatus::Cancelled);
    }
  }

  if (!failures.empty()) {
    throw AdapterError::transient("the server was not told about its controls: " +
                                  join(failures, "; "));
  }
}

void CsipausAdapter::recordDetail(const KnownEvent &event,
                                  const CsipausEventDetail &detail) {
  const auto recorded = recordedDetails_.find(event.programEventId);
  if (recorded != recordedDetails_.end() && recorded->second == detail) {
    return;
  }
  store_.recordEventDetail(event.id, detail);
  recordedDetails_[event.programEventId] = detail;
}

// "started" and "completed" say what the site did, so they follow the
// planner's lifecycle rather than the clock: the planner moves an event to
// control once the site applies the caps in force while it is in force.
void CsipausAdapter::answerProgress(ProgramEventSink &sink,
                                    std::vector<std::string> &failures) {
  if (progressRetryAt_ && serverNow() < *progressRetryAt_) {
    return;
  }
  const auto before = failures.size();
  for (const auto &due : store_.progressDue()) {
    if (!answeredThisSecond(due.event.id)) {
      answer(due.event, due.detail, due.answers.front(), sink, failures);
    }
  }
  progressRetryAt_ = failures.size() > before
                         ? std::optional<Timestamp>(serverNow() + progressRetry)
                         : std::nullopt;
}

bool CsipausAdapter::answeredThisSecond(EventRowId eventId) {
  const auto at = serverNow();
  std::erase_if(lastAnswered_,
                [at](const auto &answered) { return answered.second < at; });
  return lastAnswered_.count(eventId) != 0;
}

bool CsipausAdapter::answer(const KnownEvent &event,
                            const CsipausEventDetail &detail,
                            EventAnswer answer, ProgramEventSink &sink,
                            std::vector<std::string> &failures) {
  const auto at = serverNow();
  sep::DERControlResponse response(sep::HexBinary160(hexBinary(identity_.lfdi)),
                                   sep::mRIDType(hexBinary(detail.mrid)));
  response.createdDateTime(
      sep::TimeType(static_cast<long long>(at.time_since_epoch().count())));
  response.status(sep::UInt8(static_cast<unsigned char>(responseStatus(answer))));
  try {
    client_.send(*detail.replyTo, response);
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Stopped) {
      throw;
    }
    sink.responseSent(event, answer, event.modificationNumber, false,
                      error.what());
    failures.push_back(error.what());
    return false;
  }
  lastAnswered_[event.id] = at;
  sink.responseSent(event, answer, event.modificationNumber, true, "");
  return true;
}

void CsipausAdapter::respond(const KnownEvent &, Opt, ProgramEventSink &) {
  throw AdapterError::permanent(
      "CSIP-AUS controls cannot be opted in or out of");
}

// Answers progress, then reports; a failure of either leaves the other to
// run, and the failures are thrown together. A disconnection the Reporter
// read is acted on whether or not it could send everything.
void CsipausAdapter::runDueWork(ProgramEventSink &sink) {
  if (!endDevice_ || !dcap_) {
    return;
  }
  std::vector<std::string> failures;
  std::vector<std::string> unanswered;
  answerProgress(sink, unanswered);
  if (!unanswered.empty()) {
    failures.push_back("the server was not told about its controls: " +
                       join(unanswered, "; "));
  }
  try {
    reporter_.run(*endDevice_, *dcap_, sink);
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Stopped) {
      throw;
    }
    failures.push_back(error.what());
  }
  if (reporter_.takeDisconnected()) {
    disconnected(sink);
  }
  if (!failures.empty()) {
    throw AdapterError::transient(join(failures, "; "));
  }
}

std::optional<std::chrono::milliseconds> CsipausAdapter::dueWorkIn() const {
  return endDevice_ && dcap_ ? reporter_.dueIn() : std::nullopt;
}

bool CsipausAdapter::pollWanted() const { return repollWanted_; }

bool CsipausAdapter::jittersPolls() const { return false; }

std::optional<std::chrono::milliseconds>
CsipausAdapter::retryDelay(int failures,
                           std::chrono::milliseconds pollInterval) {
  return csipaus::retryDelay(failures, pollInterval, random_);
}

void CsipausAdapter::setParticipation(Participation participation) {
  participation_ = participation;
}

std::string CsipausAdapter::describeParticipation(Participation participation,
                                                  Opt) const {
  return participation == Participation::Paused
             ? "Participation paused. CSIP-AUS controls cannot be refused, "
               "so the server is not told and its controls still apply."
             : "Participation resumed.";
}

// Polled again when the first of the resources the last walk visited is due.
// Only those: a resource the server no longer links to is never due.
std::optional<std::chrono::seconds>
CsipausAdapter::requestedPollInterval() const {
  std::optional<Clock::duration> soonest;
  for (const auto &href : walked_) {
    if (const auto in = schedule_.untilDue(href)) {
      soonest = soonest ? std::min(*soonest, *in) : *in;
    }
  }
  if (!soonest) {
    return std::nullopt;
  }
  return std::max(std::chrono::ceil<std::chrono::seconds>(*soonest),
                  std::chrono::seconds(1));
}

std::string CsipausAdapter::describeClient(const std::string &clientId) const {
  // Grouped in fours, as IEEE 2030.5 displays an LFDI.
  std::string grouped;
  for (std::size_t i = 0; i < clientId.size(); i += 4) {
    grouped += (grouped.empty() ? "" : "-") + clientId.substr(i, 4);
  }
  return "LFDI " + grouped;
}

AdapterFactory makeCsipausFactory(const SecretOpener *opener,
                                  CsipausOptions options) {
  return [opener, options](const AdapterContext &context)
             -> std::unique_ptr<Adapter> {
    auto settings = csipausStore(context.store).loadCsipausSettings();

    const auto credential = context.store.loadClientCredential();
    if (!credential) {
      throw AdapterError::permanent(
          "a CSIP-AUS connection needs a client certificate "
          "(client_credential_id): the server knows the device by its LFDI");
    }
    DeviceIdentity identity;
    try {
      identity = identityFromCertificate(credential->certPem);
    } catch (const std::invalid_argument &error) {
      throw AdapterError::permanent(
          std::string("cannot read the client certificate: ") + error.what());
    }
    auto opened = openClientCredential(*credential, opener);

    auto &csipaus = csipausStore(context.store);
    ReportingSetup reporting;
    reporting.pen = options.pen;
    try {
      reporting.der = csipaus.loadDer();
      const auto rows = csipaus.loadReadings();
      const bool bindsState = reporting.der &&
                              (reporting.der->connectedPointId.has_value() ||
                               reporting.der->operatingPointId.has_value());
      if (!rows.empty() || bindsState) {
        const auto points = context.store.loadTelemetryPoints();
        reporting.readings = bindReadings(rows, points);
        if (bindsState) {
          reporting.state = bindState(*reporting.der, points);
        }
      }
    } catch (const std::invalid_argument &error) {
      throw AdapterError::permanent(error.what());
    }
    const bool readsState =
        reporting.state.connected.has_value() ||
        reporting.state.operating.has_value();
    if (const auto data = context.store.loadDataCredential()) {
      auto openedData = openDataCredential(*data, opener);
      reporting.reader = std::make_shared<datum::SolarNetworkClient>(
          datum::SolarNetworkCredential{std::move(openedData.host),
                                        std::move(openedData.token),
                                        std::move(openedData.secret)},
          options.connectTimeout, options.requestTimeout, &context.abort);
    } else if (!reporting.readings.empty() || readsState) {
      throw AdapterError::permanent(
          std::string(readsState ? "the DER's state is bound"
                                 : "readings are configured") +
          " but the connection has no SolarNetwork credential to read their "
          "values with");
    }

    http::TlsSettings tls;
    tls.clientCertPem = std::move(opened.certPem);
    tls.clientKeyPem = std::move(opened.keyPem);
    tls.caCertPem = settings.caCertPem.value_or("");
    tls.verifyServer = settings.verifyServerCertificate;
    tls.cipherList = cipherList;
    tls.sslVersion = CURL_SSLVERSION_TLSv1_2;

    auto http = std::make_unique<http::CurlHttpClient>(
        std::move(tls), options.connectTimeout, options.requestTimeout,
        &context.abort);
    return std::make_unique<CsipausAdapter>(
        context.config, std::move(settings), std::move(identity),
        std::move(http), context.store, nullptr, nullptr,
        std::move(reporting));
  };
}

} // namespace grid_services::csipaus
