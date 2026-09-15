#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace rana {

class SocketServer {
public:
    explicit SocketServer(const DaemonConfig& cfg) : cfg_(cfg) {}
    ~SocketServer();

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;

    bool start(std::string& err);

    // Blocks until a client connects. Returns -1 if the peer is rejected
    // (TCP allow-list) or accept fails; peer receives the remote address.
    int accept_client(std::string& peer);

private:
    DaemonConfig cfg_;
    int listen_fd_ = -1;
};

bool ip_allowed(const std::vector<std::string>& allow_list, const std::string& ip);

}  // namespace rana
