#include "protocols/csipaus/documents.hpp"

#include <stdexcept>
#include <vector>

namespace grid_services::csipaus {

namespace {

int hexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  throw std::invalid_argument("not a hex digit: " + std::string(1, c));
}

} // namespace

const xml_schema::namespace_infomap &namespaces() {
  static const xml_schema::namespace_infomap map = [] {
    xml_schema::namespace_infomap m;
    m[""].name = "urn:ieee:std:2030.5:ns";
    m["csipaus"].name = "https://csipaus.org/ns";
    return m;
  }();
  return map;
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

xml_schema::hex_binary hexBinary(const std::string &hex) {
  if (hex.size() % 2 != 0) {
    throw std::invalid_argument("an odd number of hex digits");
  }
  std::vector<char> bytes;
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    bytes.push_back(
        static_cast<char>(hexDigit(hex[i]) * 16 + hexDigit(hex[i + 1])));
  }
  return xml_schema::hex_binary(bytes.data(), bytes.size());
}

} // namespace grid_services::csipaus
