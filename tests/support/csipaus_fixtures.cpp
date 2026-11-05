#include "support/csipaus_fixtures.hpp"

namespace grid_services::testing::csipaus_fixtures {

namespace {

const std::string namespaces = " xmlns=\"urn:ieee:std:2030.5:ns\""
                               " xmlns:csipaus=\"https://csipaus.org/ns\"";

// `element` as a document: its root declares the namespaces.
std::string document(const std::string &element) {
  const auto nameEnd = element.find_first_of(" >", 1);
  return element.substr(0, nameEnd) + namespaces + element.substr(nameEnd);
}

std::string join(const std::vector<std::string> &elements) {
  std::string text;
  for (const auto &element : elements) {
    text += element;
  }
  return text;
}

std::string activePower(const std::string &name, int watts) {
  return "<csipaus:" + name + "><multiplier>0</multiplier><value>" +
         std::to_string(watts) + "</value></csipaus:" + name + ">";
}

} // namespace

std::string deviceCapability(int pollRate, const std::string &time,
                             const std::string &mirrors) {
  return document("<DeviceCapability href=\"/dcap\" pollRate=\"" +
                  std::to_string(pollRate) + "\">"
                  "<TimeLink href=\"" + time + "\"/>"
                  "<EndDeviceListLink href=\"/edev\" all=\"1\"/>"
                  "<MirrorUsagePointListLink href=\"" + mirrors +
                  "\" all=\"0\"/>"
                  "</DeviceCapability>");
}

std::string endDeviceElement(const std::string &href, const std::string &lfdi,
                             unsigned long long sfdi, bool registration) {
  return "<EndDevice href=\"" + href + "\" subscribable=\"0\">"
         "<lFDI>" + lfdi + "</lFDI>"
         "<sFDI>" + std::to_string(sfdi) + "</sFDI>"
         "<changedTime>1790000000</changedTime>"
         "<FunctionSetAssignmentsListLink href=\"" + href + "/fsa\" all=\"1\"/>" +
         (registration ? "<RegistrationLink href=\"" + href + "/rg\"/>" : "") +
         "<csipaus:ConnectionPointLink href=\"" + href + "/cp\"/>"
         "</EndDevice>";
}

std::string endDevice(const std::string &href, const std::string &lfdi,
                      unsigned long long sfdi, bool registration) {
  return document(endDeviceElement(href, lfdi, sfdi, registration));
}

std::string endDeviceList(const std::vector<std::string> &elements, int all,
                          int pollRate) {
  const auto total = all < 0 ? static_cast<int>(elements.size()) : all;
  return document("<EndDeviceList href=\"/edev\" subscribable=\"0\" all=\"" +
                  std::to_string(total) + "\" results=\"" +
                  std::to_string(elements.size()) + "\" pollRate=\"" +
                  std::to_string(pollRate) + "\">" + join(elements) +
                  "</EndDeviceList>");
}

std::string registration(unsigned pin) {
  return document("<Registration href=\"/edev/3/rg\" pollRate=\"86400\">"
                  "<dateTimeRegistered>1789900000</dateTimeRegistered>"
                  "<pIN>" + std::to_string(pin) + "</pIN>"
                  "</Registration>");
}

std::string time(long long currentTime, int pollRate) {
  return document("<Time href=\"/tm\" pollRate=\"" + std::to_string(pollRate) +
                  "\"><currentTime>" + std::to_string(currentTime) +
                  "</currentTime><dstEndTime>0</dstEndTime>"
                  "<dstOffset>0</dstOffset><dstStartTime>0</dstStartTime>"
                  "<quality>7</quality><tzOffset>36000</tzOffset></Time>");
}

std::string functionSetAssignmentsList(int pollRate) {
  return document(
      "<FunctionSetAssignmentsList href=\"/edev/3/fsa\" subscribable=\"0\" "
      "all=\"1\" results=\"1\" pollRate=\"" + std::to_string(pollRate) + "\">"
      "<FunctionSetAssignments href=\"/edev/3/fsa/1\" subscribable=\"0\">"
      "<DERProgramListLink href=\"/derp\" all=\"1\"/>"
      "<mRID>A1000000000000000000000000000001</mRID>"
      "</FunctionSetAssignments>"
      "</FunctionSetAssignmentsList>");
}

std::string emptyFunctionSetAssignmentsList(int pollRate) {
  return document(
      "<FunctionSetAssignmentsList href=\"/edev/3/fsa\" subscribable=\"0\" "
      "all=\"0\" results=\"0\" pollRate=\"" + std::to_string(pollRate) +
      "\"/>");
}

std::string derProgramList(int pollRate) {
  return document(
      "<DERProgramList href=\"/derp\" subscribable=\"0\" all=\"1\" "
      "results=\"1\" pollRate=\"" + std::to_string(pollRate) + "\">"
      "<DERProgram href=\"/derp/1\" subscribable=\"0\">"
      "<mRID>B1000000000000000000000000000001</mRID>"
      "<description>Site limits</description>"
      "<DefaultDERControlLink href=\"/derp/1/dderc\"/>"
      "<DERControlListLink href=\"/derp/1/derc\" all=\"1\"/>"
      "<primacy>2</primacy>"
      "</DERProgram>"
      "</DERProgramList>");
}

std::string twoDerProgramList(int pollRate) {
  return document(
      "<DERProgramList href=\"/derp\" subscribable=\"0\" all=\"2\" "
      "results=\"2\" pollRate=\"" + std::to_string(pollRate) + "\">"
      "<DERProgram href=\"/derp/1\" subscribable=\"0\">"
      "<mRID>B1000000000000000000000000000001</mRID>"
      "<description>Site limits</description>"
      "<DefaultDERControlLink href=\"/derp/1/dderc\"/>"
      "<DERControlListLink href=\"/derp/1/derc\" all=\"1\"/>"
      "<primacy>1</primacy>"
      "</DERProgram>"
      "<DERProgram href=\"/derp/2\" subscribable=\"0\">"
      "<mRID>B2000000000000000000000000000002</mRID>"
      "<description>Network support</description>"
      "<DERControlListLink href=\"/derp/2/derc\" all=\"1\"/>"
      "<primacy>2</primacy>"
      "</DERProgram>"
      "</DERProgramList>");
}

std::string defaultDerControl(int exportWatts, std::optional<int> setGradW) {
  return document("<DefaultDERControl href=\"/derp/1/dderc\" "
                  "subscribable=\"0\">"
                  "<mRID>D1000000000000000000000000000001</mRID>"
                  "<DERControlBase>" +
                  activePower("opModExpLimW", exportWatts) +
                  "</DERControlBase>" +
                  (setGradW ? "<setGradW>" + std::to_string(*setGradW) +
                                  "</setGradW>"
                            : std::string()) +
                  "</DefaultDERControl>");
}

std::string derControlElement(const std::string &mrid, int currentStatus,
                              long long start, int duration, int exportWatts) {
  return derControlElement(mrid, currentStatus, start, duration, exportWatts,
                           std::nullopt, std::nullopt);
}

std::string derControlElement(const std::string &mrid, int currentStatus,
                              long long start, int duration, int exportWatts,
                              std::optional<int> randomizeStart,
                              std::optional<int> randomizeDuration) {
  const auto randomize = [](const std::string &name, std::optional<int> range) {
    return range ? "<" + name + ">" + std::to_string(*range) + "</" + name + ">"
                 : std::string();
  };
  return "<DERControl href=\"/derp/1/derc/" + mrid.substr(mrid.size() - 1) +
         "\" replyTo=\"/rsps/1/rsp\" responseRequired=\"03\" "
         "subscribable=\"0\">"
         "<mRID>" + mrid + "</mRID>"
         "<creationTime>1789990000</creationTime>"
         "<EventStatus><currentStatus>" + std::to_string(currentStatus) +
         "</currentStatus><dateTime>1789990000</dateTime>"
         "<potentiallySuperseded>false</potentiallySuperseded></EventStatus>"
         "<interval><duration>" + std::to_string(duration) +
         "</duration><start>" + std::to_string(start) + "</start></interval>" +
         randomize("randomizeDuration", randomizeDuration) +
         randomize("randomizeStart", randomizeStart) +
         "<DERControlBase>" + activePower("opModExpLimW", exportWatts) +
         "</DERControlBase>"
         "</DERControl>";
}

std::string derControlElement(
    const std::string &mrid, int currentStatus, long long start, int duration,
    long long creationTime, const std::vector<std::pair<std::string, int>> &caps,
    const std::string &responseRequired, std::optional<int> rampTms) {
  std::string switches;
  std::string base =
      rampTms ? "<rampTms>" + std::to_string(*rampTms) + "</rampTms>" : "";
  for (const auto &[name, value] : caps) {
    if (name == "opModConnect" || name == "opModEnergize") {
      switches += "<" + name + ">" + (value != 0 ? "true" : "false") + "</" +
                  name + ">";
    } else {
      base += activePower(name, value);
    }
  }
  base = switches + base;
  return "<DERControl href=\"/derp/1/derc/" + mrid.substr(mrid.size() - 1) +
         "\" replyTo=\"/rsps/1/rsp\" responseRequired=\"" + responseRequired +
         "\" subscribable=\"0\">"
         "<mRID>" + mrid + "</mRID>"
         "<creationTime>" + std::to_string(creationTime) + "</creationTime>"
         "<EventStatus><currentStatus>" + std::to_string(currentStatus) +
         "</currentStatus><dateTime>" + std::to_string(creationTime) +
         "</dateTime>"
         "<potentiallySuperseded>false</potentiallySuperseded></EventStatus>"
         "<interval><duration>" + std::to_string(duration) +
         "</duration><start>" + std::to_string(start) + "</start></interval>"
         "<DERControlBase>" + base + "</DERControlBase>"
         "</DERControl>";
}

std::string derControlList(const std::vector<std::string> &elements) {
  return document("<DERControlList href=\"/derp/1/derc\" subscribable=\"0\" "
                  "all=\"" + std::to_string(elements.size()) +
                  "\" results=\"" + std::to_string(elements.size()) + "\">" +
                  join(elements) + "</DERControlList>");
}

std::string reportingEndDeviceElement(const std::string &href,
                                      const std::string &lfdi,
                                      unsigned long long sfdi, int postRate) {
  return "<EndDevice href=\"" + href + "\" subscribable=\"0\">"
         "<DERListLink href=\"" + href + "/der\" all=\"1\"/>"
         "<lFDI>" + lfdi + "</lFDI>"
         "<sFDI>" + std::to_string(sfdi) + "</sFDI>"
         "<changedTime>1790000000</changedTime>"
         "<FunctionSetAssignmentsListLink href=\"" + href + "/fsa\" all=\"1\"/>"
         "<postRate>" + std::to_string(postRate) + "</postRate>"
         "<RegistrationLink href=\"" + href + "/rg\"/>"
         "<csipaus:ConnectionPointLink href=\"" + href + "/cp\"/>"
         "</EndDevice>";
}

std::string reportingEndDevice(const std::string &href,
                               const std::string &lfdi,
                               unsigned long long sfdi, int postRate) {
  return document(reportingEndDeviceElement(href, lfdi, sfdi, postRate));
}

std::string derList(int pollRate) {
  return document(
      "<DERList href=\"/edev/3/der\" all=\"1\" "
      "results=\"1\" pollRate=\"" + std::to_string(pollRate) + "\">"
      "<DER href=\"/edev/3/der/1\" subscribable=\"0\">"
      "<CurrentDERProgramLink href=\"/derp/1\"/>"
      "<DERAvailabilityLink href=\"/edev/3/der/1/dera\"/>"
      "<DERCapabilityLink href=\"/edev/3/der/1/dercap\"/>"
      "<DERSettingsLink href=\"/edev/3/der/1/derg\"/>"
      "<DERStatusLink href=\"/edev/3/der/1/ders\"/>"
      "</DER>"
      "</DERList>");
}

std::string mirrorUsagePointElement(const std::string &href,
                                    const std::string &mrid,
                                    const std::string &lfdi,
                                    std::optional<int> postRate) {
  return "<MirrorUsagePoint href=\"" + href + "\">"
         "<mRID>" + mrid + "</mRID>"
         "<roleFlags>03</roleFlags>"
         "<serviceCategoryKind>0</serviceCategoryKind>"
         "<status>1</status>"
         "<deviceLFDI>" + lfdi + "</deviceLFDI>" +
         (postRate ? "<postRate>" + std::to_string(*postRate) + "</postRate>"
                   : "") +
         "</MirrorUsagePoint>";
}

std::string mirrorUsagePoint(const std::string &href, const std::string &mrid,
                             const std::string &lfdi,
                             std::optional<int> postRate) {
  return document(mirrorUsagePointElement(href, mrid, lfdi, postRate));
}

std::string mirrorUsagePointList(const std::vector<std::string> &elements,
                                 int pollRate) {
  return document("<MirrorUsagePointList href=\"/mup\" "
                  "all=\"" + std::to_string(elements.size()) +
                  "\" results=\"" + std::to_string(elements.size()) +
                  "\" pollRate=\"" + std::to_string(pollRate) + "\">" +
                  join(elements) + "</MirrorUsagePointList>");
}

} // namespace grid_services::testing::csipaus_fixtures
