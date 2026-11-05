/*
 * RegisteredReports.cpp
 *
 *  Created on: Sep 10, 2014
 *      Author: dupes
 */

#include "../reportmanager/RegisteredReports.h"

RegisteredReports::RegisteredReports()
{
}

/********************************************************************************/

RegisteredReports::~RegisteredReports()
{
}

/********************************************************************************/

void RegisteredReports::loadRegisteredReports(oadrRegisterReportType::oadrReport_sequence &sequence)
{
	m_reports.clear();
	m_reportNames.clear();

	for (auto &oadrReport : sequence)
	{
		string reportSpecifierID = oadrReport.reportSpecifierID();

		if (oadrReport.reportName().present())
			m_reportNames[reportSpecifierID] = *oadrReport.reportName();

		set<string> *reportDescriptions = &m_reports[reportSpecifierID];

		for (auto &oadrReportDescription : oadrReport.oadrReportDescription())
		{
			oadr2b::oadr::oadrReportDescriptionType::rID_type rID = oadrReportDescription.rID();

			reportDescriptions->insert(rID);
		}
	}
}

/********************************************************************************/

bool RegisteredReports::reportExists(string reportID)
{
	if (m_reports.find(reportID) == m_reports.end())
		return false;

	return true;
}

/********************************************************************************/

bool RegisteredReports::reportDescriptionExists(string reportID, string reportDescriptionID)
{
	auto reportItr = m_reports.find(reportID);

	if (reportItr == m_reports.end())
		return false;

	// found the report, let's see if we can find the report description
	set<string> *reportDescriptions = &reportItr->second;

	if (reportDescriptions->find(reportDescriptionID) == reportDescriptions->end())
		return false;

	return true;
}

/********************************************************************************/

string RegisteredReports::reportName(string reportID)
{
	auto itr = m_reportNames.find(reportID);

	return itr == m_reportNames.end() ? "" : itr->second;
}
