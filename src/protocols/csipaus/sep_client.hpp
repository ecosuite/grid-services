#pragma once

#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/errors.hpp"
#include "http/http_client.hpp"
#include "protocols/csipaus/documents.hpp"

namespace grid_services::csipaus {

// The server has no resource at the href. Transient: a link that has gone
// stale is found again by walking from DeviceCapability.
class NotFound : public AdapterError {
public:
  explicit NotFound(const std::string &message)
      : AdapterError(Kind::Transient, message) {}
};

// The client side of IEEE 2030.5's REST interface: resources are found by
// following hrefs from DeviceCapability, and read and written as
// application/sep+xml. Every exchange is recorded for takeExchange.
//
// A document is refused when it has elements the bindings did not read, as
// they would be dropped without a word: an extension of another CSIP-AUS
// version, or of a vendor, could hide a limit. A refusal is transient, so
// the connection keeps trying and shows the error.
class SepClient {
public:
  // `dcapUrl` is the server's DeviceCapability URL; hrefs are resolved
  // against its scheme and authority.
  SepClient(std::string dcapUrl, std::unique_ptr<http::HttpClient> http);

  const std::string &dcapHref() const { return dcapHref_; }

  // The resource at `href`. Throws NotFound for a 404, and AdapterError for
  // other failures.
  template <typename T> std::unique_ptr<T> get(const std::string &href) {
    const auto response = exchange(http::Method::Get, href, "");
    return read<T>(response, href);
  }

  // As get, but null when the server has no resource at `href`.
  template <typename T> std::unique_ptr<T> find(const std::string &href) {
    try {
      return get<T>(href);
    } catch (const NotFound &) {
      return nullptr;
    }
  }

  // Every item of a list, read `pageSize` at a time: the first page with
  // the items of the rest appended.
  template <typename T> std::unique_ptr<T> getList(const std::string &href) {
    auto list = get<T>(withPage(href, 0));
    auto &items = ListItems<T>::items(*list);
    const auto all = static_cast<std::size_t>(list->all());
    for (int page = 1; page < maxPages && items.size() < all; ++page) {
      const auto next = get<T>(withPage(href, items.size()));
      auto &more = ListItems<T>::items(*next);
      if (more.empty()) {
        break;
      }
      for (const auto &item : more) {
        items.push_back(item);
      }
    }
    return list;
  }

  // Creates a resource under `href` and returns the new resource's href,
  // from the response's Location.
  template <typename T>
  std::string post(const std::string &href, const T &document) {
    const auto response = exchange(http::Method::Post, href, write(document));
    const auto location = response.header("Location");
    if (!location) {
      throw AdapterError::transient("POST " + href + " to the server gave " +
                                    std::to_string(response.status) +
                                    " with no Location");
    }
    return hrefOf(*location);
  }

  template <typename T> void put(const std::string &href, const T &document) {
    exchange(http::Method::Put, href, write(document));
  }

  // POSTs a document that creates nothing the client follows, such as a
  // Response, so no Location is needed.
  template <typename T> void send(const std::string &href, const T &document) {
    exchange(http::Method::Post, href, write(document));
  }

  // The messages exchanged since the last call, and clears them. Null when
  // there were none. See Adapter::takeExchange.
  nlohmann::json takeExchange();

  static constexpr std::size_t pageSize = 100;
  static constexpr int maxPages = 100;

private:
  // Sends a request and records it and its response. Throws for anything
  // but a 2xx.
  http::Response exchange(http::Method method, const std::string &href,
                          std::string body);
  std::string url(const std::string &href) const;
  // An href for `url`, which may be absolute.
  std::string hrefOf(const std::string &url) const;
  static std::string withPage(const std::string &href, std::size_t start);
  // Parses a response body into a DOM. Throws AdapterError when it is not
  // XML.
  xml_schema::dom::unique_ptr<xercesc::DOMDocument>
  parseDom(const std::string &body, const std::string &href) const;
  // Throws when `original` has elements `kept` does not.
  static void checkNothingDropped(const xercesc::DOMDocument &original,
                                  const xercesc::DOMDocument &kept,
                                  const std::string &href);

  template <typename T>
  std::unique_ptr<T> read(const http::Response &response,
                          const std::string &href) {
    const auto dom = parseDom(response.body, href);
    std::unique_ptr<T> document;
    try {
      document = Document<T>::parse(*dom);
    } catch (const xml_schema::exception &error) {
      throw AdapterError::transient("cannot read the server's " +
                                    std::string(Document<T>::name) + " at " +
                                    href + ": " + describe(error));
    }
    checkNothingDropped(*dom, *Document<T>::toDom(*document), href);
    return document;
  }

  template <typename T> static std::string write(const T &document) {
    std::ostringstream out;
    Document<T>::write(out, document);
    return out.str();
  }

  static std::string describe(const xml_schema::exception &error);

  std::string origin_;
  std::string dcapHref_;
  std::unique_ptr<http::HttpClient> http_;
  nlohmann::json exchange_ = nlohmann::json::array();
};

} // namespace grid_services::csipaus
