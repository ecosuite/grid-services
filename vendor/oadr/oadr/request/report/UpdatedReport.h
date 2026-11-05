// Local addition to liboadr: a VEN's oadrUpdatedReport, acknowledging a
// report the VTN delivered in answer to oadrPoll.

#ifndef UPDATEDREPORT_H_
#define UPDATEDREPORT_H_

#include "../Oadr2bRequest.h"

#include <oadrsd/2b/oadrUpdatedReportType.hxx>

class UpdatedReport : public Oadr2bRequest {
protected:
  virtual unique_ptr<oadrPayload> generatePayload();

public:
  UpdatedReport(string venID, string requestID, string responseCode,
                string responseDescription);
  virtual ~UpdatedReport();
};

#endif /* UPDATEDREPORT_H_ */
