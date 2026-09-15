// Address parsing implementation. See addr.hpp.

#include "addr.hpp"

#include <cerrno>
#include <cstdlib>
#include <string>

bool parse_addr(const std::string& raw, Addr& out, std::string& err) {
    if (raw.rfind("tcp:", 0) == 0) {
        const std::string rest = raw.substr(4);
        const auto colon = rest.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= rest.size()) {
            err = "tcp:host:port expected";
            return false;
        }
        out.tcp_host = rest.substr(0, colon);
        const std::string port_s = rest.substr(colon + 1);
        char* end = nullptr;
        const long p = std::strtol(port_s.c_str(), &end, 10);
        if (end == port_s.c_str() || *end != '\0' || p < 1 || p > 65535) {
            err = "invalid tcp port '" + port_s + "'";
            return false;
        }
        out.tcp_port = static_cast<int>(p);
        return true;
    }
    if (!raw.empty() && raw[0] != '/') {
        err = "unix socket path must be absolute, or use tcp:host:port";
        return false;
    }
    out.unix_path = raw;
    return true;
}
