/*
 * ReportManager.cpp
 *
 *  Created on: Mar 11, 2015
 *      Author: dupes
 */

#include "../../helper/DateTimeConverter.h"
#include "../../helper/ISO8601Duration.h"
#include "../../helper/globaltime/GlobalTime.h"

#include "../reportmanager/ReportManager.h"

#include <oadrsd/2b/WsCalendarIntervalType.hxx>
#include <oadrsd/2b/properties.hxx>

#include <oadrsd/2b/DurationPropType.hxx>
#include <oadrsd/2b/DurationValueType.hxx>
#include <oadrsd/2b/ReportSpecifierType.hxx>

#include <sstream>

/********************************************************************************/

const string ReportManager::C_REPORT = "report.";
const string ReportManager::C_UPDATE = ".update";
const string ReportManager::C_COMPLETE = ".complete";

/********************************************************************************/

ReportManager::ReportManager(IScheduler *scheduler, IReportService *service,
                             ISendReport *sendReport)
    : m_scheduler(scheduler), m_service(service), m_sendReport(sendReport),
      m_sendUpdates{true} {}

/********************************************************************************/

ReportManager::~ReportManager() {}

/********************************************************************************/

void ReportManager::setRegisteredReports(
    oadrRegisterReportType::oadrReport_sequence &sequence) {
  m_registeredReports.loadRegisteredReports(sequence);
}

/********************************************************************************/

bool ReportManager::validateCreateReport(oadrCreateReportType &createReport) {
  for (auto &reportRequest : createReport.oadrReportRequest()) {
    string reportSpecifierID =
        reportRequest.reportSpecifier().reportSpecifierID();

    if (reportSpecifierID != "METADATA") {
      for (auto &interval :
           reportRequest.reportSpecifier().specifierPayload()) {
        string rID = interval.rID();

        if (!m_registeredReports.reportDescriptionExists(reportSpecifierID,
                                                         rID))
          return false;
      }
    }
  }

  return true;
}

/********************************************************************************/

void ReportManager::generatePeriodicReport(string reportRequestID, time_t now,
                                           bool finalReport) {
  auto itr = m_periodicReports.find(reportRequestID);

  if (itr == m_periodicReports.end())
    return;

  oadrReportRequestType *reportRequest = itr->second.get();

  string reportBackDuration =
      reportRequest->reportSpecifier().reportBackDuration().duration();
  unsigned int reportBackDurationInSeconds =
      ISO8601Duration::TotalSeconds(reportBackDuration);

  oadrUpdateReportType::oadrReport_sequence sequence;

  m_service->OnGeneratePeriodicReport(*reportRequest, sequence, now,
                                      reportBackDurationInSeconds);

  m_sendReport->sendUpdateReport(sequence, now, reportRequestID, now);

  //
  // If this is the last report to send, remove it from m_periodicReports and
  // m_pendingReports
  //

  string duration = reportRequest->reportSpecifier()
                        .reportInterval()
                        ->properties()
                        .duration()
                        .duration();
  unsigned int durationInSeconds = ISO8601Duration::TotalSeconds(duration);

  time_t dtstart =
      DateTimeConverter::DateTimeToTime_t(reportRequest->reportSpecifier()
                                              .reportInterval()
                                              ->properties()
                                              .dtstart()
                                              .date_time());

  // durationInSeconds == 0 means continue until cancelled
  if ((dtstart + durationInSeconds <= now && durationInSeconds != 0) ||
      finalReport) {
    // The request outlives its map entry for the callback.
    unique_ptr<oadrReportRequestType> completed = std::move(itr->second);
    m_pendingReports.erase(reportRequestID);
    m_periodicReports.erase(itr);

    m_service->OnPeriodicReportComplete(*completed);
  }
}

/********************************************************************************/

void ReportManager::sendOneshotReport(oadrReportRequestType &reportRequest) {
  string reportRequestID = reportRequest.reportRequestID();

  oadrUpdateReportType::oadrReport_sequence sequence;

  time_t dtstart = time(NULL);

  m_service->OnGenerateOneshotReport(reportRequest, sequence);

  m_sendReport->sendUpdateReport(sequence, 0, reportRequestID, dtstart);

  m_pendingReports.erase(reportRequestID);
}

/********************************************************************************/

bool ReportManager::isHistoryReport(string reportSpecifierID) {
  return m_registeredReports.reportName(reportSpecifierID)
             .rfind("METADATA_HISTORY", 0) == 0;
}

/********************************************************************************/

void ReportManager::processReportRequest(oadrReportRequestType &reportRequest) {
  string reportRequestID = reportRequest.reportRequestID();
  string reportSpecifierID =
      reportRequest.reportSpecifier().reportSpecifierID();

  //
  // Metadata report: answered with the Metadata reports (OpenADR 2.0b rules
  // 301, 311 and 327)
  //
  if (reportSpecifierID == "METADATA") {
    oadrRegisterReportType::oadrReport_sequence sequence;

    m_service->OnGenerateRegisterReport(sequence);
    m_registeredReports.loadRegisteredReports(sequence);

    m_sendReport->sendRegisterReport(sequence, reportRequestID);

    m_pendingReports.erase(reportRequestID);

    return;
  }

  if (reportRequest.reportSpecifier().reportInterval() == NULL) {
    //
    // oneshot report
    //
    sendOneshotReport(reportRequest);

    return;
  }

  string reportBackDuration =
      reportRequest.reportSpecifier().reportBackDuration().duration();
  unsigned int reportBackDurationInSeconds =
      ISO8601Duration::TotalSeconds(reportBackDuration);

  string duration = reportRequest.reportSpecifier()
                        .reportInterval()
                        ->properties()
                        .duration()
                        .duration();
  unsigned int durationInSeconds = ISO8601Duration::TotalSeconds(duration);

  time_t dtstart =
      DateTimeConverter::DateTimeToTime_t(reportRequest.reportSpecifier()
                                              .reportInterval()
                                              ->properties()
                                              .dtstart()
                                              .date_time());

  if (reportBackDurationInSeconds == 0 && !isHistoryReport(reportSpecifierID)) {
    //
    // oneshot report: any report but a history report with a
    // reportBackDuration of 0 is sent once, now, with one interval of data
    // (rule 324)
    //
    sendOneshotReport(reportRequest);

    return;
  }

  if (reportBackDurationInSeconds == 0) {
    //
    // Historical report
    //

    oadrUpdateReportType::oadrReport_sequence sequence;

    time_t now = time(NULL);

    m_service->OnGenerateHistoricalReport(reportRequest, sequence, dtstart,
                                          durationInSeconds);

    m_sendReport->sendUpdateReport(sequence, dtstart,
                                   reportRequest.reportRequestID(), now);

    m_pendingReports.erase(m_pendingReports.find(reportRequestID));

    return;
  }

  //
  // Periodic report
  //

  // Don't schedule the job if dtstart+durationInSeconds has already passed
  // durationInSeconds == 0 means run forever
  time_t now = GlobalTime::Instance()->now();

  if (dtstart + durationInSeconds < now && durationInSeconds != 0) {
    m_pendingReports.erase(m_pendingReports.find(reportRequestID));

    return;
  }

  m_service->OnPeriodicReportStart(reportRequest);

  m_periodicReports[reportRequestID] =
      unique_ptr<oadrReportRequestType>(reportRequest._clone());

  unique_ptr<JobLambda> periodicReport(
      new JobLambda([this, reportRequestID](time_t now) {
        if (m_sendUpdates) {
          this->generatePeriodicReport(reportRequestID, now, false);
        }
      }));

  string jobID = buildJobID(reportRequestID);

  unique_ptr<SchedulerJob> schedulerJob =
      SchedulerJob::CreatePeriodicSchedulerJob(
          std::move(periodicReport), jobID, dtstart,
          reportBackDurationInSeconds, durationInSeconds);

  m_scheduler->addJob(std::move(schedulerJob));
}

/********************************************************************************/

void ReportManager::manageCreateReport(oadrCreateReportType &createReport) {
  //
  // Are all requests valid?
  //

  if (!validateCreateReport(createReport)) {
    m_sendReport->sendCreatedReport(m_pendingReports, createReport.requestID(),
                                    "452", "INVALID ID");
    return;
  }

  //
  // Answering a request for Metadata sends the requester a Metadata report,
  // which implicitly cancels every report it asked for before (rule 306).
  // The requests in this payload come after it and are kept.
  //

  for (auto &reportRequest : createReport.oadrReportRequest()) {
    if (reportRequest.reportSpecifier().reportSpecifierID() == "METADATA") {
      endPeriodicReports();
      break;
    }
  }

  //
  // All requests valid; add to pending reports and send a 200/OK createdReport
  // message and
  //

  for (auto &reportRequest : createReport.oadrReportRequest()) {
    m_pendingReports.insert(reportRequest.reportRequestID());
  }

  m_sendReport->sendCreatedReport(m_pendingReports, createReport.requestID(),
                                  "200", "OK");

  m_service->OnCreatedReport(createReport.oadrReportRequest());

  //
  // Schedule the reports
  //

  for (auto &reportRequest : createReport.oadrReportRequest()) {
    processReportRequest(reportRequest);
  }
}

/********************************************************************************/

void ReportManager::processCancelReport(string reportRequestID,
                                        bool reportToFollow) {
  auto itr = m_periodicReports.find(reportRequestID);

  // TODO: if one of the reportRequests was invalid, should we return a 452
  // error to the VTN?
  if (itr == m_periodicReports.end())
    return;

  oadrReportRequestType *reportRequest = itr->second.get();

  string jobID = buildJobID(reportRequestID);

  m_scheduler->removeJob(jobID);

  if (!reportToFollow) {
    // The request outlives its map entry for the callback.
    unique_ptr<oadrReportRequestType> completed = std::move(itr->second);
    m_periodicReports.erase(itr);
    m_pendingReports.erase(reportRequestID);

    m_service->OnPeriodicReportComplete(*completed);

    return;
  }

  time_t now = GlobalTime::Instance()->now();

  time_t dtstart =
      DateTimeConverter::DateTimeToTime_t(reportRequest->reportSpecifier()
                                              .reportInterval()
                                              ->properties()
                                              .dtstart()
                                              .date_time());

  string reportBackDuration =
      reportRequest->reportSpecifier().reportBackDuration().duration();
  unsigned int reportBackDurationInSeconds =
      ISO8601Duration::TotalSeconds(reportBackDuration);

  time_t reportTime = dtstart;

  while (reportTime < now)
    reportTime += reportBackDurationInSeconds;

  unique_ptr<JobLambda> periodicReport(
      new JobLambda([this, reportRequestID](time_t now) {
    this->generatePeriodicReport(reportRequestID, now, true);
  }));

  unique_ptr<SchedulerJob> schedulerJob =
      SchedulerJob::CreateOnetimeSchedulerJob(std::move(periodicReport), jobID,
                                              reportTime);

  m_scheduler->addJob(std::move(schedulerJob));
}

/********************************************************************************/

void ReportManager::endPeriodicReports() {
  while (!m_periodicReports.empty()) {
    auto itr = m_periodicReports.begin();
    string reportRequestID = itr->first;

    m_scheduler->removeJob(buildJobID(reportRequestID));

    // The request outlives its map entry for the callback.
    unique_ptr<oadrReportRequestType> ended = std::move(itr->second);
    m_periodicReports.erase(itr);
    m_pendingReports.erase(reportRequestID);

    m_service->OnPeriodicReportComplete(*ended);
  }
}

/********************************************************************************/

void ReportManager::manageCancelReport(oadrCancelReportType &cancelReport) {
  bool reportToFollow = cancelReport.reportToFollow();
  bool allKnown = true;

  for (auto &reportRequestID : cancelReport.reportRequestID()) {
    if (m_periodicReports.find(reportRequestID) == m_periodicReports.end())
      allKnown = false;

    processCancelReport(reportRequestID, reportToFollow);
  }

  // Failing to cancel any of the reports is an error (rule 336)
  if (allKnown)
    m_sendReport->sendCanceledReport(m_pendingReports, cancelReport.requestID(),
                                     "200", "OK");
  else
    m_sendReport->sendCanceledReport(m_pendingReports, cancelReport.requestID(),
                                     "452", "Unknown reportRequestID");
}

/********************************************************************************/

void ReportManager::manageUpdatedReportCancel(
    const oadrCancelReportType &cancelReport) {
  for (auto &reportRequestID : cancelReport.reportRequestID())
    processCancelReport(reportRequestID, cancelReport.reportToFollow());
}

/********************************************************************************/

set<string> &ReportManager::getPendingReports() { return m_pendingReports; }

/********************************************************************************/

map<string, unique_ptr<oadrReportRequestType>> &
ReportManager::getPeriodicReports() {
  return m_periodicReports;
}

/********************************************************************************/

string ReportManager::buildJobID(string reportRequestID) {
  stringstream jobID;

  jobID << C_REPORT << reportRequestID << C_UPDATE;

  return jobID.str();
}

bool ReportManager::isSendUpdates() { return m_sendUpdates; }

void ReportManager::setSendUpdates(bool send) { m_sendUpdates = send; }
