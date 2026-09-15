// Address parsing for the rana-socket client CLI. A target is either an
// absolute unix socket path or `tcp:host:port` (must be on the daemon's
// allow-list). Pure parsing concern: no socket I/O happens here.

#pragma once

#include <string>

struct Addr {
    std::string unix_path;  // "" when TCP
    std::string tcp_host;   // IPv4 literal; "" when unix
    int         tcp_port = -1;
};

// Fills `out` on success; on failure returns false and sets `err`.
bool parse_addr(const std::string& raw, Addr& out, std::string& err);
