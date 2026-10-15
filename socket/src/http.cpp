#include "http.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef RANA_ENABLE_TLS
// Provider-chain transport (TLS gated by the --tls premake flag). This is the
// ONLY place TLS is spoken; generic rana.http_post remains http-only and can
// never reach a huggingface provider (see rana_provider_post).
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace rana {

namespace {

// strtol-based integer parse that reports failure instead of throwing (these
// run inside a lua_CFunction; a stray exception must never cross that boundary).
bool parse_int(const std::string& s, long& out, int base = 10) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, base);
    if (errno != 0 || end == s.c_str()) return false;
    // Tolerate trailing whitespace / chunk extensions (";ext").
    while (*end == ' ' || *end == '\t' || *end == ';') {
        ++end;
        while (*end && *end != ';') ++end;
    }
    if (*end != '\0') return false;
    out = v;
    return true;
}

int default_port_for(const std::string& scheme) { return scheme == "https" ? 443 : 80; }

}  // namespace

bool parse_url(const std::string& raw, Url& u, std::string& err) {
    size_t sp = raw.find("://");
    if (sp == std::string::npos) { err = "missing scheme"; return false; }
    u.scheme = raw.substr(0, sp);
    if (u.scheme == "https") {
#ifndef RANA_ENABLE_TLS
        // Deny-by-default: a build without RANA_ENABLE_TLS cannot speak TLS, so
        // reject https rather than silently connecting in the clear.
        err = "https requires a TLS-enabled build (rebuild with --tls)";
        return false;
#endif
    } else if (u.scheme != "http") {
        err = "unsupported scheme (http only)";
        return false;
    }
    std::string rest = raw.substr(sp + 3);
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        u.host = authority;
        u.port = default_port_for(u.scheme);
    } else {
        u.host = authority.substr(0, colon);
        long p = 0;
        if (!parse_int(authority.substr(colon + 1), p) || p <= 0 || p > 65535) {
            err = "invalid port";
            return false;
        }
        u.port = static_cast<int>(p);
    }
    if (u.host.empty()) { err = "missing host"; return false; }
    return true;
}

namespace {

// Set a receive/send deadline so a stalled upstream can never hang the
// single-threaded daemon inside recv(). timeout_ms == 0 disables the deadline.
void set_socket_timeout(int fd, uint32_t timeout_ms) {
    if (timeout_ms == 0) return;
    struct timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// ── Transport: plaintext or TLS ──────────────────────────────────────
// One HTTP connection. Plaintext uses send/recv; with RANA_ENABLE_TLS a
// https:// URL wraps the fd in OpenSSL with MANDATORY certificate + hostname
// verification. Every failure is reported by return value — never an exception
// (this runs inside a lua_CFunction). Only rana.provider_post() ever builds a TLS
// connection; generic rana.http_post cannot reach a huggingface provider.
struct Conn {
    int fd = -1;
    bool tls = false;
#ifdef RANA_ENABLE_TLS
    SSL* ssl = nullptr;
#endif
};

#ifdef RANA_ENABLE_TLS
// Process-wide client context (the daemon is single-threaded). Created lazily on
// the first https request. Returns nullptr if OpenSSL could not be set up.
SSL_CTX* tls_ctx() {
    static SSL_CTX* ctx = []() -> SSL_CTX* {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (!c) return nullptr;
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        if (SSL_CTX_set_default_verify_paths(c) != 1) {
            SSL_CTX_free(c);
            return nullptr;
        }
        return c;
    }();
    return ctx;
}
#endif

// Wrap an already-connected fd in TLS when scheme == "https". Returns false (and
// leaves the caller to close the fd) on any handshake/verification failure.
bool conn_open(Conn& c, const std::string& scheme, const std::string& host) {
    c.tls = false;
    if (scheme != "https") return true;
#ifdef RANA_ENABLE_TLS
    SSL_CTX* ctx = tls_ctx();
    if (!ctx) return false;
    c.ssl = SSL_new(ctx);
    if (!c.ssl) return false;
    SSL_set_fd(c.ssl, c.fd);
    // SNI + hostname verification: without these, a valid cert for ANY host
    // would be accepted (a silent MITM).
    SSL_set_tlsext_host_name(c.ssl, host.c_str());
    SSL_set1_host(c.ssl, host.c_str());
    if (SSL_connect(c.ssl) != 1) return false;
    if (SSL_get_verify_result(c.ssl) != X509_V_OK) return false;
    c.tls = true;
    return true;
#else
    (void)host;
    return false;
#endif
}

ssize_t conn_send(Conn& c, const void* buf, size_t n) {
#ifdef RANA_ENABLE_TLS
    if (c.tls) return SSL_write(c.ssl, buf, static_cast<int>(n));
#endif
    return ::send(c.fd, buf, n, 0);
}

ssize_t conn_recv(Conn& c, void* buf, size_t n) {
#ifdef RANA_ENABLE_TLS
    if (c.tls) return SSL_read(c.ssl, buf, static_cast<int>(n));
#endif
    return ::recv(c.fd, buf, n, 0);
}

void conn_close(Conn& c) {
#ifdef RANA_ENABLE_TLS
    // Free the SSL object whenever it was allocated, even if the handshake
    // failed before c.tls was set (otherwise SSL_new's object leaks).
    if (c.ssl) { SSL_shutdown(c.ssl); SSL_free(c.ssl); c.ssl = nullptr; }
#endif
    if (c.fd >= 0) { ::close(c.fd); c.fd = -1; }
}

}  // namespace

// Returns (status_code, body). On transport failure status is 0.
std::pair<int, std::string> http_do(const std::string& method, const std::string& url,
                                    const std::map<std::string, std::string>& headers,
                                    const std::string& body,
                                    const std::vector<Endpoint>& allow,
                                    uint32_t timeout_ms) {
    std::string err;
    Url u;
    if (!parse_url(url, u, err)) return {0, std::string("url error: ") + err};

    bool ok = false;
    for (const auto& e : allow) {
        if (e.scheme == u.scheme && e.host == u.host && (e.port == u.port || e.port == 0)) {
            ok = true;
            break;
        }
    }
    if (!ok) return {0, "destination not in endpoint allowlist"};

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res)
        return {0, "dns failure"};

    Conn conn;
    bool tls_failed = false;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_socket_timeout(fd, timeout_ms);
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) { ::close(fd); continue; }
        conn.fd = fd;
        if (conn_open(conn, u.scheme, u.host)) break;
        tls_failed = true;
        conn_close(conn);
    }
    freeaddrinfo(res);
    if (conn.fd < 0) return {0, tls_failed ? "tls handshake failed" : "connect failed"};

    std::string req = method + " " + u.path + " HTTP/1.1\r\n";
    // Host carries the port when it is not the scheme default (80 for http,
    // 443 for https).
    req += "Host: " + u.host +
           (u.port == default_port_for(u.scheme) ? "" : ":" + std::to_string(u.port)) + "\r\n";
    req += "Connection: close\r\n";
    if (headers.find("Content-Type") == headers.end())
        req += "Content-Type: application/json\r\n";
    for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t w = conn_send(conn, req.data() + sent, req.size() - sent);
        if (w <= 0) { conn_close(conn); return {0, "send failed"}; }
        sent += static_cast<size_t>(w);
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        ssize_t r = conn_recv(conn, buf, sizeof(buf));
        if (r <= 0) break;
        resp.append(buf, static_cast<size_t>(r));
    }
    conn_close(conn);

    auto header_end = resp.find("\r\n\r\n");
    if (header_end == std::string::npos) return {0, "bad response"};
    std::string head = resp.substr(0, header_end);
    std::string body_out = resp.substr(header_end + 4);

    int status = 0;
    if (head.size() >= 12 && head.compare(0, 5, "HTTP/") == 0) {
        long code = 0;
        if (parse_int(head.substr(9, 3), code)) status = static_cast<int>(code);
    }

    // De-chunk if needed.
    bool chunked = head.find("Transfer-Encoding: chunked") != std::string::npos;
    if (chunked) {
        std::string decoded;
        size_t pos = 0;
        while (pos < body_out.size()) {
            size_t nl = body_out.find("\r\n", pos);
            if (nl == std::string::npos) break;
            long sz = 0;
            if (!parse_int(body_out.substr(pos, nl - pos), sz, 16) || sz < 0) break;
            if (sz == 0) break;
            decoded += body_out.substr(nl + 2, static_cast<size_t>(sz));
            pos = nl + 2 + static_cast<size_t>(sz) + 2;
        }
        body_out = decoded;
    }
    return {status, body_out};
}

}  // namespace rana
