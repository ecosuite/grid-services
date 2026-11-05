#include "protocols/csipaus/sep_client.hpp"

#include <chrono>
#include <vector>

#include <xercesc/dom/DOM.hpp>
#include <xsd/cxx/tree/error-handler.hxx>
#include <xsd/cxx/xml/dom/parsing-source.hxx>
#include <xsd/cxx/xml/sax/std-input-source.hxx>
#include <xsd/cxx/xml/string.hxx>

#include "core/types.hpp"

namespace grid_services::csipaus {

namespace {

constexpr const char *mediaType = "application/sep+xml";

std::string transcode(const XMLCh *text) {
  return text == nullptr ? "" : xsd::cxx::xml::transcode<char>(text);
}

const xercesc::DOMElement *firstChild(const xercesc::DOMElement &element) {
  return element.getFirstElementChild();
}

bool sameName(const xercesc::DOMElement &a, const xercesc::DOMElement &b) {
  return xercesc::XMLString::equals(a.getLocalName(), b.getLocalName()) &&
         xercesc::XMLString::equals(a.getNamespaceURI(), b.getNamespaceURI());
}

// Adds the paths of `original`'s descendants that `kept` lacks. The bindings
// write elements in schema order, and read only a document in that order,
// so what they kept is a subsequence of the original's children.
void findDropped(const xercesc::DOMElement &original,
                 const xercesc::DOMElement &kept, const std::string &path,
                 std::vector<std::string> &dropped) {
  const auto *keptChild = firstChild(kept);
  for (const auto *child = firstChild(original); child != nullptr;
       child = child->getNextElementSibling()) {
    const auto childPath = path + "/" + transcode(child->getTagName());
    if (keptChild != nullptr && sameName(*child, *keptChild)) {
      findDropped(*child, *keptChild, childPath, dropped);
      keptChild = keptChild->getNextElementSibling();
    } else {
      auto entry = childPath;
      if (const auto ns = transcode(child->getNamespaceURI()); !ns.empty()) {
        entry += " (" + ns + ")";
      }
      dropped.push_back(entry);
    }
  }
}

// "403", with the reason a 2030.5 Error body gives, if it is one.
std::string describeStatus(const http::Response &response) {
  std::string text = std::to_string(response.status);
  if (response.body.empty()) {
    return text;
  }
  try {
    std::istringstream body(response.body);
    const auto error = sep::Error_(body, parseFlags);
    text += ", reason code " + std::to_string(error->reasonCode());
    if (error->maxRetryDuration()) {
      text += ", retry within " +
              std::to_string(*error->maxRetryDuration()) + "s";
    }
  } catch (const std::exception &) {
    // Not an Error document; the status says enough.
  }
  return text;
}

std::string now() {
  return toIso8601(std::chrono::system_clock::now());
}

} // namespace

SepClient::SepClient(std::string dcapUrl,
                     std::unique_ptr<http::HttpClient> http)
    : http_(std::move(http)) {
  const auto scheme = dcapUrl.find("://");
  if (scheme == std::string::npos) {
    throw AdapterError::permanent("the DeviceCapability URL " + dcapUrl +
                                  " is not absolute");
  }
  const auto path = dcapUrl.find('/', scheme + 3);
  origin_ = dcapUrl.substr(0, path);
  dcapHref_ = path == std::string::npos ? "/" : dcapUrl.substr(path);
}

std::string SepClient::url(const std::string &href) const {
  if (href.find("://") != std::string::npos) {
    return href;
  }
  return origin_ + (href.empty() || href.front() != '/' ? "/" : "") + href;
}

std::string SepClient::hrefOf(const std::string &location) const {
  if (location.rfind(origin_ + "/", 0) == 0) {
    return location.substr(origin_.size());
  }
  return location;
}

std::string SepClient::withPage(const std::string &href, std::size_t start) {
  return href + (href.find('?') == std::string::npos ? "?" : "&") +
         "s=" + std::to_string(start) + "&l=" + std::to_string(pageSize);
}

http::Response SepClient::exchange(http::Method method,
                                   const std::string &href, std::string body) {
  http::Request request;
  request.method = method;
  request.url = url(href);
  request.headers = {std::string("Accept: ") + mediaType};
  if (method == http::Method::Post || method == http::Method::Put) {
    request.headers.push_back(std::string("Content-Type: ") + mediaType);
  }
  request.body = std::move(body);

  const auto what = http::toString(method) + " " + href;
  exchange_.push_back({{"direction", "to_program"},
                       {"at", now()},
                       {"format", "xml"},
                       {"method", http::toString(method)},
                       {"url", request.url},
                       {"body", request.body}});
  auto response = http_->send(request);
  exchange_.push_back({{"direction", "from_program"},
                       {"at", now()},
                       {"format", "xml"},
                       {"status", response.status},
                       {"body", response.body}});

  if (response.ok()) {
    return response;
  }
  const auto message = what + " gave " + describeStatus(response);
  if (response.status == 404) {
    throw NotFound(message);
  }
  if (response.status == 429 || response.status >= 500) {
    throw AdapterError::transient(message);
  }
  throw AdapterError::permanent(message);
}

nlohmann::json SepClient::takeExchange() {
  if (exchange_.empty()) {
    return nullptr;
  }
  auto taken = std::move(exchange_);
  exchange_ = nlohmann::json::array();
  return taken;
}

xml_schema::dom::unique_ptr<xercesc::DOMDocument>
SepClient::parseDom(const std::string &body, const std::string &href) const {
  std::istringstream stream(body);
  xsd::cxx::xml::sax::std_input_source source(stream);
  xsd::cxx::tree::error_handler<char> handler;
  auto document = xsd::cxx::xml::dom::parse<char>(
      source, handler, xml_schema::properties(), parseFlags);
  try {
    handler.throw_if_failed<xsd::cxx::tree::parsing<char>>();
  } catch (const xml_schema::exception &error) {
    throw AdapterError::transient("the server's answer to GET " + href +
                                  " is not XML: " + describe(error));
  }
  if (!document) {
    throw AdapterError::transient("the server's answer to GET " + href +
                                  " is empty");
  }
  return document;
}

void SepClient::checkNothingDropped(const xercesc::DOMDocument &original,
                                    const xercesc::DOMDocument &kept,
                                    const std::string &href) {
  const auto *root = original.getDocumentElement();
  std::vector<std::string> dropped;
  findDropped(*root, *kept.getDocumentElement(),
              transcode(root->getTagName()), dropped);
  if (dropped.empty()) {
    return;
  }
  std::string list;
  for (const auto &path : dropped) {
    list += (list.empty() ? "" : ", ") + path;
  }
  throw AdapterError::transient("the server's " +
                                transcode(root->getTagName()) + " at " + href +
                                " has elements this worker does not read: " +
                                list);
}

std::string SepClient::describe(const xml_schema::exception &error) {
  std::ostringstream text;
  text << error;
  auto message = text.str();
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == ' ')) {
    message.pop_back();
  }
  return message;
}

} // namespace grid_services::csipaus
