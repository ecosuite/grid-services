#include "support/cactus_fixture.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace grid_services::testing {

namespace {

std::string readFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot read the CACTUS fixture file " + path);
  }
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

std::chrono::sys_time<std::chrono::milliseconds> fromEpoch(double seconds) {
  return std::chrono::sys_time<std::chrono::milliseconds>(
      std::chrono::milliseconds(std::llround(seconds * 1000)));
}

} // namespace

std::vector<std::string> CactusFixture::keys() const {
  std::vector<std::string> keys;
  for (const auto &exchange : exchanges) {
    keys.push_back(exchange.key);
  }
  return keys;
}

CactusFixture loadCactusFixture(const std::string &test, RestScript &script) {
  const auto directory =
      std::string(GRID_SERVICES_FIXTURES_DIR) + "/csipaus/cactus/" + test;
  const auto manifest =
      nlohmann::json::parse(readFile(directory + "/manifest.json"));

  CactusFixture fixture;
  for (const auto &entry : manifest.at("exchanges")) {
    const auto key = entry.at("method").get<std::string>() + " " +
                     entry.at("path").get<std::string>();
    const auto status = entry.at("status").get<long>();

    RestScript::Response response{status, "", {}, std::nullopt};
    if (entry.contains("body")) {
      response.body =
          readFile(directory + "/" + entry.at("body").get<std::string>());
    }
    if (entry.contains("location")) {
      response.headers.emplace("Location",
                               entry.at("location").get<std::string>());
    }
    fixture.exchanges.push_back({key, status,
                                 fromEpoch(entry.at("epoch").get<double>()),
                                 response.body});
    script.queue(key, std::move(response));
  }
  return fixture;
}

} // namespace grid_services::testing
