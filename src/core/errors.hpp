#pragma once

#include <stdexcept>
#include <string>

namespace grid_services {

// Raised by adapters. The runner decides what to do from the kind:
//
//   Transient  network trouble or a server error; retry with backoff
//   Permanent  needs an operator: bad credentials, rejected registration,
//              invalid configuration
//   Stopped    the runner is stopping and the operation was abandoned
class AdapterError : public std::runtime_error {
public:
  enum class Kind { Transient, Permanent, Stopped };

  AdapterError(Kind kind, const std::string &message)
      : std::runtime_error(message), kind_(kind) {}

  Kind kind() const { return kind_; }

  static AdapterError transient(const std::string &message) {
    return {Kind::Transient, message};
  }
  static AdapterError permanent(const std::string &message) {
    return {Kind::Permanent, message};
  }
  static AdapterError stopped() { return {Kind::Stopped, "stopped"}; }

private:
  Kind kind_;
};

} // namespace grid_services
