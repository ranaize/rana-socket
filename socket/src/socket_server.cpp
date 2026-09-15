#include "socket_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace rana {
namespace {

bool cidr_match(const std::string& cidr, const std::string& ip) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    const std::string net = cidr.substr(0, slash);
    const int bits = std::atoi(cidr.c_str() + slash + 1);

    struct in_addr net_addr{};
    struct in_addr ip_addr{};
    if (inet_pton(AF_INET, net.c_str(), &net_addr) != 1) return false;
    if (inet_pton(AF_INET, ip.c_str(), &ip_addr) != 1) return false;
    if (bits < 0 || bits > 32) return false;

    const uint32_t mask = bits == 0 ? 0u : ~uint32_t{0} << (32 - bits);
    const uint32_t n = ntohl(net_addr.s_addr);
    const uint32_t i = ntohl(ip_addr.s_addr);
    return (n & mask) == (i & mask);
}

}  // namespace

bool ip_allowed(const std::vector<std::string>& allow_list, const std::string& ip) {
    for (const auto& entry : allow_list) {
        if (entry == ip) return true;
        if (entry.find('/') != std::string::npos && cidr_match(entry, ip)) return true;
    }
    return false;
}

SocketServer::~SocketServer() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        if (cfg_.socket_type == "unix") ::unlink(cfg_.socket_path.c_str());
    }
}

bool SocketServer::start(std::string& err) {
    if (cfg_.socket_type == "unix") {
        if (cfg_.socket_path.empty()) {
            err = "unix socket_type requires socket_path";
            return false;
        }
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            err = std::strerror(errno);
            return false;
        }
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (cfg_.socket_path.size() >= sizeof(addr.sun_path)) {
            err = "socket_path too long";
            return false;
        }
        std::strncpy(addr.sun_path, cfg_.socket_path.c_str(), sizeof(addr.sun_path) - 1);
        ::unlink(cfg_.socket_path.c_str());
        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            err = "bind: " + std::string(std::strerror(errno));
            return false;
        }
        ::chmod(cfg_.socket_path.c_str(), cfg_.socket_permissions);
    } else if (cfg_.socket_type == "tcp") {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            err = std::strerror(errno);
            return false;
        }
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg_.port);
        if (inet_pton(AF_INET, cfg_.bind_address.c_str(), &addr.sin_addr) != 1) {
            err = "invalid bind_address";
            return false;
        }
        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            err = "bind: " + std::string(std::strerror(errno));
            return false;
        }
    } else {
        err = "daemon.socket_type must be \"unix\" or \"tcp\"";
        return false;
    }

    if (::listen(listen_fd_, 8) < 0) {
        err = "listen: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

int SocketServer::accept_client(std::string& peer) {
    struct sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
    const int cfd =
        ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&ss), &slen);
    if (cfd < 0) return -1;

    if (cfg_.socket_type == "tcp") {
        char ip[INET_ADDRSTRLEN] = {0};
        const auto* a = reinterpret_cast<const struct sockaddr_in*>(&ss);
        if (::inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip)) == nullptr) {
            ::close(cfd);
            return -1;
        }
        peer = ip;
        if (!ip_allowed(cfg_.allowed_ips, peer)) {
            ::close(cfd);  // rejected: not on the allow-list
            return -1;
        }
    } else {
        peer = "unix";
    }
    return cfd;
}

}  // namespace rana
