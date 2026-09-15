// Wire transport implementation. See transport.hpp.

#include "transport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <flatbuffers/flatbuffers.h>
#include <command_generated.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

int open_socket(const Addr& addr, std::string& err) {
    const int fd = ::socket(addr.tcp_host.empty() ? AF_UNIX : AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = ::strerror(errno);
        return -1;
    }

    if (!addr.tcp_host.empty()) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = ::htons(static_cast<uint16_t>(addr.tcp_port));
        if (::inet_pton(AF_INET, addr.tcp_host.c_str(), &sa.sin_addr) != 1) {
            err = "invalid IPv4 address '" + addr.tcp_host + "'";
            ::close(fd);
            return -1;
        }
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) < 0) {
            err = ::strerror(errno);
            ::close(fd);
            return -1;
        }
        return fd;
    }

    if (addr.unix_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        err = "unix socket path too long";
        ::close(fd);
        return -1;
    }
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, addr.unix_path.c_str(), sizeof(sa.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) < 0) {
        err = ::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_exact(int fd, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

bool recv_exact(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        const ssize_t r = ::recv(fd, p, n, 0);
        if (r == 0) return false;  // peer closed
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool send_frame(int fd, const void* data, uint32_t n) {
    const uint8_t hdr[4] = {
        uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n),
    };
    return send_exact(fd, hdr, sizeof(hdr)) && (n == 0 || send_exact(fd, data, n));
}

bool recv_frame(int fd, std::string& out) {
    uint8_t hdr[4];
    if (!recv_exact(fd, hdr, sizeof(hdr))) return false;
    const uint32_t n = (uint32_t{hdr[0]} << 24) | (uint32_t{hdr[1]} << 16) |
                       (uint32_t{hdr[2]} << 8) | uint32_t{hdr[3]};
    if (n == 0 || n > kMaxFrameSize) return false;
    out.assign(n, '\0');
    return recv_exact(fd, out.data(), n);
}

int run_client(const std::string& addr_raw, const uint8_t* cmd_data, size_t cmd_len) {
    if (cmd_len == 0 || cmd_len > kMaxFrameSize) {
        std::fprintf(stderr, "rana-voice-agent: bad command size %zu\n", cmd_len);
        return 5;
    }

    Addr addr;
    std::string err;
    if (!parse_addr(addr_raw, addr, err)) {
        std::fprintf(stderr, "rana-voice-agent: %s\n", err.c_str());
        return 2;
    }
    const int fd = open_socket(addr, err);
    if (fd < 0) {
        std::fprintf(stderr, "rana-voice-agent: connect: %s\n", err.c_str());
        return 3;
    }

    if (!send_frame(fd, cmd_data, static_cast<uint32_t>(cmd_len))) {
        std::fprintf(stderr, "rana-voice-agent: send: %s\n", ::strerror(errno));
        ::close(fd);
        return 4;
    }

    std::string raw;
    if (!recv_frame(fd, raw)) {
        std::fprintf(stderr, "rana-voice-agent: no response (daemon closed)\n");
        ::close(fd);
        return 5;
    }
    ::close(fd);

    const auto* resp = ::flatbuffers::GetRoot<rana::Response>(raw.data());
    const uint64_t request_id = resp->request_id();
    const rana::Status status = resp->status();
    const char* message = resp->message() ? resp->message()->c_str() : "";
    const char* status_name = rana::EnumNameStatus(status);

    std::printf("request_id=%llu status=%s message=%s\n",
                static_cast<unsigned long long>(request_id), status_name, message);
    return status == rana::Status_OK ? 0 : 1;
}
