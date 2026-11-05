#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <xercesc/util/PlatformUtils.hpp>

#include "crypto/sealed_box.hpp"

int main(int argc, char **argv) {
  xercesc::XMLPlatformUtils::Initialize();
  grid_services::initialiseCrypto();
  spdlog::set_level(spdlog::level::warn);

  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();

  xercesc::XMLPlatformUtils::Terminate();
  return result;
}
