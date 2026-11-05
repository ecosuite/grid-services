#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "support/scripted_rest.hpp"

namespace grid_services::testing {

// A CACTUS run's envoy exchanges, as test-servers/cactus/capture.py copies
// them into tests/fixtures/csipaus/cactus/<TEST>/.
struct CactusFixture {
  struct Exchange {
    // "METHOD /path", as RestScript keys requests.
    std::string key;
    long status = 0;
    // When the run's proxy received the request.
    std::chrono::sys_time<std::chrono::milliseconds> requestedAt;
    // The response's body.
    std::string body;
  };

  std::vector<Exchange> exchanges;

  std::vector<std::string> keys() const;
};

// Queues each of `test`'s responses on `script` under its request's key, in
// the order the run made them.
CactusFixture loadCactusFixture(const std::string &test, RestScript &script);

} // namespace grid_services::testing
