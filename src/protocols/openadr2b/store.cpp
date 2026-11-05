#include "protocols/openadr2b/store.hpp"

#include "core/errors.hpp"

namespace grid_services::openadr2b {

Openadr2bStore &openadr2bStore(RunnerStore &store) {
  auto *openadr2b = dynamic_cast<Openadr2bStore *>(&store);
  if (openadr2b == nullptr) {
    throw AdapterError::permanent(
        "the worker's store has no OpenADR 2.0b settings");
  }
  return *openadr2b;
}

} // namespace grid_services::openadr2b
