#pragma once

#include <ctime>
#include <memory>
#include <string>

#include <oadr/scheduler/IScheduler.h>
#include <oadr/scheduler/SchedulerJobs.h>

namespace grid_services::openadr2b {

// A liboadr scheduler driven by the runner thread instead of its own
// thread, so report jobs never run concurrently with polling.
class TickScheduler : public IScheduler {
public:
  bool addJob(std::unique_ptr<SchedulerJob> job) override;
  bool removeJob(std::string id) override;
  void executeNext() override {}
  size_t getNumScheduledJobs() override;
  void run() override {}
  void stop() override {}

  // Runs every job due at or before `now`.
  void runDue(std::time_t now);

  void clear();

private:
  SchedulerJobs jobs_;
};

} // namespace grid_services::openadr2b
