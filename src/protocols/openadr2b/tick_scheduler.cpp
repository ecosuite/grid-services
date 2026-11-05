#include "protocols/openadr2b/tick_scheduler.hpp"

#include <vector>

#include <oadr/scheduler/SchedulerJob.h>

namespace grid_services::openadr2b {

bool TickScheduler::addJob(std::unique_ptr<SchedulerJob> job) {
  return jobs_.addJob(std::move(job));
}

bool TickScheduler::removeJob(std::string id) { return jobs_.removeJob(id); }

size_t TickScheduler::getNumScheduledJobs() { return jobs_.size(); }

void TickScheduler::runDue(std::time_t now) {
  // Bounded so a job that keeps rescheduling itself into the past cannot
  // spin forever.
  for (size_t runs = 0; runs < 1000 && jobs_.size() > 0; ++runs) {
    jobs_.sort();
    SchedulerJob *job = jobs_.front();
    if (job->getRuntime() > now) {
      return;
    }
    const std::string id = job->getID();
    job->execute(now);
    // The job may have removed itself while running.
    const auto &map = jobs_.jobMap();
    const auto found = map.find(id);
    if (found != map.end() && found->second->isComplete()) {
      jobs_.removeJob(id);
    }
  }
}

void TickScheduler::clear() {
  std::vector<std::string> ids;
  for (const auto &[id, job] : jobs_.jobMap()) {
    ids.push_back(id);
  }
  for (const auto &id : ids) {
    jobs_.removeJob(id);
  }
}

} // namespace grid_services::openadr2b
