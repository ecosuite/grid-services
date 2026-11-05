#include "support/oadr_fixtures.hpp"

#include <sstream>

namespace grid_services::testing::fixtures {

namespace {

std::string payload(const std::string &body) {
  return R"(<?xml version="1.0" encoding="UTF-8"?>
<oadr:oadrPayload xmlns:oadr="http://openadr.org/oadr-2.0b/2012/07"
    xmlns:ei="http://docs.oasis-open.org/ns/energyinterop/201110"
    xmlns:pyld="http://docs.oasis-open.org/ns/energyinterop/201110/payloads"
    xmlns:emix="http://docs.oasis-open.org/ns/emix/2011/06"
    xmlns:xcal="urn:ietf:params:xml:ns:icalendar-2.0"
    xmlns:strm="urn:ietf:params:xml:ns:icalendar-2.0:stream"
    xmlns:power="http://docs.oasis-open.org/ns/emix/2011/06/power"
    xmlns:scale="http://docs.oasis-open.org/ns/emix/2011/06/siscale"
    xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
<oadr:oadrSignedObject>
)" + body +
         R"(
</oadr:oadrSignedObject>
</oadr:oadrPayload>
)";
}

std::string eiResponse(const std::string &code) {
  return "<ei:eiResponse><ei:responseCode>" + code +
         "</ei:responseCode><ei:responseDescription>" +
         (code == "200" ? "OK" : "ERROR") +
         "</ei:responseDescription><pyld:requestID/></ei:eiResponse>";
}

std::string itemBase(const std::string &units) {
  if (units.empty()) {
    return "";
  }
  if (units == "currencyPerKWh") {
    return "<oadr:currencyPerKWh>"
           "<oadr:itemDescription>currencyPerKWh</oadr:itemDescription>"
           "<oadr:itemUnits>USD</oadr:itemUnits>"
           "<scale:siScaleCode>none</scale:siScaleCode>"
           "</oadr:currencyPerKWh>";
  }
  if (units.rfind("power", 0) == 0) {
    return "<power:" + units +
           "><power:itemDescription/><power:itemUnits>W</power:itemUnits>"
           "<scale:siScaleCode>none</scale:siScaleCode>"
           "<power:powerAttributes><power:hertz>60</power:hertz>"
           "<power:voltage>120</power:voltage><power:ac>true</power:ac>"
           "</power:powerAttributes></power:" +
           units + ">";
  }
  return "<power:" + units +
         "><power:itemDescription/><power:itemUnits>Wh</power:itemUnits>"
         "<scale:siScaleCode>none</scale:siScaleCode></power:" +
         units + ">";
}

std::string signal(const SignalSpec &spec, int index, bool floatPayload) {
  std::ostringstream xml;
  xml << "<ei:eiEventSignal><strm:intervals>";
  int uid = 0;
  for (const auto &[duration, value] : spec.intervals) {
    xml << "<ei:interval><xcal:duration><xcal:duration>" << duration
        << "</xcal:duration></xcal:duration>"
        << "<xcal:uid><xcal:text>" << uid++ << "</xcal:text></xcal:uid>";
    if (floatPayload) {
      xml << "<ei:signalPayload><ei:payloadFloat><ei:value>" << value
          << "</ei:value></ei:payloadFloat></ei:signalPayload>";
    }
    xml << "</ei:interval>";
  }
  xml << "</strm:intervals>"
      << "<ei:signalName>" << spec.name << "</ei:signalName>"
      << "<ei:signalType>" << spec.type << "</ei:signalType>"
      << "<ei:signalID>signal-" << index << "</ei:signalID>"
      << itemBase(spec.units) << "</ei:eiEventSignal>";
  return xml.str();
}

std::string event(const EventSpec &spec) {
  std::ostringstream xml;
  xml << "<oadr:oadrEvent><ei:eiEvent><ei:eventDescriptor>"
      << "<ei:eventID>" << spec.id << "</ei:eventID>"
      << "<ei:modificationNumber>" << spec.modification
      << "</ei:modificationNumber>"
      << "<ei:eiMarketContext><emix:marketContext>" << spec.marketContext
      << "</emix:marketContext></ei:eiMarketContext>"
      << "<ei:createdDateTime>2030-07-13T09:10:00Z</ei:createdDateTime>"
      << "<ei:eventStatus>" << spec.status << "</ei:eventStatus>"
      << "<ei:testEvent>" << spec.testEvent << "</ei:testEvent>";
  if (!spec.comment.empty()) {
    xml << "<ei:vtnComment>" << spec.comment << "</ei:vtnComment>";
  }
  xml << "</ei:eventDescriptor>"
      << "<ei:eiActivePeriod><xcal:properties>"
      << "<xcal:dtstart><xcal:date-time>" << spec.start
      << "</xcal:date-time></xcal:dtstart>"
      << "<xcal:duration><xcal:duration>" << spec.duration
      << "</xcal:duration></xcal:duration>";
  if (!spec.startAfter.empty()) {
    xml << "<xcal:tolerance><xcal:tolerate><xcal:startafter>" << spec.startAfter
        << "</xcal:startafter></xcal:tolerate></xcal:tolerance>";
  }
  xml << "</xcal:properties><xcal:components xsi:nil=\"true\"/>"
      << "</ei:eiActivePeriod><ei:eiEventSignals>"
      << signal({spec.signalName, spec.signalType, spec.units, spec.intervals},
                1, spec.floatPayload);
  int index = 2;
  for (const auto &more : spec.moreSignals) {
    xml << signal(more, index++, spec.floatPayload);
  }
  xml << "</ei:eiEventSignals><ei:eiTarget>";
  for (const auto &venId : spec.venIds) {
    xml << "<ei:venID>" << venId << "</ei:venID>";
  }
  xml << "</ei:eiTarget></ei:eiEvent>"
      << "<oadr:oadrResponseRequired>" << spec.responseRequired
      << "</oadr:oadrResponseRequired></oadr:oadrEvent>";
  return xml.str();
}

std::string reportRequest(const ReportRequestSpec &spec) {
  std::string xml = "<oadr:oadrReportRequest>"
                    "<ei:reportRequestID>" +
                    spec.reportRequestId +
                    "</ei:reportRequestID>"
                    "<ei:reportSpecifier>"
                    "<ei:reportSpecifierID>" +
                    spec.reportSpecifierId +
                    "</ei:reportSpecifierID>"
                    "<xcal:granularity><xcal:duration>" +
                    spec.granularity +
                    "</xcal:duration></xcal:granularity>"
                    "<ei:reportBackDuration><xcal:duration>" +
                    spec.backDuration +
                    "</xcal:duration></ei:reportBackDuration>";
  if (!spec.start.empty()) {
    xml += "<ei:reportInterval><xcal:properties>"
           "<xcal:dtstart><xcal:date-time>" +
           spec.start +
           "</xcal:date-time></xcal:dtstart>"
           "<xcal:duration><xcal:duration>" +
           spec.duration +
           "</xcal:duration></xcal:duration>"
           "</xcal:properties></ei:reportInterval>";
  }
  std::vector<std::string> rids{spec.rid};
  rids.insert(rids.end(), spec.moreRids.begin(), spec.moreRids.end());
  for (const auto &rid : rids) {
    xml += "<ei:specifierPayload><ei:rID>" + rid +
           "</ei:rID>"
           "<ei:readingType>x-notApplicable</ei:readingType>"
           "</ei:specifierPayload>";
  }
  return xml + "</ei:reportSpecifier></oadr:oadrReportRequest>";
}

std::string cancelReportBody(const std::string &reportRequestId,
                             bool reportToFollow,
                             const std::string &venId = "ven-1") {
  return "<oadr:oadrCancelReport ei:schemaVersion=\"2.0b\">"
         "<pyld:requestID>cancel-report-1</pyld:requestID>"
         "<ei:reportRequestID>" +
         reportRequestId +
         "</ei:reportRequestID>"
         "<pyld:reportToFollow>" +
         (reportToFollow ? "true" : "false") +
         "</pyld:reportToFollow><ei:venID>" +
         venId + "</ei:venID></oadr:oadrCancelReport>";
}

} // namespace

std::string distributeEvent(const std::string &requestId,
                            const std::vector<EventSpec> &events,
                            const std::string &vtnId) {
  std::string body = "<oadr:oadrDistributeEvent ei:schemaVersion=\"2.0b\">"
                     "<pyld:requestID>" +
                     requestId + "</pyld:requestID><ei:vtnID>" + vtnId +
                     "</ei:vtnID>";
  for (const auto &spec : events) {
    body += event(spec);
  }
  return payload(body + "</oadr:oadrDistributeEvent>");
}

std::string createdPartyRegistration(const std::string &venId,
                                     const std::string &registrationId,
                                     const std::string &pollFrequency,
                                     const std::string &code) {
  return payload(
      "<oadr:oadrCreatedPartyRegistration ei:schemaVersion=\"2.0b\">" +
      eiResponse(code) + "<ei:registrationID>" + registrationId +
      "</ei:registrationID><ei:venID>" + venId +
      "</ei:venID><ei:vtnID>vtn-1</ei:vtnID>"
      "<oadr:oadrProfiles><oadr:oadrProfile>"
      "<oadr:oadrProfileName>2.0b</oadr:oadrProfileName>"
      "<oadr:oadrTransports><oadr:oadrTransport>"
      "<oadr:oadrTransportName>simpleHttp</oadr:oadrTransportName>"
      "</oadr:oadrTransport></oadr:oadrTransports>"
      "</oadr:oadrProfile></oadr:oadrProfiles>"
      "<oadr:oadrRequestedOadrPollFreq><xcal:duration>" +
      pollFrequency +
      "</xcal:duration></oadr:oadrRequestedOadrPollFreq>"
      "</oadr:oadrCreatedPartyRegistration>");
}

std::string createdPartyRegistrationUnknown() {
  return payload(
      "<oadr:oadrCreatedPartyRegistration ei:schemaVersion=\"2.0b\">" +
      eiResponse("200") +
      "<ei:vtnID>vtn-1</ei:vtnID>"
      "<oadr:oadrProfiles><oadr:oadrProfile>"
      "<oadr:oadrProfileName>2.0b</oadr:oadrProfileName>"
      "<oadr:oadrTransports><oadr:oadrTransport>"
      "<oadr:oadrTransportName>simpleHttp</oadr:oadrTransportName>"
      "</oadr:oadrTransport></oadr:oadrTransports>"
      "</oadr:oadrProfile></oadr:oadrProfiles>"
      "</oadr:oadrCreatedPartyRegistration>");
}

std::string response(const std::string &code) {
  return payload("<oadr:oadrResponse ei:schemaVersion=\"2.0b\">" +
                 eiResponse(code) + "<ei:venID>ven-1</ei:venID>" +
                 "</oadr:oadrResponse>");
}

std::string registeredReport(const std::vector<ReportRequestSpec> &piggyback) {
  std::string requests;
  for (const auto &spec : piggyback) {
    requests += reportRequest(spec);
  }
  return payload("<oadr:oadrRegisteredReport ei:schemaVersion=\"2.0b\">" +
                 eiResponse("200") + requests + "<ei:venID>ven-1</ei:venID>" +
                 "</oadr:oadrRegisteredReport>");
}

std::string updatedReport(const std::string &code,
                          const std::string &description,
                          const std::string &cancelReportRequestId) {
  return payload("<oadr:oadrUpdatedReport ei:schemaVersion=\"2.0b\">"
                 "<ei:eiResponse><ei:responseCode>" +
                 code + "</ei:responseCode><ei:responseDescription>" +
                 description +
                 "</ei:responseDescription><pyld:requestID>req-1</pyld:requestID>"
                 "</ei:eiResponse>" +
                 (cancelReportRequestId.empty()
                      ? std::string()
                      : cancelReportBody(cancelReportRequestId, false)) +
                 "<ei:venID>ven-1</ei:venID></oadr:oadrUpdatedReport>");
}

std::string canceledPartyRegistration(const std::string &code) {
  return payload(
      "<oadr:oadrCanceledPartyRegistration ei:schemaVersion=\"2.0b\">" +
      eiResponse(code) +
      "<ei:registrationID>reg-1</ei:registrationID><ei:venID>ven-1</ei:venID>"
      "</oadr:oadrCanceledPartyRegistration>");
}

std::string cancelPartyRegistration(const std::string &registrationId) {
  return payload("<oadr:oadrCancelPartyRegistration ei:schemaVersion=\"2.0b\">"
                 "<pyld:requestID>cancel-1</pyld:requestID>"
                 "<ei:registrationID>" +
                 registrationId +
                 "</ei:registrationID><ei:venID>ven-1</ei:venID>"
                 "</oadr:oadrCancelPartyRegistration>");
}

std::string requestReregistration(const std::string &venId) {
  return payload("<oadr:oadrRequestReregistration ei:schemaVersion=\"2.0b\">"
                 "<ei:venID>" +
                 venId +
                 "</ei:venID></oadr:oadrRequestReregistration>");
}

std::string createdOpt(const std::string &code) {
  return payload("<oadr:oadrCreatedOpt ei:schemaVersion=\"2.0b\">" +
                 eiResponse(code) + "<ei:optID>opt-1</ei:optID>" +
                 "</oadr:oadrCreatedOpt>");
}

std::string createStatusReport(const std::string &reportRequestId,
                               const std::string &start,
                               const std::string &backDuration) {
  return createReport(
      {{reportRequestId, "TELEMETRY_STATUS", "status", backDuration, start}});
}

std::string createReport(const std::vector<ReportRequestSpec> &requests,
                         const std::string &requestId,
                         const std::string &venId) {
  std::string body = "<oadr:oadrCreateReport ei:schemaVersion=\"2.0b\">"
                     "<pyld:requestID>" +
                     requestId + "</pyld:requestID>";
  for (const auto &spec : requests) {
    body += reportRequest(spec);
  }
  if (!venId.empty()) {
    body += "<ei:venID>" + venId + "</ei:venID>";
  }
  return payload(body + "</oadr:oadrCreateReport>");
}

std::string cancelReport(const std::string &reportRequestId,
                         bool reportToFollow, const std::string &venId) {
  return payload(cancelReportBody(reportRequestId, reportToFollow, venId));
}

std::string registerReport(const std::string &venId) {
  return payload("<oadr:oadrRegisterReport ei:schemaVersion=\"2.0b\">"
                 "<pyld:requestID>register-report-1</pyld:requestID>"
                 "<ei:venID>" +
                 venId + "</ei:venID></oadr:oadrRegisterReport>");
}

std::string updateReport(const std::string &requestId) {
  return payload("<oadr:oadrUpdateReport ei:schemaVersion=\"2.0b\">"
                 "<pyld:requestID>" +
                 requestId +
                 "</pyld:requestID><oadr:oadrReport>"
                 "<ei:reportRequestID>vtn-rr-1</ei:reportRequestID>"
                 "<ei:reportSpecifierID>vtn-spec-1</ei:reportSpecifierID>"
                 "<ei:createdDateTime>2030-07-13T09:10:00Z</ei:createdDateTime>"
                 "</oadr:oadrReport></oadr:oadrUpdateReport>");
}

} // namespace grid_services::testing::fixtures
