/*
 * RegisteredReports.h
 *
 *  Created on: Sep 10, 2014
 *      Author: dupes
 */

#ifndef REGISTEREDREPORTS_H_
#define REGISTEREDREPORTS_H_

#include <iostream>
#include <map>
#include <set>

#include <oadrsd/2b/oadrRegisterReportType.hxx>
#include <oadrsd/2b/oadrReportDescriptionType.hxx>
#include <oadrsd/2b/oadrReportType.hxx>
#include <oadrsd/2b/reportNameType.hxx>

using namespace oadr2b::oadr;
using namespace std;

class RegisteredReports {
private:
  map<string, set<string>> m_reports;
  map<string, string> m_reportNames;

public:
  RegisteredReports();
  virtual ~RegisteredReports();

  void
  loadRegisteredReports(oadrRegisterReportType::oadrReport_sequence &sequence);

  bool reportExists(string reportID);
  bool reportDescriptionExists(string reportID, string reportDescriptionID);

  // The reportName of a registered report, such as
  // METADATA_TELEMETRY_STATUS, or empty.
  string reportName(string reportID);
};

#endif /* REGISTEREDREPORTS_H_ */
