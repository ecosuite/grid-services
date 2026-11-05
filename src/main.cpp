#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <pthread.h>
#include <stop_token>
#include <string>
#include <thread>
#include <unistd.h>

#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <xercesc/util/PlatformUtils.hpp>

#include "protocols/csipaus/csipaus_adapter.hpp"
#include "protocols/openadr2b/openadr2b_adapter.hpp"
#include "crypto/sealed_box.hpp"
#include "store/pg_store.hpp"
#include "supervisor/supervisor.hpp"

namespace {

void usage() {
  std::cerr << R"(usage:
  grid-services                    run the worker
  grid-services keygen             print a new worker key pair
  grid-services seal PUBLIC_KEY    seal stdin for the worker, e.g. a client key

environment:
  GRID_SERVICES_DATABASE_URL  libpq connection string (required to run)
  GRID_SERVICES_SECRET_KEY    worker secret key; needed to open sealed client keys
  GRID_SERVICES_WORKER_ID     name recorded against running connections
                              (default: hostname)
  GRID_SERVICES_LOG_LEVEL     trace, debug, info, warn or error (default: info)
  GRID_SERVICES_CSIPAUS_PEN   the IANA Private Enterprise Number that ends the
                              mRIDs made for CSIP-AUS readings, at most
                              99999999 (default: 0)
)";
}

std::optional<std::string> environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

std::string hostname() {
  char buffer[256] = {};
  if (gethostname(buffer, sizeof buffer - 1) != 0) {
    return "grid-services";
  }
  return buffer;
}

int keygen() {
  const auto pair = grid_services::generateKeyPair();
  std::cout << "public key: " << pair.publicKeyBase64 << "\n"
            << "secret key: " << pair.secretKeyBase64 << "\n";
  return 0;
}

int seal(const std::string &publicKey) {
  const std::string plaintext(std::istreambuf_iterator<char>(std::cin), {});
  std::cout << grid_services::seal(publicKey, plaintext) << "\n";
  return 0;
}

int runWorker() {
  const auto databaseUrl = environment("GRID_SERVICES_DATABASE_URL");
  if (!databaseUrl) {
    std::cerr << "GRID_SERVICES_DATABASE_URL is not set\n";
    return 2;
  }

  std::unique_ptr<grid_services::SecretOpener> opener;
  if (const auto secretKey = environment("GRID_SERVICES_SECRET_KEY")) {
    opener = std::make_unique<grid_services::SecretOpener>(*secretKey);
    spdlog::info("worker public key: {}", opener->publicKeyBase64());
  } else {
    spdlog::warn("GRID_SERVICES_SECRET_KEY is not set; connections with sealed "
                 "client keys will not start");
  }

  // Block termination signals in every thread; one thread waits for them.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  grid_services::AdapterRegistry adapters;
  adapters.add("openadr2b", grid_services::openadr2b::makeOpenadr2bFactory(opener.get()));
  grid_services::csipaus::CsipausOptions csipausOptions;
  if (const auto pen = environment("GRID_SERVICES_CSIPAUS_PEN")) {
    unsigned long value = 0;
    try {
      value = std::stoul(*pen);
    } catch (const std::exception &) {
      std::cerr << "GRID_SERVICES_CSIPAUS_PEN is not a number\n";
      return 2;
    }
    if (value > grid_services::csipaus::maxPen) {
      std::cerr << "GRID_SERVICES_CSIPAUS_PEN is more than 99999999: mRIDs "
                   "end in it as 8 decimal digits\n";
      return 2;
    }
    csipausOptions.pen = static_cast<std::uint32_t>(value);
  }
  adapters.add("csipaus", grid_services::csipaus::makeCsipausFactory(
                              opener.get(), csipausOptions));

  grid_services::RunnerOptions runnerOptions;
  runnerOptions.workerId =
      environment("GRID_SERVICES_WORKER_ID").value_or(hostname());

  grid_services::PgSupervisorStore supervisorStore(*databaseUrl);
  const std::string connectionString = *databaseUrl;
  grid_services::Supervisor supervisor(
      supervisorStore,
      [connectionString](grid_services::ConnectionId id) {
        return std::make_unique<grid_services::PgRunnerStore>(connectionString, id);
      },
      adapters, runnerOptions);

  std::stop_source stop;
  std::thread signalWaiter([&signals, &stop] {
    int received = 0;
    sigwait(&signals, &received);
    spdlog::info("received signal {}; shutting down", received);
    stop.request_stop();
  });
  signalWaiter.detach();

  spdlog::info("worker {} starting", runnerOptions.workerId);
  supervisor.run(stop.get_token());
  spdlog::info("worker stopped");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const auto base = spdlog::default_logger();
  spdlog::set_default_logger(std::make_shared<spdlog::logger>(
      "worker", base->sinks().begin(), base->sinks().end()));
  spdlog::set_pattern("%Y-%m-%dT%H:%M:%S.%e%z %^%l%$ [%n] %v");
  if (const auto level = environment("GRID_SERVICES_LOG_LEVEL")) {
    spdlog::set_level(spdlog::level::from_str(*level));
  }

  try {
    grid_services::initialiseCrypto();

    const std::string command = argc > 1 ? argv[1] : "run";
    if (command == "keygen") {
      return keygen();
    }
    if (command == "seal" && argc == 3) {
      return seal(argv[2]);
    }
    if (command != "run") {
      usage();
      return 2;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
      std::cerr << "curl failed to initialise\n";
      return 1;
    }
    xercesc::XMLPlatformUtils::Initialize();

    const int status = runWorker();

    xercesc::XMLPlatformUtils::Terminate();
    curl_global_cleanup();
    return status;
  } catch (const std::exception &error) {
    spdlog::critical("{}", error.what());
    return 1;
  }
}
