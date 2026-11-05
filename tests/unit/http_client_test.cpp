#include <gtest/gtest.h>

#include <atomic>

#include "core/errors.hpp"
#include "http/curl_http_client.hpp"
#include "support/loopback_server.hpp"

using namespace grid_services;
using namespace grid_services::http;
using grid_services::testing::LoopbackServer;

namespace {

CurlHttpClient client(const std::atomic<bool> *abort = nullptr) {
  return CurlHttpClient({}, std::chrono::seconds(5), std::chrono::seconds(5),
                        abort);
}

} // namespace

TEST(CurlHttpClient, GetsWithTheGivenHeaders) {
  LoopbackServer server("200 OK\r\nContent-Type: application/sep+xml\r\n",
                        "<DeviceCapability/>");
  auto http = client();
  Request request;
  request.url = server.url("/dcap");
  request.headers = {"Accept: application/sep+xml"};

  const auto response = http.send(request);

  EXPECT_EQ(response.status, 200);
  EXPECT_TRUE(response.ok());
  EXPECT_EQ(response.body, "<DeviceCapability/>");
  EXPECT_EQ(response.header("content-type"), "application/sep+xml");
  const auto &received = server.received();
  EXPECT_EQ(received.method, "GET");
  EXPECT_EQ(received.target, "/dcap");
  ASSERT_EQ(received.headers.count("accept"), 1u);
  EXPECT_EQ(received.headers.find("accept")->second, "application/sep+xml");
  EXPECT_TRUE(received.body.empty());
}

TEST(CurlHttpClient, PostsABodyAndReadsTheLocation) {
  LoopbackServer server("201 Created\r\nLocation: /edev/3\r\n");
  auto http = client();
  Request request;
  request.method = Method::Post;
  request.url = server.url("/edev");
  request.body = "<EndDevice/>";
  request.headers = {"Content-Type: application/sep+xml"};

  const auto response = http.send(request);

  EXPECT_EQ(response.status, 201);
  EXPECT_EQ(response.header("Location"), "/edev/3");
  const auto &received = server.received();
  EXPECT_EQ(received.method, "POST");
  EXPECT_EQ(received.body, "<EndDevice/>");
  EXPECT_EQ(received.headers.find("content-type")->second,
            "application/sep+xml");
  EXPECT_EQ(received.headers.count("expect"), 0u);
}

TEST(CurlHttpClient, Puts) {
  LoopbackServer server("204 No Content\r\n");
  auto http = client();
  Request request;
  request.method = Method::Put;
  request.url = server.url("/edev/3/cp");
  request.body = "<ConnectionPoint/>";

  EXPECT_EQ(http.send(request).status, 204);
  EXPECT_EQ(server.received().method, "PUT");
  EXPECT_EQ(server.received().body, "<ConnectionPoint/>");
}

TEST(CurlHttpClient, ReturnsErrorStatusesRatherThanThrowing) {
  LoopbackServer server("404 Not Found\r\n", "<Error/>");
  auto http = client();
  Request request;
  request.url = server.url("/edev/9");

  const auto response = http.send(request);

  EXPECT_EQ(response.status, 404);
  EXPECT_FALSE(response.ok());
  EXPECT_EQ(response.body, "<Error/>");
}

TEST(CurlHttpClient, AnUnreachableServerIsTransient) {
  auto http = client();
  try {
    // Nothing listens on port 1.
    http.send({Method::Get, "http://127.0.0.1:1/", "", {}});
    FAIL() << "expected an AdapterError";
  } catch (const AdapterError &error) {
    EXPECT_EQ(error.kind(), AdapterError::Kind::Transient);
  }
}

TEST(CurlHttpClient, StopsWhenAborted) {
  std::atomic<bool> abort{true};
  auto http = client(&abort);
  try {
    http.send({Method::Get, "http://127.0.0.1:1/", "", {}});
    FAIL() << "expected an AdapterError";
  } catch (const AdapterError &error) {
    EXPECT_EQ(error.kind(), AdapterError::Kind::Stopped);
  }
}
