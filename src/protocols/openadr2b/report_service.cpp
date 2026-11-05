#include "protocols/openadr2b/report_service.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include <oadr/helper/DateTimeConverter.h>
#include <oadr/helper/ISO8601Duration.h>
#include <oadr/request/report/ReportHelper.h>
#include <oadr/request/report/ReportName.h>
#include <oadrsd/2b/oadr_20b.hxx>

namespace grid_services::openadr2b {

namespace {

constexpr int statusSamplingSeconds = 60;

Timestamp fromTimeT(std::time_t at) {
  return Timestamp(std::chrono::seconds(at));
}

std::time_t toTimeT(Timestamp at) {
  return static_cast<std::time_t>(at.time_since_epoch().count());
}

oadrDataQualityType::value qualityOf(datum::Quality quality) {
  switch (quality) {
  case datum::Quality::Good:
    return oadrDataQualityType::Quality_Good___Non_Specific;
  case datum::Quality::NoValue:
    // A placeholder for missing data must be one of the Quality Bad values
    // (rule 318). The delivery record keeps the more precise reason.
    return oadrDataQualityType::Quality_Bad___Non_Specific;
  case datum::Quality::Stale:
    return oadrDataQualityType::Quality_Bad___Last_Known_Value;
  case datum::Quality::Unreadable:
    return oadrDataQualityType::Quality_Bad___Comm_Failure;
  }
  return oadrDataQualityType::Quality_Bad___Non_Specific;
}

ReportHelper::ePowerItemType powerKind(const std::string &kind) {
  if (kind == "power_reactive") {
    return ReportHelper::ePowerReactive;
  }
  if (kind == "power_apparent") {
    return ReportHelper::ePowerApparent;
  }
  return ReportHelper::ePowerReal;
}

ReportHelper::eEnergyItemType energyKind(const std::string &kind) {
  if (kind == "energy_reactive") {
    return ReportHelper::eEnergyReactive;
  }
  if (kind == "energy_apparent") {
    return ReportHelper::eEnergyApparent;
  }
  return ReportHelper::eEnergyReal;
}

// Builds the metadata description of one data point. Throws
// std::invalid_argument when its OpenADR terms are not in the schema.
oadrReportDescriptionType describe(const ReportDataPoint &point,
                                   const ReportOffer &offer,
                                   const std::string &marketContext) {
  try {
    const ReportEnumeratedType reportType(point.reportType.c_str());
    const ReadingTypeEnumeratedType readingType(point.readingType.c_str());
    const SiScaleCodeType scale(point.item.scale.c_str());
    const auto &kind = point.item.kind;
    if (kind.rfind("power_", 0) == 0) {
      // The attributes describe the metered supply; nominal without a
      // meter description.
      return ReportHelper::generateDescriptionPowerItem(
          point.rid, point.resourceId, reportType, readingType, marketContext,
          offer.minSamplingSeconds, offer.maxSamplingSeconds, false,
          &DurationModifier::SECONDS, powerKind(kind), point.item.units, scale,
          60.0, 0.0, true, point.item.description);
    }
    if (kind.rfind("energy_", 0) == 0) {
      return ReportHelper::generateDescriptionEnergyItem(
          point.rid, point.resourceId, reportType, readingType, marketContext,
          offer.minSamplingSeconds, offer.maxSamplingSeconds, false,
          &DurationModifier::SECONDS, energyKind(kind), point.item.units, scale,
          point.item.description);
    }
    if (kind == "temperature") {
      return ReportHelper::generateDescriptionTemperature(
          point.rid, point.resourceId, reportType, readingType, marketContext,
          offer.minSamplingSeconds, offer.maxSamplingSeconds, false,
          &DurationModifier::SECONDS,
          temperatureUnitType(point.item.units.c_str()), scale);
    }
    // Voltage and frequency have no OpenADR item of their own here.
    const auto description =
        !point.item.description.empty() ? point.item.description
        : kind == "voltage"             ? std::string("Voltage")
        : kind == "frequency"           ? std::string("Frequency")
                                        : point.item.description;
    return ReportHelper::generateDescriptionCustom(
        point.rid, point.resourceId, reportType, readingType, marketContext,
        offer.minSamplingSeconds, offer.maxSamplingSeconds, false,
        &DurationModifier::SECONDS, description, point.item.units, scale);
  } catch (const std::invalid_argument &) {
    throw;
  } catch (const std::exception &error) {
    throw std::invalid_argument("report " + offer.specifierId +
                                " data point " + point.rid +
                                " is not valid OpenADR: " + error.what());
  }
}

// Throws std::invalid_argument when a data point of a report under one of
// the alliance's well-known names breaks that report's profile: reportType
// usage, readingType Direct Read, and powerReal or energyReal units (OpenADR
// 2.0b rules 308 and 331).
void checkProfile(const ReportOffer &offer, const ReportDataPoint &point) {
  if (offer.reportName != "TELEMETRY_USAGE" &&
      offer.reportName != "HISTORY_USAGE") {
    return;
  }
  const auto breaks = [&](const std::string &what) {
    return std::invalid_argument(
        "report " + offer.specifierId + " data point " + point.rid + " has " +
        what + ", but " + offer.reportName +
        " reports use usage, Direct Read, and power_real or energy_real "
        "(OpenADR 2.0b rule 331)");
  };
  if (point.reportType != "usage") {
    throw breaks("report type " + point.reportType);
  }
  if (point.readingType != "Direct Read") {
    throw breaks("reading type " + point.readingType);
  }
  if (point.item.kind != "power_real" && point.item.kind != "energy_real") {
    throw breaks("item kind " + point.item.kind);
  }
}

} // namespace

int durationSeconds(const std::string &iso8601) {
  if (iso8601.empty()) {
    return 0;
  }
  return static_cast<int>(ISO8601Duration::TotalSeconds(iso8601));
}

ReportService::ReportService(std::string resourceId,
                             std::optional<std::string> marketContext,
                             std::vector<ReportOffer> offers,
                             std::shared_ptr<datum::DatumReader> reader,
                             ReportRequestLog &requests,
                             std::function<std::time_t()> clock)
    : resourceId_(std::move(resourceId)), marketContext_(std::move(marketContext)),
      offers_(std::move(offers)), reader_(std::move(reader)),
      requests_(requests), clock_(std::move(clock)) {
  std::set<std::string> specifiers{statusSpecifierId};
  for (const auto &offer : offers_) {
    if (!specifiers.insert(offer.specifierId).second) {
      throw std::invalid_argument("report specifier " + offer.specifierId +
                                  " is used twice");
    }
    // Validates the OpenADR terms up front, so a bad offer is a
    // configuration error rather than a failed registration.
    std::set<std::string> rids;
    for (const auto &point : offer.dataPoints) {
      // Rule 308.
      if (!rids.insert(point.rid).second) {
        throw std::invalid_argument("report " + offer.specifierId +
                                    " uses rID " + point.rid + " twice");
      }
      checkProfile(offer, point);
      describe(point, offer, marketContext_.value_or(""));
    }
  }
}

void ReportService::setParticipation(Participation participation) {
  participation_ = participation;
}

const ReportOffer *ReportService::offer(const std::string &specifierId) const {
  for (const auto &offer : offers_) {
    if (offer.enabled && offer.specifierId == specifierId) {
      return &offer;
    }
  }
  return nullptr;
}

bool ReportService::offersSpecifier(const std::string &specifierId) const {
  return specifierId == statusSpecifierId || offer(specifierId) != nullptr;
}

std::optional<std::pair<std::string, std::string>>
ReportService::refusal(const oadrReportRequestType &request) const {
  const std::string specifierId = request.reportSpecifier().reportSpecifierID();
  if (specifierId == "METADATA") {
    return std::nullopt;
  }
  if (!offersSpecifier(specifierId)) {
    return std::make_pair("452", "unknown reportSpecifierID " + specifierId);
  }
  const auto *offered = offer(specifierId);
  const bool history =
      offered != nullptr && offered->reportName.rfind("HISTORY", 0) == 0;
  if (history && request.reportSpecifier().reportInterval() != nullptr) {
    const auto &properties =
        request.reportSpecifier().reportInterval()->properties();
    int requested = durationSeconds(properties.duration().duration());
    if (requested == 0) {
      // Everything since dtstart (rule 325).
      const std::time_t start =
          DateTimeConverter::DateTimeToTime_t(properties.dtstart().date_time());
      requested = static_cast<int>(std::max<std::time_t>(0, clock_() - start));
    }
    if (requested > offered->historySeconds) {
      // Rule 340: more data than offered.
      return std::make_pair(
          "454", "only " + std::to_string(offered->historySeconds) +
                     "s of history are offered for " + specifierId);
    }
  }
  return std::nullopt;
}

std::optional<GeneratedReport>
ReportService::takeGenerated(const std::string &requestId) {
  const auto found = generated_.find(requestId);
  if (found == generated_.end()) {
    return std::nullopt;
  }
  auto report = std::move(found->second);
  generated_.erase(found);
  return report;
}

void ReportService::remember(const std::string &requestId,
                             GeneratedReport report) {
  generated_[requestId] = std::move(report);
}

// ---------------------------------------------------------------------------
// Metadata

void ReportService::OnGenerateRegisterReport(
    oadrRegisterReportType::oadrReport_sequence &sequence) {
  const auto marketContext = marketContext_.value_or("");
  const std::time_t now = clock_();

  oadrReportType::oadrReportDescription_sequence status;
  status.push_back(ReportHelper::generateDescriptionResourceStatus(
      statusRid, resourceId_, marketContext, statusSamplingSeconds,
      statusSamplingSeconds, false, &DurationModifier::SECONDS));
  // The metadata duration says how much data is kept, and may not be less
  // than the sampling period (rule 312).
  sequence.push_back(ReportHelper::generateReportDescription(
      &ReportName::TELEMETRY_STATUS, statusSamplingSeconds,
      &DurationModifier::SECONDS, statusSpecifierId, now, status));

  for (const auto &offer : offers_) {
    if (!offer.enabled) {
      continue;
    }
    oadrReportType::oadrReportDescription_sequence descriptions;
    for (const auto &point : offer.dataPoints) {
      descriptions.push_back(describe(point, offer, marketContext));
    }
    ReportName name(offer.reportName);
    sequence.push_back(ReportHelper::generateReportDescription(
        &name, std::max(offer.historySeconds, offer.maxSamplingSeconds),
        &DurationModifier::SECONDS, offer.specifierId, now, descriptions));
  }
}

// ---------------------------------------------------------------------------
// Requests

void ReportService::OnCreatedReport(
    oadrCreateReportType::oadrReportRequest_sequence &sequence) {
  if (sink_ == nullptr) {
    return;
  }
  for (const auto &request : sequence) {
    const auto &specifier = request.reportSpecifier();
    const std::string specifierId = specifier.reportSpecifierID();
    if (specifierId == "METADATA" || !offersSpecifier(specifierId)) {
      continue;
    }
    const auto *offered = offer(specifierId);

    ReportRequest record;
    record.programRequestId = request.reportRequestID();
    record.specifierId = specifierId;
    record.reportName = offered != nullptr ? offered->reportName
                                           : std::string("TELEMETRY_STATUS");
    if (offered != nullptr) {
      record.offerId = offered->id;
    }
    record.granularitySeconds =
        durationSeconds(specifier.granularity().duration());
    record.reportBackSeconds =
        durationSeconds(specifier.reportBackDuration().duration());
    const bool history = record.reportName.rfind("HISTORY", 0) == 0;
    if (specifier.reportInterval() == nullptr) {
      record.kind = ReportRequestKind::OneShot;
    } else {
      const auto &properties = specifier.reportInterval()->properties();
      record.startAt = fromTimeT(DateTimeConverter::DateTimeToTime_t(
          properties.dtstart().date_time()));
      record.durationSeconds =
          durationSeconds(properties.duration().duration());
      record.kind = record.reportBackSeconds > 0 ? ReportRequestKind::Periodic
                    : history                    ? ReportRequestKind::History
                                                 : ReportRequestKind::OneShot;
    }
    for (const auto &payload : specifier.specifierPayload()) {
      record.rids.push_back(payload.rID());
    }
    requests_.requested(record);
  }
}

void ReportService::OnPeriodicReportComplete(
    const oadrReportRequestType &request) {
  if (sink_ != nullptr) {
    requests_.ended(request.reportRequestID(),
                    cancelling_ ? ReportRequestStatus::Cancelled
                                : ReportRequestStatus::Completed);
  }
}

// ---------------------------------------------------------------------------
// Values

std::vector<std::string>
ReportService::requestedRids(const oadrReportRequestType &request) const {
  const std::string specifierId = request.reportSpecifier().reportSpecifierID();
  const auto *offered = offer(specifierId);
  std::vector<std::string> rids;
  for (const auto &payload : request.reportSpecifier().specifierPayload()) {
    const std::string rid = payload.rID();
    const bool known = specifierId == statusSpecifierId
                           ? rid == statusRid
                           : offered != nullptr &&
                                 dataPoint(*offered, rid) != nullptr;
    if (known && std::find(rids.begin(), rids.end(), rid) == rids.end()) {
      rids.push_back(rid);
    }
  }
  return rids;
}

const ReportDataPoint *ReportService::dataPoint(const ReportOffer &offer,
                                                const std::string &rid) const {
  for (const auto &point : offer.dataPoints) {
    if (point.rid == rid) {
      return &point;
    }
  }
  return nullptr;
}

ReportService::Datums ReportService::readDatums(
    const ReportOffer &offer, const std::vector<std::string> &rids,
    Timestamp start, Timestamp end) {
  Datums datums;
  if (!reader_) {
    datums.failure = "the connection has no SolarNetwork credential";
    return datums;
  }
  for (const auto &rid : rids) {
    const auto *point = dataPoint(offer, rid);
    const Stream stream{point->source.nodeId, point->source.sourceId};
    if (datums.byStream.count(stream) > 0 || datums.failure) {
      continue;
    }
    const auto request =
        reader_->describeList(stream.nodeId, stream.sourceId, start, end);
    try {
      auto found = reader_->list(stream.nodeId, stream.sourceId, start, end);
      datums.reads.push_back(readRecord(stream.nodeId, stream.sourceId, "list",
                                      request, found.size(), std::nullopt,
                                      std::make_pair(start, end)));
      datums.byStream[stream] = std::move(found);
    } catch (const datum::DatumError &error) {
      datums.failure = error.what();
      datums.reads.push_back(readRecord(stream.nodeId, stream.sourceId, "list",
                                      request, std::nullopt, datums.failure,
                                      std::make_pair(start, end)));
    }
  }
  if (datums.failure && sink_ != nullptr) {
    sink_->activity({.category = ActivityCategory::Reports,
                     .message = "Could not read datums for report " +
                                offer.specifierId + ": " + *datums.failure});
  }
  return datums;
}

oadr2b::oadr::oadrReportPayloadType
ReportService::payloadFor(const std::string &rid,
                          const datum::Sample &sample) const {
  // A point with no value still goes in the report, as a placeholder whose
  // quality says why (rule 318).
  return ReportHelper::generateOadrReportPayloadFloat(
      rid, static_cast<float>(sample.value.value_or(0.0)),
      qualityOf(sample.quality));
}

void ReportService::appendStatusReport(
    const oadrReportRequestType &request,
    oadrUpdateReportType::oadrReport_sequence &sequence, Timestamp start,
    int durationSeconds) {
  const bool manualOverride = participation_ == Participation::Paused;
  IntervalType interval = ReportHelper::generateInterval(
      toTimeT(start), std::max(1, durationSeconds), &DurationModifier::SECONDS);
  GeneratedReport generated;
  generated.intervalStart = start;
  generated.intervalEnd = start + std::chrono::seconds(durationSeconds);
  for (const auto &rid : requestedRids(request)) {
    interval.streamPayloadBase().push_back(
        ReportHelper::generateOadrReportPayloadResourceStatus(
            rid, true, manualOverride));
    generated.payload.push_back(
        {{"rid", rid},
         {"dtstart", isoTime(start)},
         {"duration_seconds", durationSeconds},
         {"online", true},
         {"manual_override", manualOverride},
         {"quality", toString(datum::Quality::Good)}});
  }
  intervals::interval_sequence intervals;
  intervals.push_back(interval);
  const std::string requestId = request.reportRequestID();
  sequence.push_back(ReportHelper::generateReport(
      &ReportName::TELEMETRY_STATUS, statusSpecifierId, requestId, clock_(),
      intervals));
  remember(requestId, std::move(generated));
}

void ReportService::appendIntervals(
    const oadrReportRequestType &request,
    oadrUpdateReportType::oadrReport_sequence &sequence, Timestamp start,
    Timestamp end, int granularitySeconds) {
  const std::string specifierId = request.reportSpecifier().reportSpecifierID();
  const std::string requestId = request.reportRequestID();
  const int total =
      static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(end - start).count());
  if (specifierId == statusSpecifierId) {
    appendStatusReport(request, sequence, start, total);
    return;
  }
  const auto *offered = offer(specifierId);
  if (offered == nullptr || total <= 0) {
    return;
  }
  const int granularity =
      granularitySeconds <= 0 || granularitySeconds > total ? total
                                                             : granularitySeconds;
  const int count = std::max(1, total / granularity);
  const auto rids = requestedRids(request);
  // Read back one interval so differences have a baseline, and take in the
  // reading at the end of the window.
  const auto datums =
      readDatums(*offered, rids, start - std::chrono::seconds(granularity),
                 end + std::chrono::seconds(1));

  GeneratedReport generated;
  generated.reads = datums.reads;
  generated.intervalStart = start;
  generated.intervalEnd = start + std::chrono::seconds(count * granularity);
  intervals::interval_sequence intervals;
  for (int index = 0; index < count; ++index) {
    const Timestamp from = start + std::chrono::seconds(index * granularity);
    const Timestamp to = from + std::chrono::seconds(granularity);
    IntervalType interval = ReportHelper::generateInterval(
        toTimeT(from), granularity, &DurationModifier::SECONDS);
    for (const auto &rid : rids) {
      const auto *point = dataPoint(*offered, rid);
      const Stream stream{point->source.nodeId, point->source.sourceId};
      const auto found = datums.byStream.find(stream);
      const datum::Sample sample =
          found == datums.byStream.end()
              ? datum::Sample{std::nullopt, datum::Quality::Unreadable}
              : datum::reduceWindow(found->second, point->source, from, to);
      interval.streamPayloadBase().push_back(payloadFor(rid, sample));
      generated.payload.push_back(
          {{"rid", rid},
           {"point_id", point->pointId},
           {"dtstart", isoTime(from)},
           {"duration_seconds", granularity},
           {"value", sample.value ? nlohmann::json(*sample.value)
                                  : nlohmann::json(nullptr)},
           {"quality", toString(sample.quality)},
           {"source", sourceRecord(point->source)}});
    }
    intervals.push_back(interval);
  }
  ReportName name(offered->reportName);
  sequence.push_back(ReportHelper::generateReport(
      &name, specifierId, requestId, clock_(), intervals));
  remember(requestId, std::move(generated));
}

void ReportService::OnGenerateOneshotReport(
    const oadrReportRequestType &request,
    oadrUpdateReportType::oadrReport_sequence &sequence) {
  const std::string specifierId = request.reportSpecifier().reportSpecifierID();
  const std::string requestId = request.reportRequestID();
  const Timestamp now = fromTimeT(clock_());
  if (specifierId == statusSpecifierId) {
    appendStatusReport(request, sequence, now, statusSamplingSeconds);
    return;
  }
  const auto *offered = offer(specifierId);
  if (offered == nullptr) {
    return;
  }
  // The most recent sample of each point, as one interval of the offered
  // sampling period ending now (rule 337).
  const int duration = offered->minSamplingSeconds;
  const Timestamp start = now - std::chrono::seconds(duration);
  const auto rids = requestedRids(request);
  std::map<Stream, std::optional<datum::Datum>> latest;
  std::optional<std::string> failure;
  nlohmann::json reads = nlohmann::json::array();
  if (!reader_) {
    failure = "the connection has no SolarNetwork credential";
  }
  for (const auto &rid : rids) {
    const auto *point = dataPoint(*offered, rid);
    const Stream stream{point->source.nodeId, point->source.sourceId};
    if (failure || latest.count(stream) > 0) {
      continue;
    }
    const auto request =
        reader_->describeMostRecent(stream.nodeId, stream.sourceId);
    try {
      auto found = reader_->mostRecent(stream.nodeId, stream.sourceId);
      reads.push_back(readRecord(stream.nodeId, stream.sourceId, "most_recent",
                               request, found ? 1 : 0, std::nullopt));
      latest[stream] = std::move(found);
    } catch (const datum::DatumError &error) {
      failure = error.what();
      reads.push_back(readRecord(stream.nodeId, stream.sourceId, "most_recent",
                               request, std::nullopt, failure));
    }
  }
  if (failure && sink_ != nullptr) {
    sink_->activity({.category = ActivityCategory::Reports,
                     .message = "Could not read datums for report " +
                                offered->specifierId + ": " + *failure});
  }

  GeneratedReport generated;
  generated.reads = std::move(reads);
  generated.intervalStart = start;
  generated.intervalEnd = now;
  IntervalType interval = ReportHelper::generateInterval(
      toTimeT(start), duration, &DurationModifier::SECONDS);
  for (const auto &rid : rids) {
    const auto *point = dataPoint(*offered, rid);
    const Stream stream{point->source.nodeId, point->source.sourceId};
    const auto found = latest.find(stream);
    const datum::Sample sample =
        found == latest.end()
            ? datum::Sample{std::nullopt, datum::Quality::Unreadable}
            : datum::latestSample(found->second, point->source, now);
    interval.streamPayloadBase().push_back(payloadFor(rid, sample));
    generated.payload.push_back(
        {{"rid", rid},
         {"point_id", point->pointId},
         {"dtstart", isoTime(start)},
         {"duration_seconds", duration},
         {"value", sample.value ? nlohmann::json(*sample.value)
                                : nlohmann::json(nullptr)},
         {"quality", toString(sample.quality)},
         {"source", sourceRecord(point->source)}});
  }
  intervals::interval_sequence intervals;
  intervals.push_back(interval);
  ReportName name(offered->reportName);
  sequence.push_back(ReportHelper::generateReport(
      &name, specifierId, requestId, clock_(), intervals));
  remember(requestId, std::move(generated));
}

void ReportService::OnGeneratePeriodicReport(
    const oadrReportRequestType &request,
    oadrUpdateReportType::oadrReport_sequence &sequence, time_t dtstart,
    unsigned int durationInSeconds) {
  // The report covers the period that just elapsed, in intervals of the
  // requested granularity.
  const Timestamp end = fromTimeT(dtstart);
  const Timestamp start = end - std::chrono::seconds(durationInSeconds);
  appendIntervals(request, sequence, start, end,
                  durationSeconds(
                      request.reportSpecifier().granularity().duration()));
}

void ReportService::OnGenerateHistoricalReport(
    const oadrReportRequestType &request,
    oadrUpdateReportType::oadrReport_sequence &sequence, time_t dtstart,
    unsigned int durationInSeconds) {
  // History is served at the offer's finest sampling period, up to now.
  const auto *offered = offer(request.reportSpecifier().reportSpecifierID());
  if (offered == nullptr) {
    return;
  }
  // A zero duration asks for everything since dtstart (rule 325).
  const Timestamp start = fromTimeT(dtstart);
  const Timestamp now = fromTimeT(clock_());
  const Timestamp end =
      durationInSeconds == 0
          ? now
          : std::min(start + std::chrono::seconds(durationInSeconds), now);
  appendIntervals(request, sequence, start, end, offered->minSamplingSeconds);
}

} // namespace grid_services::openadr2b
