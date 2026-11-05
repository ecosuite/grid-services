#pragma once

#include <initializer_list>
#include <sstream>
#include <string>

#include <csipaussd/v11/xml-schema.hxx>

// Reading and writing CSIP-AUS documents in tests, the way a client does:
// documents are parsed without schema validation, as the IEEE schema is not
// in the repository, and Xerces is initialised once by main.
namespace csipaus_test {

namespace xml_schema = ::csipaus::xml_schema;

inline const xml_schema::flags parseFlags =
    xml_schema::flags::dont_initialize | xml_schema::flags::dont_validate;
inline const xml_schema::flags serializeFlags =
    xml_schema::flags::dont_initialize;

// A fixture from fixtures/, as a stream to parse.
std::istringstream fixture(const std::string &name);

// IEEE 2030.5 as the default namespace and the CSIP-AUS extensions under
// the csipaus prefix, as CSIP-AUS documents are written.
xml_schema::namespace_infomap namespaces();

xml_schema::hex_binary hex(std::initializer_list<unsigned char> bytes);

// Upper case, as CSIP-AUS writes identifiers.
std::string hexString(const xml_schema::hex_binary &value);

} // namespace csipaus_test
