#include "protocols/csipaus/reporter.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "core/errors.hpp"

namespace grid_services::csipaus {

namespace {

std::chrono::seconds seconds(unsigned long value) {
  return std::chrono::seconds(static_cast<long>(value));
}

std::string join(const std::vector<std::string> &parts) {
  std::string text;
  for (const auto &part : parts) {
    text += (text.empty() ? "" : "; ") + part;
  }
  return text;
}

// A failed send is retried no sooner than this.
constexpr std::chrono::seconds minimumRetry{60};
// Due work is run again no sooner than this, whatever a server's rates.
constexpr std::chrono::milliseconds minimumWake{1000};

} // namespace

std::chrono::seconds windowPostDelay(std::chrono::seconds postRate) {
  return std::min(readingSettle, postRate / 4);
}

Reporter::Reporter(ReportingSetup setup, SepClient &client, std::string lfdi,
                   ResourceSchedule &schedule,
                   std::function<Timestamp()> wallClock,
                   const std::chrono::seconds &clockOffset)
    : setup_(std::move(setup)), client_(client), lfdi_(std::move(lfdi)),
      schedule_(schedule), wallClock_(std::move(wallClock)),
      clockOffset_(clockOffset) {
  for (const auto mirror : {Mirror::Site, Mirror::Der}) {
    MirrorState state;
    state.mirror = mirror;
    state.mrid = mirrorMrid(lfdi_, mirror, setup_.pen);
    for (const auto &reading : setup_.readings) {
      if (reading.mirror != mirror) {
        continue;
      }
      state.readings.push_back(reading);
      const auto &source = reading.point.source;
      auto stream = std::find_if(
          state.streams.begin(), state.streams.end(), [&](const auto &known) {
            return known.nodeId == source.nodeId &&
                   known.sourceId == source.sourceId;
          });
      if (stream == state.streams.end()) {
        state.streams.push_back({source.nodeId, source.sourceId, {}, {}, {}});
        stream = std::prev(state.streams.end());
      }
      stream->readings.push_back(reading);
    }
    if (!state.readings.empty()) {
      mirrors_.push_back(std::move(state));
    }
  }
  reset();
}

bool Reporter::reports() const {
  return setup_.der.has_value() || !mirrors_.empty();
}

void Reporter::reset() {
  for (auto &mirror : mirrors_) {
    mirror.href.reset();
    mirror.defined = false;
    mirror.postRate = defaultPostRate;
    for (auto &stream : mirror.streams) {
      stream.next.reset();
      stream.retryAt.reset();
    }
  }
  mirrorListHref_.reset();
  derListFailed_ = false;
  mirrorListFailed_ = false;
  derListHref_.reset();
  capabilityHref_.reset();
  settingsHref_.reset();
  statusHref_.reset();
  capabilityDescribed_ = false;
  capabilityRetryAt_.reset();
  walked_ = false;
  setGradW_ = setup_.der ? setup_.der->setGradW : 0;
  settingsUpdatedAt_ = setup_.der ? setup_.der->updatedAt : Timestamp{};
  settingsTaken_.reset();
  settingsRetryAt_.reset();
  lastStatus_.reset();
  statusDueAt_.reset();
  statusFailed_ = false;
  lastConnected_.reset();
  disconnected_ = false;
}

bool Reporter::takeDisconnected() { return std::exchange(disconnected_, false); }

void Reporter::walked(std::optional<int> setGradW, Timestamp at) {
  walked_ = true;
  if (!setup_.der) {
    return;
  }
  const auto inForce = setGradW.value_or(setup_.der->setGradW);
  if (inForce != setGradW_) {
    setGradW_ = inForce;
    settingsUpdatedAt_ = at;
  }
}

bool Reporter::settingsDue() const {
  return walked_ && settingsTaken_ != setGradW_;
}

Timestamp Reporter::serverNow() const { return wallClock_() + clockOffset_; }

std::optional<std::chrono::milliseconds> Reporter::dueIn() const {
  using std::chrono::milliseconds;
  if (!reports()) {
    return std::nullopt;
  }
  const auto now = serverNow();
  std::optional<milliseconds> soonest;
  const auto consider = [&soonest](milliseconds in) {
    const auto clamped = std::max(in, minimumWake);
    soonest = soonest ? std::min(*soonest, clamped) : clamped;
  };
  const auto at = [&](Timestamp due) {
    consider(std::chrono::duration_cast<milliseconds>(due - now));
  };
  // A list that could not be fetched is tried again on the runner's next
  // pass, not at once.
  const auto list = [&](const std::optional<std::string> &href, bool failed) {
    if (const auto in = href ? schedule_.untilDue(*href) : std::nullopt;
        in && !failed) {
      consider(std::chrono::ceil<milliseconds>(*in));
    }
  };

  list(derListHref_, derListFailed_);
  if (!mirrors_.empty()) {
    list(mirrorListHref_, mirrorListFailed_);
  }
  if (setup_.der && capabilityHref_ && !capabilityDescribed_ &&
      capabilityRetryAt_) {
    at(*capabilityRetryAt_);
  }
  if (setup_.der && settingsHref_ && settingsDue() && settingsRetryAt_) {
    at(*settingsRetryAt_);
  }
  if (statusHref_ && statusDueAt_) {
    at(*statusDueAt_);
  }
  for (const auto &mirror : mirrors_) {
    if (!mirror.href) {
      continue;
    }
    for (const auto &stream : mirror.streams) {
      const auto posted =
          stream.next.value_or(windowStart(now, mirror.postRate)) +
          mirror.postRate + windowPostDelay(mirror.postRate);
      at(stream.retryAt ? std::max(*stream.retryAt, posted) : posted);
    }
  }
  return soonest;
}

void Reporter::run(const sep::EndDevice &device,
                   const sep::DeviceCapability &dcap, ProgramEventSink &sink) {
  if (!reports()) {
    return;
  }
  std::vector<std::string> failures;
  // Whether the step succeeded.
  const auto attempt = [&failures](const std::function<void()> &step) {
    try {
      step();
      return true;
    } catch (const AdapterError &error) {
      if (error.kind() == AdapterError::Kind::Stopped) {
        throw;
      }
      failures.push_back(error.what());
      return false;
    }
  };

  derListFailed_ = !attempt([&] { refreshDer(device); });
  if (setup_.der) {
    describeDer(sink, failures);
  }
  if (!mirrors_.empty()) {
    mirrorListFailed_ = !attempt([&] { refreshMirrors(device, dcap, sink); });
    for (auto &mirror : mirrors_) {
      for (auto &stream : mirror.streams) {
        // A server that has removed the mirror loses it for the rest.
        if (mirror.href) {
          postDue(mirror, stream, sink, failures);
        }
      }
    }
  }
  reportStatus(sink, failures);

  if (!failures.empty()) {
    throw AdapterError::transient(join(failures));
  }
}

void Reporter::refreshDer(const sep::EndDevice &device) {
  if (!device.DERListLink()) {
    derListHref_.reset();
    capabilityHref_.reset();
    settingsHref_.reset();
    statusHref_.reset();
    return;
  }
  const auto &href = device.DERListLink()->href();
  if (derListHref_ == href && !schedule_.due(href)) {
    return;
  }
  const auto list = client_.getList<sep::DERList>(href);
  derListPollRate_ = seconds(list->pollRate());
  schedule_.fetched(href, derListPollRate_);
  derListHref_ = href;

  // CSIP-AUS has a single DER per EndDevice.
  std::optional<std::string> capability;
  std::optional<std::string> settings;
  std::optional<std::string> status;
  if (!list->DER().empty()) {
    const auto &der = list->DER().front();
    if (der.DERCapabilityLink()) {
      capability = der.DERCapabilityLink()->href();
    }
    if (der.DERSettingsLink()) {
      settings = der.DERSettingsLink()->href();
    }
    if (der.DERStatusLink()) {
      status = der.DERStatusLink()->href();
    }
  }
  if (capability != capabilityHref_ || settings != settingsHref_) {
    // A DER the server has given new links is described again.
    capabilityDescribed_ = false;
    settingsTaken_.reset();
  }
  capabilityHref_ = std::move(capability);
  settingsHref_ = std::move(settings);
  statusHref_ = std::move(status);
}

// DERCapability once; DERSettings once the programs have been walked, and
// again when its setGradW changes. Each refused is sent again after
// describeRetry.
void Reporter::describeDer(ProgramEventSink &sink,
                           std::vector<std::string> &failures) {
  const auto at = serverNow();
  const auto retryDue = [at](const std::optional<Timestamp> &retryAt) {
    return !retryAt || at >= *retryAt;
  };
  const auto retryAfter = [at](bool ok) {
    return ok ? std::nullopt : std::optional<Timestamp>(at + describeRetry);
  };
  if (capabilityHref_ && !capabilityDescribed_ && retryDue(capabilityRetryAt_)) {
    ReportDelivery delivery;
    delivery.report = "der_capability";
    delivery.payload = capabilityPayload(*setup_.der, at);
    const auto href = *capabilityHref_;
    capabilityDescribed_ = deliver(
        std::move(delivery), "DERCapability",
        [&] { client_.put(href, capabilityDocument(*setup_.der)); }, sink,
        failures);
    capabilityRetryAt_ = retryAfter(capabilityDescribed_);
  }
  if (settingsHref_ && settingsDue() && retryDue(settingsRetryAt_)) {
    const auto setGradW = setGradW_;
    const auto updatedAt = settingsUpdatedAt_;
    ReportDelivery delivery;
    delivery.report = "der_settings";
    delivery.payload = settingsPayload(*setup_.der, setGradW, updatedAt, at);
    const auto href = *settingsHref_;
    const bool ok = deliver(
        std::move(delivery), "DERSettings",
        [&] {
          client_.put(href, settingsDocument(*setup_.der, setGradW, updatedAt));
        },
        sink, failures);
    if (ok) {
      settingsTaken_ = setGradW;
    }
    settingsRetryAt_ = retryAfter(ok);
  }
}

void Reporter::refreshMirrors(const sep::EndDevice &device,
                              const sep::DeviceCapability &dcap,
                              ProgramEventSink &sink) {
  if (!dcap.MirrorUsagePointListLink()) {
    throw AdapterError::transient(
        "the server's DeviceCapability has no MirrorUsagePointListLink to "
        "post readings to");
  }
  const auto &listHref = dcap.MirrorUsagePointListLink()->href();
  if (mirrorListHref_ != listHref) {
    // Found again in the server's new list.
    for (auto &mirror : mirrors_) {
      mirror.href.reset();
    }
    mirrorListHref_ = listHref;
  }
  const bool undefined =
      std::any_of(mirrors_.begin(), mirrors_.end(),
                  [](const MirrorState &mirror) { return !mirror.href; });
  if (!undefined && !schedule_.due(*mirrorListHref_)) {
    return;
  }

  const auto list = client_.getList<sep::MirrorUsagePointList>(*mirrorListHref_);
  schedule_.fetched(*mirrorListHref_, seconds(list->pollRate()));
  // Readings are posted at the server's rate for each mirror usage point,
  // or else the EndDevice's.
  const auto fallback =
      device.postRate() ? seconds(*device.postRate()) : defaultPostRate;

  for (auto &mirror : mirrors_) {
    const auto found =
        std::find_if(list->MirrorUsagePoint().begin(),
                     list->MirrorUsagePoint().end(), [&](const auto &point) {
                       return hexString(point.mRID()) == mirror.mrid &&
                              point.href().present();
                     });
    std::chrono::seconds postRate = fallback;
    if (found == list->MirrorUsagePoint().end() || !mirror.defined) {
      // Missing, or found again after a restart: posting it defines its
      // readings afresh, and finds the same one by its mRID.
      const auto href = client_.post(
          *mirrorListHref_,
          mirrorUsagePoint(mirror.mirror, mirror.readings, lfdi_, setup_.pen));
      const auto created = client_.get<sep::MirrorUsagePoint>(href);
      if (created->postRate()) {
        postRate = seconds(*created->postRate());
      }
      if (found == list->MirrorUsagePoint().end()) {
        sink.activity({.category = ActivityCategory::Reports,
                       .message = "Created the " + describeMirror(mirror.mirror) +
                                  " mirror usage point at " + href +
                                  ", posting readings every " +
                                  std::to_string(postRate.count()) + " s"});
      }
      mirror.href = href;
      mirror.defined = true;
    } else {
      mirror.href = *found->href();
      if (found->postRate()) {
        postRate = seconds(*found->postRate());
      }
    }
    if (postRate.count() <= 0) {
      postRate = defaultPostRate;
    }
    if (postRate != mirror.postRate) {
      // Windows are aligned to the new rate from the one under way.
      mirror.postRate = postRate;
      for (auto &stream : mirror.streams) {
        stream.next.reset();
      }
    }
  }
}

void Reporter::postDue(MirrorState &mirror, StreamState &stream,
                       ProgramEventSink &sink,
                       std::vector<std::string> &failures) {
  const auto now = serverNow();
  if (stream.retryAt && now < *stream.retryAt) {
    return;
  }
  const auto current = windowStart(now, mirror.postRate);
  if (!stream.next) {
    stream.next = current;
  }
  const auto oldest = current - mirror.postRate * readingBacklog;
  if (*stream.next < oldest) {
    stream.next = oldest;
  }
  while (*stream.next + mirror.postRate + windowPostDelay(mirror.postRate) <=
         now) {
    switch (postWindow(mirror, stream, *stream.next, *stream.next <= oldest,
                       sink, failures)) {
    case WindowOutcome::Done:
      break;
    case WindowOutcome::Waiting:
      stream.retryAt = now + readingRecheck;
      return;
    case WindowOutcome::Refused:
      stream.retryAt = now + std::max(mirror.postRate, minimumRetry);
      return;
    }
    *stream.next += mirror.postRate;
  }
  stream.retryAt.reset();
}

Reporter::WindowOutcome
Reporter::postWindow(MirrorState &mirror, StreamState &stream, Timestamp start,
                     bool last, ProgramEventSink &sink,
                     std::vector<std::string> &failures) {
  const auto end = start + mirror.postRate;
  // Windows are the server's; datums are timed by the site's clock, which
  // is taken to be the worker's.
  const auto from = start - clockOffset_;
  const auto to = end - clockOffset_;
  // Reaching back a window lets a difference find the reading before it.
  const auto readFrom = from - mirror.postRate;
  const auto readTo = to + std::chrono::seconds(1);

  ReportDelivery delivery;
  delivery.report = mirror.mirror == Mirror::Site ? "site_readings"
                                                   : "der_readings";
  delivery.intervalStart = start;
  delivery.intervalEnd = end;

  // The stream's datums are all in once SolarNetwork has one at or after
  // the window's end, as SolarNode uploads them in order.
  std::optional<std::string> notArrived;
  if (setup_.reader) {
    const auto request =
        setup_.reader->describeMostRecent(stream.nodeId, stream.sourceId);
    try {
      const auto latest =
          setup_.reader->mostRecent(stream.nodeId, stream.sourceId);
      if (!latest) {
        notArrived = "SolarNetwork has no datum of " + stream.sourceId;
      } else if (latest->at < to) {
        notArrived = "SolarNetwork's latest datum of " + stream.sourceId +
                     " is from " + isoTime(latest->at);
      }
      if (notArrived && !last) {
        return WindowOutcome::Waiting;
      }
      delivery.reads.push_back(readRecord(stream.nodeId, stream.sourceId,
                                          "most_recent", request,
                                          latest ? 1u : 0u, std::nullopt));
    } catch (const datum::DatumError &error) {
      if (!last) {
        return WindowOutcome::Waiting;
      }
      notArrived = "SolarNetwork could not be read: " + std::string(error.what());
      delivery.reads.push_back(readRecord(stream.nodeId, stream.sourceId,
                                          "most_recent", request, std::nullopt,
                                          std::string(error.what())));
    }
  }

  if (notArrived) {
    for (const auto &reading : stream.readings) {
      delivery.payload.push_back(
          {{"rid", readingId(reading)},
           {"point_id", reading.point.pointId},
           {"dtstart", isoTime(start)},
           {"duration_seconds", mirror.postRate.count()},
           {"value", nullptr},
           {"quality", "incomplete"},
           {"source", sourceRecord(reading.point.source)}});
    }
    delivery.ok = false;
    delivery.message =
        "Nothing was posted: the window's datums had not all arrived when it "
        "was the oldest of the last " +
        std::to_string(readingBacklog) + " windows (" + *notArrived + ").";
    sink.reportDelivered(delivery);
    return WindowOutcome::Done;
  }

  std::optional<std::vector<datum::Datum>> datums;
  if (setup_.reader) {
    const auto request = setup_.reader->describeList(
        stream.nodeId, stream.sourceId, readFrom, readTo);
    try {
      datums = setup_.reader->list(stream.nodeId, stream.sourceId, readFrom,
                                   readTo);
      delivery.reads.push_back(readRecord(stream.nodeId, stream.sourceId,
                                          "list", request, datums->size(),
                                          std::nullopt,
                                          std::make_pair(from, to)));
    } catch (const datum::DatumError &error) {
      delivery.reads.push_back(readRecord(stream.nodeId, stream.sourceId,
                                          "list", request, std::nullopt,
                                          std::string(error.what()),
                                          std::make_pair(from, to)));
    }
  }

  std::vector<std::pair<BoundReading, datum::Sample>> values;
  bool anySent = false;
  bool anyOutOfRange = false;
  for (const auto &reading : stream.readings) {
    const datum::Sample sample =
        datums ? datum::reduceWindow(*datums, reading.point.source, from, to)
               : datum::Sample{std::nullopt, datum::Quality::Unreadable};
    const bool good =
        sample.value.has_value() && sample.quality == datum::Quality::Good;
    const auto sent =
        good ? sentValue(reading.powerOfTen, *sample.value) : std::nullopt;
    anySent = anySent || sent.has_value();
    anyOutOfRange = anyOutOfRange || (good && !sent);
    // A value that does not fit is kept as the window had it.
    delivery.payload.push_back(
        {{"rid", readingId(reading)},
         {"point_id", reading.point.pointId},
         {"dtstart", isoTime(start)},
         {"duration_seconds", mirror.postRate.count()},
         {"value", sent ? nlohmann::json(receivedValue(reading.powerOfTen, *sent))
                   : good ? nlohmann::json(*sample.value)
                          : nlohmann::json(nullptr)},
         {"quality", good && !sent ? std::string("out_of_range")
                                   : toString(sample.quality)},
         {"source", sourceRecord(reading.point.source)}});
    values.emplace_back(reading, sample);
  }

  if (!anySent) {
    // Nothing to tell the server; it is not sent placeholders.
    delivery.ok = false;
    delivery.message =
        anyOutOfRange
            ? "Nothing was posted: no reading had a value in the window that "
              "fits an Int16 at its power of ten."
            : "Nothing was posted: no reading had a value in the window.";
    sink.reportDelivered(delivery);
    return WindowOutcome::Done;
  }

  const auto href = *mirror.href;
  const auto document =
      readingsDocument(values, lfdi_, setup_.pen, start, mirror.postRate);
  const bool taken = deliver(
      std::move(delivery), describeMirror(mirror.mirror) + " readings",
      [&] {
        try {
          client_.send(href, document);
        } catch (const NotFound &) {
          // The server has removed it, as after a long silence: it is found
          // or created again.
          mirror.href.reset();
          mirror.defined = false;
          throw;
        }
      },
      sink, failures);
  return taken ? WindowOutcome::Done : WindowOutcome::Refused;
}

void Reporter::reportStatus(ProgramEventSink &sink,
                            std::vector<std::string> &failures) {
  if (!statusHref_) {
    return;
  }
  const auto now = serverNow();
  const bool due = !statusDueAt_ || now >= *statusDueAt_;
  if (const auto unbound = unboundStatus()) {
    if (due) {
      ReportDelivery delivery;
      delivery.report = "der_status";
      delivery.ok = false;
      delivery.message = *unbound;
      sink.reportDelivered(delivery);
      statusDueAt_ = now + std::max(derListPollRate_, minimumRetry);
    }
    return;
  }
  if (!due && statusFailed_) {
    return;
  }
  ReportDelivery delivery;
  delivery.report = "der_status";
  const auto [connectedState, operatingState] = readState(delivery);
  noteConnected(connectedState.sample);
  const auto state = derState(connectedState.sample, operatingState.sample);
  if (!due && lastStatus_ == state) {
    return;
  }
  delivery.payload =
      statusPayload(state, connectedState, operatingState, now);
  const auto href = *statusHref_;
  const bool ok = deliver(
      std::move(delivery), "DERStatus",
      [&] { client_.put(href, statusDocument(state, now)); }, sink, failures);
  statusFailed_ = !ok;
  if (ok) {
    lastStatus_ = state;
  }
  statusDueAt_ =
      now + (ok ? derListPollRate_ : std::max(derListPollRate_, minimumRetry));
}

std::optional<std::string> Reporter::unboundStatus() const {
  if (!setup_.der) {
    return "No DERStatus was sent: the DER is not described (csipaus_der).";
  }
  std::vector<std::string> unbound;
  if (!setup_.state.connected) {
    unbound.push_back("connected");
  }
  if (!setup_.state.operating) {
    unbound.push_back("operating");
  }
  if (unbound.empty()) {
    return std::nullopt;
  }
  return "No DERStatus was sent: the DER's " +
         (unbound.size() == 2 ? unbound[0] + " and " + unbound[1]
                              : unbound[0]) +
         " state is not bound to a telemetry point.";
}

void Reporter::noteConnected(const datum::Sample &sample) {
  const auto value = stateSample(sample).value;
  if (!value) {
    return;
  }
  if (lastConnected_ == 1.0 && *value == 0.0) {
    disconnected_ = true;
  }
  lastConnected_ = value;
}

std::pair<StateReading, StateReading>
Reporter::readState(ReportDelivery &delivery) {
  using Stream = std::pair<datum::NodeId, std::string>;
  std::map<Stream, std::optional<datum::Datum>> latest;
  std::set<Stream> unreadable;
  // Datums are timed by the site's clock, which is taken to be the
  // worker's.
  const auto now = wallClock_();
  const auto read = [&](const TelemetryPoint &point) -> StateReading {
    const Stream stream{point.source.nodeId, point.source.sourceId};
    if (latest.count(stream) == 0 && unreadable.count(stream) == 0) {
      if (!setup_.reader) {
        unreadable.insert(stream);
      } else {
        const auto request =
            setup_.reader->describeMostRecent(stream.first, stream.second);
        try {
          auto found = setup_.reader->mostRecent(stream.first, stream.second);
          delivery.reads.push_back(readRecord(stream.first, stream.second,
                                              "most_recent", request,
                                              found ? 1u : 0u, std::nullopt));
          latest[stream] = std::move(found);
        } catch (const datum::DatumError &error) {
          unreadable.insert(stream);
          delivery.reads.push_back(readRecord(
              stream.first, stream.second, "most_recent", request,
              std::nullopt, std::string(error.what())));
        }
      }
    }
    const auto found = latest.find(stream);
    return {point,
            stateSample(found == latest.end()
                            ? datum::Sample{std::nullopt,
                                            datum::Quality::Unreadable}
                            : datum::latestSample(found->second, point.source,
                                                  now))};
  };
  auto connectedState = read(*setup_.state.connected);
  auto operatingState = read(*setup_.state.operating);
  return {std::move(connectedState), std::move(operatingState)};
}

template <typename Send>
bool Reporter::deliver(ReportDelivery delivery, const std::string &what,
                       Send send, ProgramEventSink &sink,
                       std::vector<std::string> &failures) {
  // Only this document's exchange is kept with it.
  client_.takeExchange();
  try {
    send();
    delivery.ok = true;
  } catch (const AdapterError &error) {
    if (error.kind() == AdapterError::Kind::Stopped) {
      throw;
    }
    delivery.ok = false;
    delivery.message = error.what();
    failures.push_back(what + " was not taken: " + error.what());
  }
  delivery.exchange = client_.takeExchange();
  sink.reportDelivered(delivery);
  return delivery.ok;
}

} // namespace grid_services::csipaus
