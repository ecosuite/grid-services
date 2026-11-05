/*
 * BlankReportService.h
 *
 *  Created on: Jul 6, 2015
 *      Author: dupes
 */

#ifndef OADRTEST_OADRTEST_TESTS_REPORTMANAGER_BLANKREPORTSERVICE_H_
#define OADRTEST_OADRTEST_TESTS_REPORTMANAGER_BLANKREPORTSERVICE_H_

#include <oadr/manager/reportmanager/IReportService.h>
#include <string>

class BlankReportService : public IReportService {
public:
  BlankReportService();
  virtual ~BlankReportService();

  virtual void
  OnPeriodicReportStart(const oadrReportRequestType &reportRequest);

  virtual void
  OnPeriodicReportComplete(const oadrReportRequestType &reportRequest);

  virtual void
  OnGenerateOneshotReport(const oadrReportRequestType &reportRequest,
                          oadrUpdateReportType::oadrReport_sequence &sequence);

  virtual void
  OnGeneratePeriodicReport(const oadrReportRequestType &reportRequest,
                           oadrUpdateReportType::oadrReport_sequence &sequence,
                           time_t dtstart, unsigned int durationInSeconds);

  virtual void OnGenerateHistoricalReport(
      const oadrReportRequestType &reportRequest,
      oadrUpdateReportType::oadrReport_sequence &sequence, time_t dtstart,
      unsigned int durationInSeconds);

  virtual void OnGenerateRegisterReport(
      oadrRegisterReportType::oadrReport_sequence &sequence);

  virtual void
  OnCreatedReport(oadrCreateReportType::oadrReportRequest_sequence &sequence) {}

  unsigned int OneshotReportCount;
  unsigned int PeriodicReportCount;
  unsigned int HistoricalReportCount;

  unsigned int PeriodicReportStartCount;
  unsigned int PeriodicReportCompleteCount;
  // The reportRequestID of the last completed request, read inside the
  // callback: the request must still be alive there.
  std::string LastCompletedRequestID;
};

#endif /* OADRTEST_OADRTEST_TESTS_REPORTMANAGER_BLANKREPORTSERVICE_H_ */
