#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rana {

// A parsed outbound URL. `parse_url` is a plain function (no exceptions): it
// reports failure through `err` so a lua_CFunction caller never has to catch.
// A https URL is accepted only at parse time when the build has RANA_ENABLE_TLS.
struct Url {
    std::string scheme, host, path;
    int port = 0;
};
bool parse_url(const std::string& raw, Url& u, std::string& err);

// An outbound endpoint the sandbox has allowlisted. It lives here rather than
// inside LuaRuntime so the transport translation unit has no dependency on the
// runtime — http_do() takes a plain vector of these.
struct Endpoint {
    std::string scheme;
    std::string host;
    int port = 0;
};

// Minimal blocking HTTP client (single-threaded daemon). Only http:// is
// supported unless the build has RANA_ENABLE_TLS, in which case https:// is
// spoken with mandatory certificate + hostname verification. Every request is
// checked against `allow` before a socket is opened. Returns (status_code,
// body); status is 0 on a transport or gate failure, otherwise the upstream's
// own status code. Never throws across the caller's boundary.
std::pair<int, std::string> http_do(const std::string& method, const std::string& url,
                                    const std::map<std::string, std::string>& headers,
                                    const std::string& body,
                                    const std::vector<Endpoint>& allow,
                                    uint32_t timeout_ms);

}  // namespace rana
