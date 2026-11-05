// Local addition to liboadr: a VEN's oadrCanceledPartyRegistration, answering
// an oadrCancelPartyRegistration from the VTN.

#ifndef CANCELEDPARTYREGISTRATION_H_
#define CANCELEDPARTYREGISTRATION_H_

#include <oadrsd/2b/oadrCanceledPartyRegistrationType.hxx>

#include "Oadr2bRequest.h"

class CanceledPartyRegistration : public Oadr2bRequest {
private:
  string m_registrationID;

protected:
  virtual unique_ptr<oadrPayload> generatePayload();

public:
  CanceledPartyRegistration(string venID, string registrationID,
                            string responseCode, string responseDescription,
                            string requestID);
  virtual ~CanceledPartyRegistration();
};

#endif /* CANCELEDPARTYREGISTRATION_H_ */
