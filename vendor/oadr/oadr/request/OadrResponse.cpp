// Local addition to liboadr.

#include "OadrResponse.h"

OadrResponse::OadrResponse(string venID, string responseCode,
                           string responseDescription, string requestID)
    : Oadr2bRequest("oadrResponse", "oadrResponse", venID, responseCode,
                    responseDescription, requestID) {}

/********************************************************************************/

OadrResponse::~OadrResponse() {}

/********************************************************************************/

unique_ptr<oadrPayload> OadrResponse::generatePayload() {
  oadrResponseType::eiResponse_type eir(responseCode(), requestID());
  if (responseDescription() != "")
    eir.responseDescription(responseDescription());

  oadrResponseType response(eir);
  response.schemaVersion("2.0b");
  response.venID(venID());

  oadrSignedObject oso;
  oso.oadrResponse(response);

  unique_ptr<oadrPayload> payload(new oadrPayload(oso));
  return payload;
}
