// Local addition to liboadr: a VEN's oadrResponse, acknowledging a payload
// the VTN delivered in answer to oadrPoll, such as oadrRequestReregistration.

#ifndef OADRRESPONSE_H_
#define OADRRESPONSE_H_

#include <oadrsd/2b/oadrResponseType.hxx>

#include "Oadr2bRequest.h"

class OadrResponse : public Oadr2bRequest {
protected:
  virtual unique_ptr<oadrPayload> generatePayload();

public:
  OadrResponse(string venID, string responseCode, string responseDescription,
               string requestID = "");
  virtual ~OadrResponse();
};

#endif /* OADRRESPONSE_H_ */
