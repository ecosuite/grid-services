#pragma once

#include <memory>
#include <ostream>
#include <string>

#include <csipaussd/v11/csipaus-ext.hxx>
#include <csipaussd/v11/sep.hxx>

// The documents the client exchanges with a utility server, and how each is
// read and written with the generated bindings.
namespace grid_services::csipaus {

namespace sep = ::csipaus::sep;
namespace ext = ::csipaus::ext;
namespace xml_schema = ::csipaus::xml_schema;

// IEEE 2030.5 as the default namespace and CSIP-AUS v1.1 under csipaus:.
const xml_schema::namespace_infomap &namespaces();

// Upper-case hex digits, as 2030.5 writes identifiers.
std::string hexString(const xml_schema::hex_binary &value);
// Throws std::invalid_argument for anything but an even number of hex digits.
xml_schema::hex_binary hexBinary(const std::string &hex);

// Xerces is initialised by the process, and the IEEE schema is not shipped,
// so documents are read without validation. What validation would catch is
// checked by SepClient instead.
inline const xml_schema::flags parseFlags =
    xml_schema::flags::dont_initialize | xml_schema::flags::dont_validate;
inline const xml_schema::flags writeFlags =
    xml_schema::flags::dont_initialize | xml_schema::flags::no_xml_declaration;

// Document<T>::name, parse(const DOMDocument &), toDom(const T &) and
// write(std::ostream &, const T &) for each document.
template <typename T> struct Document;

#define CSIPAUS_DOCUMENT(Type, Function)                                       \
  template <> struct Document<Type> {                                          \
    static constexpr const char *name = #Type;                                 \
    static std::unique_ptr<Type> parse(const xercesc::DOMDocument &document) {  \
      return Function(document, parseFlags);                                   \
    }                                                                          \
    static xml_schema::dom::unique_ptr<xercesc::DOMDocument>                   \
    toDom(const Type &value) {                                                 \
      return Function(value, namespaces(), writeFlags);                        \
    }                                                                          \
    static void write(std::ostream &out, const Type &value) {                  \
      Function(out, value, namespaces(), "UTF-8", writeFlags);                 \
    }                                                                          \
  }

CSIPAUS_DOCUMENT(sep::DeviceCapability, sep::DeviceCapability_);
CSIPAUS_DOCUMENT(sep::Time, sep::Time_);
CSIPAUS_DOCUMENT(sep::EndDeviceList, sep::EndDeviceList_);
CSIPAUS_DOCUMENT(sep::EndDevice, sep::EndDevice_);
CSIPAUS_DOCUMENT(sep::Registration, sep::Registration_);
CSIPAUS_DOCUMENT(ext::ConnectionPointType, ext::ConnectionPoint);
CSIPAUS_DOCUMENT(sep::FunctionSetAssignmentsList,
                 sep::FunctionSetAssignmentsList_);
CSIPAUS_DOCUMENT(sep::FunctionSetAssignments, sep::FunctionSetAssignments_);
CSIPAUS_DOCUMENT(sep::DERProgramList, sep::DERProgramList_);
CSIPAUS_DOCUMENT(sep::DERProgram, sep::DERProgram_);
CSIPAUS_DOCUMENT(sep::DERControlList, sep::DERControlList_);
CSIPAUS_DOCUMENT(sep::DERControl, sep::DERControl_);
CSIPAUS_DOCUMENT(sep::DefaultDERControl, sep::DefaultDERControl_);
CSIPAUS_DOCUMENT(sep::DERControlResponse, sep::DERControlResponse_);
CSIPAUS_DOCUMENT(sep::DERList, sep::DERList_);
CSIPAUS_DOCUMENT(sep::DER, sep::DER_);
CSIPAUS_DOCUMENT(sep::DERCapability, sep::DERCapability_);
CSIPAUS_DOCUMENT(sep::DERSettings, sep::DERSettings_);
CSIPAUS_DOCUMENT(sep::DERStatus, sep::DERStatus_);
CSIPAUS_DOCUMENT(sep::DERAvailability, sep::DERAvailability_);
CSIPAUS_DOCUMENT(sep::MirrorUsagePointList, sep::MirrorUsagePointList_);
CSIPAUS_DOCUMENT(sep::MirrorUsagePoint, sep::MirrorUsagePoint_);
CSIPAUS_DOCUMENT(sep::MirrorMeterReadingList, sep::MirrorMeterReadingList_);
CSIPAUS_DOCUMENT(sep::MirrorMeterReading, sep::MirrorMeterReading_);
CSIPAUS_DOCUMENT(sep::Error, sep::Error_);

#undef CSIPAUS_DOCUMENT

// ListItems<T>::items(T &) for each list document, so lists can be read a
// page at a time.
template <typename T> struct ListItems;

#define CSIPAUS_LIST(Type, Items)                                              \
  template <> struct ListItems<Type> {                                         \
    static auto &items(Type &list) { return list.Items(); }                    \
  }

CSIPAUS_LIST(sep::EndDeviceList, EndDevice);
CSIPAUS_LIST(sep::FunctionSetAssignmentsList, FunctionSetAssignments);
CSIPAUS_LIST(sep::DERProgramList, DERProgram);
CSIPAUS_LIST(sep::DERControlList, DERControl);
CSIPAUS_LIST(sep::DERList, DER);
CSIPAUS_LIST(sep::MirrorUsagePointList, MirrorUsagePoint);
CSIPAUS_LIST(sep::MirrorMeterReadingList, MirrorMeterReading);

#undef CSIPAUS_LIST

// A list document, read a page at a time.
template <typename T>
concept PagedList = requires(T &list) { ListItems<T>::items(list); };

} // namespace grid_services::csipaus
