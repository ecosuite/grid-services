// Local addition to liboadr.

#include "CanceledPartyRegistration.h"

CanceledPartyRegistration::CanceledPartyRegistration(
    string venID, string registrationID, string responseCode,
    string responseDescription, string requestID)
    : Oadr2bRequest("oadrCanceledPartyRegistration", "oadrResponse", venID,
                    responseCode, responseDescription, requestID),
      m_registrationID(registrationID) {}

/********************************************************************************/

CanceledPartyRegistration::~CanceledPartyRegistration() {}

/********************************************************************************/

unique_ptr<oadrPayload> CanceledPartyRegistration::generatePayload() {
  oadrCanceledPartyRegistrationType::eiResponse_type eir(responseCode(),
                                                         requestID());
  if (responseDescription() != "")
    eir.responseDescription(responseDescription());

  oadrCanceledPartyRegistrationType canceled(eir);
  canceled.schemaVersion("2.0b");
  if (m_registrationID != "")
    canceled.registrationID(m_registrationID);
  canceled.venID(venID());

  oadrSignedObject oso;
  oso.oadrCanceledPartyRegistration(canceled);

  unique_ptr<oadrPayload> payload(new oadrPayload(oso));
  return payload;
}
