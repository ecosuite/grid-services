#include "protocols/csipaus/limits.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <set>

namespace grid_services::csipaus {

namespace {

const std::vector<std::string> capNames = {
    "export", "import", "generation", "load", "max_output", "energize",
    "connect"};

// FNV-1a: a hash that stays the same from build to build, so a device keeps
// its offsets across restarts.
std::uint64_t stableHash(const std::string &text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::optional<double> capValue(const NormalizedEvent &event,
                               const std::string &limit) {
  for (const auto &signal : event.signals) {
    if (signal.type == "limit" && signal.name == limit &&
        !signal.intervals.empty()) {
      return signal.intervals.front().value;
    }
  }
  return std::nullopt;
}

struct Control {
  Timestamp start;
  Timestamp end;
  double value = 0;
  int primacy = 0;
  Timestamp created;
  std::string eventId;
  std::optional<int> rampTms;
};

struct Default {
  Timestamp since;
  double value = 0;
  int primacy = 0;
  std::string eventId;
};

// Whether `a` takes precedence over `b`.
bool outranks(const Control &a, const Control &b) {
  if (a.primacy != b.primacy) {
    return a.primacy < b.primacy;
  }
  return a.created > b.created;
}

// Adds `segment`, joining it to the last when it carries on the same value
// from the same event, with the same ramp.
void append(std::vector<LimitSegment> &segments, LimitSegment segment) {
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.limit == segment.limit && last.end == segment.start &&
        last.value == segment.value && last.source == segment.source &&
        last.programEventId == segment.programEventId &&
        last.rampTms == segment.rampTms && last.setGradW == segment.setGradW) {
      last.end = segment.end;
      return;
    }
  }
  segments.push_back(std::move(segment));
}

void addCap(std::vector<LimitSegment> &segments, const std::string &limit,
            const std::vector<ObservedEvent> &observed,
            const std::optional<int> &setGradW, const std::string &lfdi,
            Timestamp at) {
  std::vector<Control> controls;
  std::optional<Default> standing;
  for (const auto &[event, detail, ramp] : observed) {
    const auto value = capValue(event, limit);
    if (!value) {
      continue;
    }
    if (detail.kind == CsipausEventKind::Default) {
      if (!standing || detail.primacy < standing->primacy) {
        standing =
            Default{event.startAt, *value, detail.primacy, event.programEventId};
      }
      continue;
    }
    if (isTerminal(event.status)) {
      continue;
    }
    const auto [start, end] = effectiveWindow(event, detail, lfdi);
    if (end <= at || end <= start) {
      continue;
    }
    controls.push_back({start, end, *value, detail.primacy,
                        detail.creationTime.value_or(Timestamp{}),
                        event.programEventId, ramp.rampTms});
  }

  std::set<Timestamp> points;
  for (const auto &control : controls) {
    points.insert(control.start);
    points.insert(control.end);
  }
  if (standing) {
    points.insert(standing->since);
  }
  if (points.empty()) {
    return;
  }

  for (auto from = points.begin(), to = std::next(from); to != points.end();
       ++from, ++to) {
    const Control *winner = nullptr;
    for (const auto &control : controls) {
      if (control.start <= *from && control.end >= *to &&
          (winner == nullptr || outranks(control, *winner))) {
        winner = &control;
      }
    }
    if (winner != nullptr) {
      append(segments,
             {limit, *from, *to, winner->value, CsipausEventKind::Control,
              winner->eventId, winner->rampTms,
              winner->rampTms ? std::nullopt : setGradW});
    } else if (standing && standing->since <= *from) {
      append(segments, {limit, *from, *to, standing->value,
                        CsipausEventKind::Default, standing->eventId,
                        std::nullopt, setGradW});
    }
  }
  if (standing) {
    append(segments, {limit, *points.rbegin(), std::nullopt, standing->value,
                      CsipausEventKind::Default, standing->eventId,
                      std::nullopt, setGradW});
  }
}

} // namespace

int randomOffset(const std::string &lfdi, const std::string &mrid, int range) {
  if (range == 0) {
    return 0;
  }
  const auto span = static_cast<std::uint64_t>(std::abs(range)) + 1;
  const auto magnitude = static_cast<int>(stableHash(lfdi + ":" + mrid) % span);
  return range < 0 ? -magnitude : magnitude;
}

EffectiveWindow effectiveWindow(const NormalizedEvent &event,
                                const CsipausEventDetail &detail,
                                const std::string &lfdi) {
  const auto start =
      event.startAt +
      std::chrono::seconds(randomOffset(lfdi, detail.mrid,
                                        detail.randomizeStartSeconds.value_or(0)));
  const auto end =
      start + std::chrono::seconds(
                  event.durationSeconds +
                  randomOffset(lfdi, detail.mrid,
                               detail.randomizeDurationSeconds.value_or(0)));
  return {start, end};
}

std::vector<LimitSegment> limitsInForce(const std::vector<ObservedEvent> &observed,
                                        const std::string &lfdi, Timestamp at) {
  std::vector<LimitSegment> segments;
  const auto setGradW = setGradWInForce(observed);
  for (const auto &limit : capNames) {
    addCap(segments, limit, observed, setGradW, lfdi, at);
  }
  return segments;
}

std::optional<int> setGradWInForce(const std::vector<ObservedEvent> &observed) {
  const ObservedEvent *standing = nullptr;
  for (const auto &candidate : observed) {
    if (candidate.detail.kind == CsipausEventKind::Default &&
        candidate.ramp.setGradW &&
        (standing == nullptr ||
         candidate.detail.primacy < standing->detail.primacy)) {
      standing = &candidate;
    }
  }
  return standing ? standing->ramp.setGradW : std::nullopt;
}

std::set<std::string> supersededControls(const std::vector<ObservedEvent> &observed,
                                         const std::string &lfdi, Timestamp at) {
  const auto limits = limitsInForce(observed, lfdi, at);
  std::set<std::string> superseded;
  for (const auto &[event, detail, ramp] : observed) {
    if (detail.kind != CsipausEventKind::Control || isTerminal(event.status) ||
        std::none_of(capNames.begin(), capNames.end(),
                     [&event](const auto &limit) {
                       return capValue(event, limit).has_value();
                     })) {
      continue;
    }
    const auto [start, end] = effectiveWindow(event, detail, lfdi);
    if (end <= at || end <= start) {
      continue;
    }
    const auto from = std::max(at, start);
    const bool wins = std::any_of(
        limits.begin(), limits.end(), [&event, from](const auto &segment) {
          return segment.source == CsipausEventKind::Control &&
                 segment.programEventId == event.programEventId &&
                 segment.end && *segment.end > from;
        });
    if (!wins) {
      superseded.insert(event.programEventId);
    }
  }
  return superseded;
}

bool switchedOff(const std::vector<LimitSegment> &limits, Timestamp at) {
  return std::any_of(limits.begin(), limits.end(), [at](const auto &segment) {
    return (segment.limit == "connect" || segment.limit == "energize") &&
           segment.value == 0 && segment.start <= at &&
           (!segment.end || at < *segment.end);
  });
}

std::vector<ObservedEvent> defaultsOf(const std::vector<ObservedEvent> &observed) {
  std::vector<ObservedEvent> defaults;
  std::copy_if(observed.begin(), observed.end(), std::back_inserter(defaults),
               [](const auto &candidate) {
                 return candidate.detail.kind == CsipausEventKind::Default;
               });
  return defaults;
}

} // namespace grid_services::csipaus
