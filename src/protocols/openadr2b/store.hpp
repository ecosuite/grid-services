#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/store.hpp"
#include "protocols/openadr2b/reports.hpp"

namespace grid_services::openadr2b {

// A connection's OpenADR 2.0b settings. The client certificate is the
// connection's client credential, which is not OpenADR's own.
struct Openadr2bSettings {
  std::string vtnUrl;
  std::string venName;
  std::optional<std::string> venId;
  std::optional<std::string> marketContext;
  std::optional<std::string> caCertPem;
  bool verifyServerCertificate = true;
  // Signals the site can act on. Events with any other signal are refused.
  std::vector<std::string> acceptedSignals{"SIMPLE"};
};

// Database access for the OpenADR 2.0b tables, on the runner's own session.
// A runner store that serves OpenADR connections implements it beside
// RunnerStore.
class Openadr2bStore {
public:
  virtual ~Openadr2bStore() = default;

  virtual Openadr2bSettings loadOpenadr2bSettings() = 0;
  virtual std::vector<ReportOfferRow> loadReportOffers() = 0;

  virtual void recordReportRequest(const ReportRequest &request) = 0;
  // Ends the active request with the program's id, if there is one.
  virtual void endReportRequest(const std::string &programRequestId,
                                ReportRequestStatus status) = 0;
  // Ends every active request. Returns how many there were.
  virtual int endAllReportRequests(ReportRequestStatus status) = 0;
};

// The OpenADR side of a runner store. Throws AdapterError::permanent when
// the store has none.
Openadr2bStore &openadr2bStore(RunnerStore &store);

} // namespace grid_services::openadr2b
