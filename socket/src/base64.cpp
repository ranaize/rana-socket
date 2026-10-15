#include "luafunctions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rana {

namespace {
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

std::string base64_encode(const std::vector<uint8_t>& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | uint32_t(in[i + 2]);
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(kBase64[(n >> 6) & 63]);
        out.push_back(kBase64[n & 63]);
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t n = uint32_t(in[i]) << 16;
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(kBase64[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    size_t i = 0;
    while (i + 3 < in.size() + 1) {
        int n = 0, cnt = 0;
        for (int j = 0; j < 4 && i < in.size(); ++j, ++i) {
            if (in[i] == '=') { n <<= 6; continue; }
            const int v = val(in[i]);
            if (v < 0) { n <<= 6; continue; }
            n = (n << 6) | v; cnt++;
        }
        if (cnt == 0) break;
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        if (cnt >= 3) out.push_back(static_cast<char>((n >> 8) & 0xFF));
        if (cnt >= 4) out.push_back(static_cast<char>(n & 0xFF));
    }
    return out;
}

}  // namespace rana
