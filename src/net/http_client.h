// SPDX-License-Identifier: GPL-3.0-or-later
//
// A blocking HTTP client on libcurl (https, redirects, encodings), from
// jtplay, for the logos a server gives as URLs (see IconCache). One
// instance per thread; requests block, so the callers are workers.
#ifndef NET_HTTP_CLIENT_H
#define NET_HTTP_CLIENT_H

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>

namespace net {

class HttpClient {
public:
  struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool has_body = false;
    bool follow_redirects = true;

    // Gives up when no data has moved for this long. This is not a cap on
    // the whole transfer: a large listing or image on a slow link may take
    // as long as it needs, as long as bytes keep arriving.
    long stall_timeout_ms = 30000;
    // Optional hard limit on the whole request, 0 = none.
    long deadline_ms = 0;

    // The body is held outside any managed heap, so every caller states how
    // much it is willing to hold. Exceeding it aborts the transfer, as an
    // error unless 'truncate' is set: then the first max_bytes are the body
    // and Response::truncated says so.
    size_t max_bytes = 8u << 20;
    bool truncate = false;

    // A login, for a server that asks for one (basic or digest, whichever it
    // offers). "" = none.
    std::string user;
    std::string password;
  };

  struct Response {
    long status = 0;
    std::string url;                            // after redirects
    std::map<std::string, std::string> headers; // keys lower-cased
    std::string body;
    bool truncated = false;                     // cut at Request::max_bytes
  };

  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Returns false only on transport errors (DNS, TLS, stall, ...);
  // HTTP error statuses are a successful request with res.status set.
  bool Perform(const Request& req, Response& res, std::string& error);

private:
  CURL* curl_ = nullptr;
};

} // namespace net

#endif
