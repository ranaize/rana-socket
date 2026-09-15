#include "protocol.h"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace rana {
namespace {

constexpr uint32_t kMaxFrameSize = 16u * 1024 * 1024;

}  // namespace

bool read_exact(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = recv(fd, p + got, n - got, 0);
        if (r == 0) return false;  // peer closed
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

bool read_frame(int fd, std::string& out) {
    uint8_t hdr[4];
    if (!read_exact(fd, hdr, sizeof(hdr))) return false;
    const uint32_t len = (uint32_t{hdr[0]} << 24) | (uint32_t{hdr[1]} << 16) |
                         (uint32_t{hdr[2]} << 8) | uint32_t{hdr[3]};
    if (len == 0 || len > kMaxFrameSize) return false;
    out.assign(len, '\0');
    return read_exact(fd, out.data(), len);
}

bool write_frame(int fd, const void* data, uint32_t len) {
    const uint8_t hdr[4] = {
        uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len),
    };

    auto send_all = [&](const void* buf, size_t n) {
        const auto* p = static_cast<const uint8_t*>(buf);
        size_t sent = 0;
        while (sent < n) {
            const ssize_t w = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            sent += static_cast<size_t>(w);
        }
        return true;
    };

    return send_all(hdr, sizeof(hdr)) && (len == 0 || send_all(data, len));
}

}  // namespace rana
