#include <gtest/gtest.h>
#include <xercesc/util/PlatformUtils.hpp>

int main(int argc, char **argv) {
  xercesc::XMLPlatformUtils::Initialize();

  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();

  xercesc::XMLPlatformUtils::Terminate();
  return result;
}
