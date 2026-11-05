// Local addition to liboadr.

#include "UpdatedReport.h"

UpdatedReport::UpdatedReport(string venID, string requestID,
                             string responseCode, string responseDescription)
    : Oadr2bRequest("oadrUpdatedReport", "oadrResponse", venID, responseCode,
                    responseDescription, requestID) {}

/********************************************************************************/

UpdatedReport::~UpdatedReport() {}

/********************************************************************************/

unique_ptr<oadrPayload> UpdatedReport::generatePayload() {
  oadrUpdatedReportType::eiResponse_type eir(responseCode(), requestID());
  if (responseDescription() != "")
    eir.responseDescription(responseDescription());

  oadrUpdatedReportType updated(eir);
  updated.schemaVersion("2.0b");
  updated.venID(venID());

  oadrSignedObject oso;
  oso.oadrUpdatedReport(updated);

  unique_ptr<oadrPayload> payload(new oadrPayload(oso));
  return payload;
}
