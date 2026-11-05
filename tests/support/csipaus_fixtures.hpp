#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// CSIP-AUS test data.
namespace grid_services::testing::csipaus_fixtures {

// A self-signed ECDSA P-256 device certificate and its key, generated for
// these tests. Its LFDI is C995EBDDFCFA53DE23DA8EA2BFFF91834D51D617 and its
// SFDI 541127305912.
inline const char *const deviceCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBnzCCAUWgAwIBAgIUTPWClZV9ilF0VykAWdRjNQNKTSowCgYIKoZIzj0EAwIw\n"
    "JDEiMCAGA1UEAwwZZ3JpZC1zZXJ2aWNlcyB0ZXN0IGRldmljZTAgFw0yNjEwMDQy\n"
    "MjUzMThaGA8yMTI2MDkxMDIyNTMxOFowJDEiMCAGA1UEAwwZZ3JpZC1zZXJ2aWNl\n"
    "cyB0ZXN0IGRldmljZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABHJxpo7pzraA\n"
    "KLty9CStAjs0nCKWM4VYvEDABUKp2xZJH0MMX09dQnvoyyJ8JVp8EZfILU0M/pPy\n"
    "/Qnfkp+KremjUzBRMB0GA1UdDgQWBBTPZnsKzXV+yPHN5A87JQ7RuIq7DDAfBgNV\n"
    "HSMEGDAWgBTPZnsKzXV+yPHN5A87JQ7RuIq7DDAPBgNVHRMBAf8EBTADAQH/MAoG\n"
    "CCqGSM49BAMCA0gAMEUCIG/0gZche/PAsUnf1u3DGdJOEuymWR2GYbyHOX5SvXrD\n"
    "AiEAzFTjEb/ijxgzb/nh2Hs0+Gfk0VSPqriAb2uQ+oTAnkc=\n"
    "-----END CERTIFICATE-----\n";

inline const char *const deviceKeyPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgCja2A5i5pVNLHgxg\n"
    "H3Kq7R9+WYzuHtFaRza9dLrZVQ+hRANCAARycaaO6c62gCi7cvQkrQI7NJwiljOF\n"
    "WLxAwAVCqdsWSR9DDF9PXUJ76MsifCVafBGXyC1NDP6T8v0J35Kfiq3p\n"
    "-----END PRIVATE KEY-----\n";

inline const char *const deviceLfdi =
    "C995EBDDFCFA53DE23DA8EA2BFFF91834D51D617";
inline constexpr unsigned long long deviceSfdi = 541127305912ull;

// Documents a CSIP-AUS utility server sends, written against the vendored
// schema. Each is a whole document unless it says it is an element for a
// list.

// Links to `time` (/tm), /edev and `mirrors` (/mup).
std::string deviceCapability(int pollRate = 86400,
                             const std::string &time = "/tm",
                             const std::string &mirrors = "/mup");

// An EndDevice element, with links under `href`: FunctionSetAssignments at
// href/fsa, Registration at href/rg unless `registration` is false, and
// ConnectionPoint at href/cp.
std::string endDeviceElement(const std::string &href, const std::string &lfdi,
                             unsigned long long sfdi, bool registration = true);
std::string endDevice(const std::string &href, const std::string &lfdi,
                      unsigned long long sfdi, bool registration = true);
// A page of an EndDeviceList at /edev holding `all` in total.
std::string endDeviceList(const std::vector<std::string> &elements,
                          int all = -1, int pollRate = 300);
std::string registration(unsigned pin);
// The server's Time at /tm.
std::string time(long long currentTime, int pollRate = 900);

// One FunctionSetAssignments whose DERProgramList is at /derp.
std::string functionSetAssignmentsList(int pollRate = 3600);
// No FunctionSetAssignments yet, as before the server has any programs.
std::string emptyFunctionSetAssignmentsList(int pollRate = 60);
// One DERProgram, "Site limits", with its DefaultDERControl at
// /derp/1/dderc and its DERControlList at /derp/1/derc.
std::string derProgramList(int pollRate = 60);
// The DefaultDERControl at /derp/1/dderc, limiting export, with `setGradW`
// unless it is null.
std::string defaultDerControl(int exportWatts,
                              std::optional<int> setGradW = 27);
// A DERControl element limiting export.
std::string derControlElement(const std::string &mrid, int currentStatus,
                              long long start, int duration, int exportWatts);
// The same, randomized by up to `randomizeStart` and `randomizeDuration`
// seconds when they are given.
std::string derControlElement(const std::string &mrid, int currentStatus,
                              long long start, int duration, int exportWatts,
                              std::optional<int> randomizeStart,
                              std::optional<int> randomizeDuration);
// A DERControl element created at `creationTime`, setting `caps`: each an
// ActivePower element (opModImpLimW, opModExpLimW, opModGenLimW or
// opModLoadLimW, in that order) and its watts, or a switch (opModConnect or
// opModEnergize, in that order and before the others), on unless 0; with
// `rampTms` when given.
std::string derControlElement(
    const std::string &mrid, int currentStatus, long long start, int duration,
    long long creationTime, const std::vector<std::pair<std::string, int>> &caps,
    const std::string &responseRequired = "03",
    std::optional<int> rampTms = std::nullopt);
std::string derControlList(const std::vector<std::string> &elements);
// Two DERPrograms: "Site limits" at /derp/1 (primacy 1), with its
// DefaultDERControl at /derp/1/dderc and its DERControlList at /derp/1/derc,
// and "Network support" at /derp/2 (primacy 2), with only a DERControlList,
// at /derp/2/derc.
std::string twoDerProgramList(int pollRate = 60);

// An EndDevice element like endDeviceElement's, with its DERList at
// href/der and a postRate.
std::string reportingEndDeviceElement(const std::string &href,
                                      const std::string &lfdi,
                                      unsigned long long sfdi, int postRate);
std::string reportingEndDevice(const std::string &href,
                               const std::string &lfdi,
                               unsigned long long sfdi, int postRate);
// The DERList at /edev/3/der with its one DER, whose capability, settings
// and status are at /edev/3/der/1/dercap, derg and ders.
std::string derList(int pollRate = 60);
// A MirrorUsagePoint element at `href` for the device.
std::string mirrorUsagePointElement(const std::string &href,
                                    const std::string &mrid,
                                    const std::string &lfdi,
                                    std::optional<int> postRate);
std::string mirrorUsagePoint(const std::string &href, const std::string &mrid,
                             const std::string &lfdi,
                             std::optional<int> postRate);
// The MirrorUsagePointList at /mup.
std::string mirrorUsagePointList(const std::vector<std::string> &elements,
                                 int pollRate = 60);

} // namespace grid_services::testing::csipaus_fixtures
