#include "helper/documents.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace csipaus_test {

std::istringstream fixture(const std::string &name) {
  const std::string path = std::string(CSIPAUS_FIXTURES_DIR) + "/" + name;
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot read fixture " + path);
  }
  std::ostringstream content;
  content << file.rdbuf();
  return std::istringstream(content.str());
}

xml_schema::namespace_infomap namespaces() {
  xml_schema::namespace_infomap map;
  map[""].name = "urn:ieee:std:2030.5:ns";
  map["csipaus"].name = "https://csipaus.org/ns";
  return map;
}

xml_schema::hex_binary hex(std::initializer_list<unsigned char> bytes) {
  const std::vector<char> data(bytes.begin(), bytes.end());
  return xml_schema::hex_binary(data.data(), data.size());
}

std::string hexString(const xml_schema::hex_binary &value) {
  static const char digits[] = "0123456789ABCDEF";
  std::string text;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto byte = static_cast<unsigned char>(value.data()[i]);
    text += digits[byte >> 4];
    text += digits[byte & 0x0f];
  }
  return text;
}

} // namespace csipaus_test
