#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/adapter.hpp"
#include "datum/datum.hpp"
#include "protocols/csipaus/reporting.hpp"
#include "protocols/csipaus/resource_schedule.hpp"
#include "protocols/csipaus/sep_client.hpp"

namespace grid_services::csipaus {

// How long after a window ends its readings are posted, at the soonest,
// so that datums the site uploads late are in it.
inline constexpr std::chrono::seconds readingSettle{20};
// How long after a window of `postRate` ends it is posted, at the soonest:
// readingSettle, but no more than a quarter of the post rate, so that a site
// whose datums are in by then posts within half of it (CSIP-AUS v1.2
// explainer, section 8.1).
std::chrono::seconds windowPostDelay(std::chrono::seconds postRate);
// How often a window whose stream has not reached SolarNetwork past its end
// is looked at again.
inline constexpr std::chrono::seconds readingRecheck{10};
// The windows still posted late, after the server could not be reached or
// while a stream's datums had not arrived: the CSIP-AUS v1.2 explainer's
// section 8.3 asks for six or more.
inline constexpr int readingBacklog = 12;
// How long a description the server refused waits before it is sent again.
inline constexpr std::chrono::seconds describeRetry{300};

// What a connection reports, and where its values come from.
struct ReportingSetup {
  std::optional<DerDescription> der;
  StatePoints state;
  std::vector<BoundReading> readings;
  // Null when the connection has no SolarNetwork credential, and so no
  // readings and no state.
  std::shared_ptr<datum::DatumReader> reader;
  // The IANA Private Enterprise Number that ends the mRIDs the worker makes.
  std::uint32_t pen = 0;
};

// Sends a CSIP-AUS server what the device says about itself, as it falls
// due:
//
//   - DERCapability and DERSettings, once per registration and when they
//     change, which restarts the connection. DERSettings waits for a walk
//     of the programs, and carries the setGradW of the default in force,
//     else the DER's own; when that changes it is sent again, alone, with
//     the walk's time as its updatedTime;
//   - the readings, one MirrorMeterReadingList per stream per window of its
//     mirror usage point's post rate, aligned to the server's clock, once
//     SolarNetwork has a datum of the stream at or after the window's end;
//   - DERStatus, from the latest values of the DER's connected and operating
//     state points, at the DERList's poll rate and when it changes. A
//     connected value of 0 read after one of 1, good or stale, is a
//     disconnection, which takeDisconnected gives once.
//
// Every document sent, or attempted, is a report delivery. Each stream's
// windows are posted oldest first, as late as they must be: a window whose
// datums have not all arrived waits for them, and is never posted from part
// of them, and one the server did not take is posted again a post rate
// later, as long as it is among the last readingBacklog. A window still
// waiting for its datums when it is the oldest of those is recorded, as not
// posted, and left. A DER that is not described, or whose
// state is not bound, sends no DERStatus: a failed delivery says so at the
// DERList's poll rate.
class Reporter {
public:
  Reporter(ReportingSetup setup, SepClient &client, std::string lfdi,
           ResourceSchedule &schedule, std::function<Timestamp()> wallClock,
           const std::chrono::seconds &clockOffset);

  // Whether there is anything to send.
  bool reports() const;
  // Forgets what the server has, as after registering again.
  void reset();
  // Takes what a walk of the programs found: the setGradW of the default in
  // force, if one sets it, at the walk's time by the server's clock.
  void walked(std::optional<int> setGradW, Timestamp at);
  // Sends what is due, to the device's DERList and the mirror usage points
  // the DeviceCapability lists. Throws AdapterError::transient when
  // something could not be sent; it is sent again later.
  void run(const sep::EndDevice &device, const sep::DeviceCapability &dcap,
           ProgramEventSink &sink);
  // How long until something is due: a window, a retry, the next DERStatus,
  // or a list to fetch again; a second at the soonest. Nothing when nothing
  // is scheduled.
  std::optional<std::chrono::milliseconds> dueIn() const;
  // Whether the DER disconnected since this was last asked.
  bool takeDisconnected();

private:
  // The readings a mirror takes from one stream, posted together.
  struct StreamState {
    datum::NodeId nodeId = 0;
    std::string sourceId;
    std::vector<BoundReading> readings;
    // The start of the next window to post.
    std::optional<Timestamp> next;
    // When the next window is tried again: after the server did not take
    // it, or while its datums have not all arrived.
    std::optional<Timestamp> retryAt;
  };

  struct MirrorState {
    Mirror mirror = Mirror::Site;
    std::string mrid;
    std::vector<BoundReading> readings;
    std::vector<StreamState> streams;
    std::optional<std::string> href;
    // Whether the worker has posted it, with its readings, since it
    // started.
    bool defined = false;
    std::chrono::seconds postRate = defaultPostRate;
  };

  enum class WindowOutcome {
    // Posted, or recorded as not posted: the next window follows.
    Done,
    // The stream's datums have not all arrived.
    Waiting,
    // The server did not take it.
    Refused,
  };

  Timestamp serverNow() const;
  void refreshDer(const sep::EndDevice &device);
  void describeDer(ProgramEventSink &sink, std::vector<std::string> &failures);
  // Whether DERSettings has something the server has not taken.
  bool settingsDue() const;
  // Finds the mirror usage points, posting those the server lacks, and
  // their post rates.
  void refreshMirrors(const sep::EndDevice &device,
                      const sep::DeviceCapability &dcap,
                      ProgramEventSink &sink);
  void postDue(MirrorState &mirror, StreamState &stream,
               ProgramEventSink &sink, std::vector<std::string> &failures);
  // Posts one window of a stream's readings once its datums have arrived,
  // or, when it is the `last` chance, records that they did not.
  WindowOutcome postWindow(MirrorState &mirror, StreamState &stream,
                           Timestamp start, bool last, ProgramEventSink &sink,
                           std::vector<std::string> &failures);
  void reportStatus(ProgramEventSink &sink, std::vector<std::string> &failures);
  // Why no DERStatus can be sent, if none can.
  std::optional<std::string> unboundStatus() const;
  // The latest value of each state point, recording the reads on `delivery`.
  std::pair<StateReading, StateReading> readState(ReportDelivery &delivery);
  // Takes a read of the connected state: 0 after 1 is a disconnection. A
  // read with no value leaves the last value as it was.
  void noteConnected(const datum::Sample &sample);
  // Sends a document and records it as a delivery. Returns false, and adds
  // to `failures`, when the server did not take it.
  template <typename Send>
  bool deliver(ReportDelivery delivery, const std::string &what, Send send,
               ProgramEventSink &sink, std::vector<std::string> &failures);

  ReportingSetup setup_;
  SepClient &client_;
  std::string lfdi_;
  ResourceSchedule &schedule_;
  std::function<Timestamp()> wallClock_;
  const std::chrono::seconds &clockOffset_;

  std::vector<MirrorState> mirrors_;
  std::optional<std::string> mirrorListHref_;
  // Whether the lists could not be fetched on the last run.
  bool derListFailed_ = false;
  bool mirrorListFailed_ = false;
  // The DER's resources, from the device's DERList.
  std::optional<std::string> derListHref_;
  std::chrono::seconds derListPollRate_{900};
  std::optional<std::string> capabilityHref_;
  std::optional<std::string> settingsHref_;
  std::optional<std::string> statusHref_;
  bool capabilityDescribed_ = false;
  std::optional<Timestamp> capabilityRetryAt_;
  // Whether the programs have been walked since registering, so that the
  // setGradW in force is known.
  bool walked_ = false;
  // DERSettings' setGradW and updatedTime: the setGradW in force, else the
  // DER's, and when it took that value.
  int setGradW_ = 0;
  Timestamp settingsUpdatedAt_{};
  // The setGradW the server last took in DERSettings.
  std::optional<int> settingsTaken_;
  std::optional<Timestamp> settingsRetryAt_;
  std::optional<DerState> lastStatus_;
  std::optional<Timestamp> statusDueAt_;
  bool statusFailed_ = false;
  // The last connected value read, 1 or 0, since registering.
  std::optional<double> lastConnected_;
  bool disconnected_ = false;
};

} // namespace grid_services::csipaus
