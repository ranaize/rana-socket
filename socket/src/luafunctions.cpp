#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // execvpe
#endif

#include "luafunctions.h"

#include "ask_reply_generated.h"
#include "audio.hpp"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef PLUTO_ETL_ENABLE
#include <lstate.h>
#endif

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <cmath>
#include <cstdio>

#include <dirent.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "logger.h"
#include "protocol.h"

namespace rana {

// ───────────────────────────────────────────────────────────────────────
// Process spawning (moved here so rana.spawn can delegate to it)
// ───────────────────────────────────────────────────────────────────────

namespace {

std::vector<std::string> merged_env(const std::vector<std::string>& extras) {
    std::map<std::string, std::string> kv;
    for (char** e = environ; e && *e; ++e) {
        const std::string s(*e);
        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace(s.substr(0, eq), s.substr(eq + 1));
    }
    for (const auto& e : extras) {
        const auto eq = e.find('=');
        if (eq == std::string::npos) continue;
        kv[e.substr(0, eq)] = e.substr(eq + 1);
    }
    std::vector<std::string> out;
    out.reserve(kv.size());
    for (const auto& [k, v] : kv) out.push_back(k + "=" + v);
    return out;
}

}  // namespace

SpawnResult spawn_process(const std::string& workdir,
                         const std::vector<std::string>& argv,
                         const std::vector<std::string>& extra_env,
                         uint32_t timeout_ms,
                         bool capture_stdout) {
    SpawnResult res;
    if (argv.empty()) { res.spawn_failed = true; return res; }

    int err_pipe[2];
    if (pipe(err_pipe) != 0) { res.spawn_failed = true; return res; }

    int out_pipe[2] = {-1, -1};
    if (capture_stdout) {
        if (pipe(out_pipe) != 0) {
            ::close(err_pipe[0]); ::close(err_pipe[1]);
            res.spawn_failed = true; return res;
        }
    }

    const std::vector<std::string> env_store = merged_env(extra_env);

    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (const auto& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
    c_argv.push_back(nullptr);

    std::vector<char*> c_env;
    c_env.reserve(env_store.size() + 1);
    for (const auto& e : env_store) c_env.push_back(const_cast<char*>(e.c_str()));
    c_env.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        if (capture_stdout) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
        res.spawn_failed = true; return res;
    }
    if (pid == 0) {
        ::close(err_pipe[0]);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(err_pipe[1]);
        if (capture_stdout) {
            ::close(out_pipe[0]);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::close(out_pipe[1]);
        }
        if (!workdir.empty()) ::chdir(workdir.c_str());
        ::execvpe(argv[0].c_str(), c_argv.data(), c_env.data());
        ::_exit(127);
    }

    ::close(err_pipe[1]);
    ::fcntl(err_pipe[0], F_SETFL, ::fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);
    int out_fd = -1;
    if (capture_stdout) {
        ::close(out_pipe[1]);
        out_fd = out_pipe[0];
        ::fcntl(out_fd, F_SETFL, ::fcntl(out_fd, F_GETFL) | O_NONBLOCK);
    }

    auto drain = [&](int fd, std::string& dst) {
        if (fd < 0) return;
        char buf[4096];
        for (;;) {
            const ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r > 0) { dst.append(buf, static_cast<size_t>(r)); continue; }
            break;
        }
    };

    std::string err_out, out_out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    for (;;) {
        drain(err_pipe[0], err_out);
        if (capture_stdout) drain(out_fd, out_out);
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) { if (errno == EINTR) continue; res.spawn_failed = true; break; }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            res.timed_out = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drain(err_pipe[0], err_out);
    if (capture_stdout) drain(out_fd, out_out);
    ::close(err_pipe[0]);
    if (capture_stdout) ::close(out_fd);

    if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.exit_code = 128 + WTERMSIG(status);

    res.stderr_text = std::move(err_out);
    if (res.stderr_text.size() > 4096) res.stderr_text.resize(4096);
    if (capture_stdout) {
        res.stdout_text = std::move(out_out);
        if (res.stdout_text.size() > 16384) res.stdout_text.resize(16384);
    }
    return res;
}

// ───────────────────────────────────────────────────────────────────────
// base64 (RFC4648)
// ───────────────────────────────────────────────────────────────────────

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

// ───────────────────────────────────────────────────────────────────────
// Minimal JSON (encode/parse) operating on the Lua stack.
// ───────────────────────────────────────────────────────────────────────

namespace json {

static void encode(lua_State* L, int idx, std::string& out) {
    idx = lua_absindex(L, idx);
    const int t = lua_type(L, idx);
    switch (t) {
        case LUA_TNIL:
            out += "null";
            return;
        case LUA_TBOOLEAN:
            out += lua_toboolean(L, idx) ? "true" : "false";
            return;
        case LUA_TNUMBER: {
            double d = lua_tonumber(L, idx);
            double ip;
            if (std::modf(d, &ip) == 0.0 && std::fabs(d) < 1e15) {
                out += std::to_string(static_cast<long long>(ip));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", d);
                out += buf;
            }
            return;
        }
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            out += '"';
            for (size_t i = 0; i < len; ++i) {
                char c = s[i];
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out += c;
                        }
                }
            }
            out += '"';
            return;
        }
        case LUA_TTABLE: {
            // Array if it is a dense 1..n sequence.
            const int n = static_cast<int>(luaL_len(L, idx));
            bool is_array = (n > 0);
            if (is_array) {
                for (int i = 1; i <= n; ++i) {
                    lua_rawgeti(L, idx, i);
                    if (lua_isnil(L, -1)) { is_array = false; lua_pop(L, 1); break; }
                    lua_pop(L, 1);
                }
            }
            if (is_array) {
                out += '[';
                for (int i = 1; i <= n; ++i) {
                    if (i > 1) out += ',';
                    lua_rawgeti(L, idx, i);
                    encode(L, -1, out);
                    lua_pop(L, 1);
                }
                out += ']';
            } else {
                out += '{';
                bool first = true;
                lua_pushnil(L);
                while (lua_next(L, idx)) {
                    if (lua_type(L, -2) != LUA_TSTRING) { lua_pop(L, 1); continue; }
                    if (!first) out += ',';
                    first = false;
                    size_t klen = 0;
                    const char* k = lua_tolstring(L, -2, &klen);
                    out += '"';
                    out.append(k, klen);
                    out += "\":";
                    encode(L, -1, out);
                    lua_pop(L, 1);
                }
                out += '}';
            }
            return;
        }
        default:
            out += "null";
            return;
    }
}

struct Parser {
    const char* p;
    const char* end;
    lua_State* L;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    void err(const std::string& m) { throw std::runtime_error("json: " + m); }
    char peek() { if (p >= end) err("unexpected end"); return *p; }
    char take() { if (p >= end) err("unexpected end"); return *p++; }
    void expect(char c) { if (take() != c) err(std::string("expected '") + c + "'"); }

    void parse_value() {
        ws();
        if (p >= end) err("unexpected end");
        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return parse_string();
            case 't': case 'f': return parse_bool();
            case 'n': return parse_null();
            default: return parse_number();
        }
    }

    void parse_object() {
        expect('{');
        lua_newtable(L);
        ws();
        if (peek() == '}') { take(); return; }
        while (true) {
            ws();
            if (peek() != '"') err("object key must be string");
            parse_string();          // pushes key
            ws(); expect(':'); ws();
            parse_value();           // pushes value
            lua_rawset(L, -3);
            ws();
            char c = take();
            if (c == ',') continue;
            if (c == '}') return;
            err("expected ',' or '}'");
        }
    }

    void parse_array() {
        expect('[');
        lua_newtable(L);
        int i = 1;
        ws();
        if (peek() == ']') { take(); return; }
        while (true) {
            parse_value();
            lua_rawseti(L, -2, i++);
            ws();
            char c = take();
            if (c == ',') continue;
            if (c == ']') return;
            err("expected ',' or ']'");
        }
    }

    void parse_string() {
        expect('"');
        std::string s;
        while (p < end) {
            char c = take();
            if (c == '"') break;
            if (c == '\\') {
                char e = take();
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        if (p + 4 > end) err("bad unicode escape");
                        unsigned int cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = take();
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else err("bad hex in unicode escape");
                        }
                        // Encode as UTF-8 (BMP only).
                        if (cp < 0x80) s += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            s += static_cast<char>(0xC0 | (cp >> 6));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (cp >> 12));
                            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: err("bad escape");
                }
            } else {
                s += c;
            }
        }
        lua_pushlstring(L, s.data(), s.size());
    }

    void parse_bool() {
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; lua_pushboolean(L, 1); return; }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; lua_pushboolean(L, 0); return; }
        err("bad literal");
    }

    void parse_null() {
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; lua_pushnil(L); return; }
        err("bad literal");
    }

    void parse_number() {
        const char* start = p;
        if (peek() == '-') take();
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '+' || *p == '-')) take();
        std::string s(start, p);
        lua_pushnumber(L, std::strtod(s.c_str(), nullptr));
    }
};

static void decode(lua_State* L, const std::string& s) {
    Parser ps{s.data(), s.data() + s.size(), L};
    ps.parse_value();
    ps.ws();
    if (ps.p != ps.end) throw std::runtime_error("json: trailing characters");
}

}  // namespace json

// ───────────────────────────────────────────────────────────────────────
// Minimal blocking HTTP client (used by rana.http_post / post_multipart).
// Only http:// is supported; every request is checked against the endpoint
// allowlist before a socket is opened.
// ───────────────────────────────────────────────────────────────────────

namespace {

struct Url {
    std::string scheme, host, path;
    int port = 0;
};

bool parse_url(const std::string& raw, Url& u, std::string& err) {
    size_t sp = raw.find("://");
    if (sp == std::string::npos) { err = "missing scheme"; return false; }
    u.scheme = raw.substr(0, sp);
    std::string rest = raw.substr(sp + 3);
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        u.host = authority;
        u.port = (u.scheme == "https") ? 443 : 80;
    } else {
        u.host = authority.substr(0, colon);
        u.port = std::stoi(authority.substr(colon + 1));
    }
    if (u.scheme != "http" && u.scheme != "https") { err = "unsupported scheme"; return false; }
    return true;
}

// Returns (status_code, body). On transport failure status is 0.
std::pair<int, std::string> http_do(const std::string& method, const std::string& url,
                                    const std::map<std::string, std::string>& headers,
                                    const std::string& body,
                                    const std::vector<LuaRuntime::Endpoint>& allow) {
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

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return {0, "connect failed"};

    std::string req = method + " " + u.path + " HTTP/1.1\r\n";
    req += "Host: " + u.host + "\r\n";
    req += "Connection: close\r\n";
    if (headers.find("Content-Type") == headers.end())
        req += "Content-Type: application/json\r\n";
    for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t w = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (w <= 0) { ::close(fd); return {0, "send failed"}; }
        sent += static_cast<size_t>(w);
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        resp.append(buf, static_cast<size_t>(r));
    }
    ::close(fd);

    auto header_end = resp.find("\r\n\r\n");
    if (header_end == std::string::npos) return {0, "bad response"};
    std::string head = resp.substr(0, header_end);
    std::string body_out = resp.substr(header_end + 4);

    int status = 0;
    if (head.size() >= 12 && head.compare(0, 5, "HTTP/") == 0) {
        status = std::stoi(head.substr(9, 3));
    }

    // De-chunk if needed.
    bool chunked = head.find("Transfer-Encoding: chunked") != std::string::npos;
    if (chunked) {
        std::string decoded;
        size_t pos = 0;
        while (pos < body_out.size()) {
            size_t nl = body_out.find("\r\n", pos);
            if (nl == std::string::npos) break;
            size_t sz = std::stoul(body_out.substr(pos, nl - pos), nullptr, 16);
            if (sz == 0) break;
            decoded += body_out.substr(nl + 2, sz);
            pos = nl + 2 + sz + 2;
        }
        body_out = decoded;
    }
    return {status, body_out};
}

}  // namespace

// ───────────────────────────────────────────────────────────────────────
// FlatBuffers reply/response builders
// ───────────────────────────────────────────────────────────────────────

namespace {
std::string build_ask_reply_bytes(const std::string& action, const std::string& payload) {
    flatbuffers::FlatBufferBuilder b(128);
    const auto a = b.CreateString(action);
    const auto p = b.CreateString(payload);
    const auto off = CreateAskReply(b, a, p);
    FinishAskReplyBuffer(b, off);
    return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}

std::string build_response_bytes(uint64_t request_id, Status status, const std::string& message) {
    flatbuffers::FlatBufferBuilder b(128);
    const auto m = b.CreateString(message);
    const auto off = CreateResponse(b, request_id, status, m);
    b.Finish(off);
    return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}

Status status_from_string(const std::string& s) {
    if (s == "ok") return Status_OK;
    if (s == "unknown" || s == "unknown_command") return Status_UNKNOWN_COMMAND;
    if (s == "forbidden") return Status_FORBIDDEN;
    if (s == "invalid" || s == "invalid_payload") return Status_INVALID_PAYLOAD;
    if (s == "safety" || s == "safety_gate_failed") return Status_SAFETY_GATE_FAILED;
    if (s == "forward" || s == "forward_to_client") return Status_FORWARD_TO_CLIENT;
    return Status_EXECUTION_FAILED;  // "error" / default
}

std::string status_to_string(Status s) {
    switch (s) {
        case Status_OK: return "ok";
        case Status_UNKNOWN_COMMAND: return "unknown_command";
        case Status_FORBIDDEN: return "forbidden";
        case Status_INVALID_PAYLOAD: return "invalid_payload";
        case Status_SAFETY_GATE_FAILED: return "safety_gate_failed";
        case Status_FORWARD_TO_CLIENT: return "forward_to_client";
        case Status_EXECUTION_FAILED:
        default: return "execution_failed";
    }
}
}  // namespace

// ───────────────────────────────────────────────────────────────────────
// C bridge: the "rana" module
// ───────────────────────────────────────────────────────────────────────

namespace {

LuaRuntime* rt_from_up(lua_State* L) {
    return static_cast<LuaRuntime*>(lua_touserdata(L, lua_upvalueindex(1)));
}

std::map<std::string, std::string> table_to_map(lua_State* L, int idx) {
    std::map<std::string, std::string> m;
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) return m;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t len = 0;
            const char* k = lua_tolstring(L, -2, &len);
            std::string v;
            if (lua_type(L, -1) == LUA_TSTRING) {
                size_t vl = 0; const char* vs = lua_tolstring(L, -1, &vl); v.assign(vs, vl);
            } else if (lua_isnumber(L, -1)) {
                v = std::to_string(lua_tonumber(L, -1));
            } else if (lua_isboolean(L, -1)) {
                v = lua_toboolean(L, -1) ? "true" : "false";
            }
            m[std::string(k, len)] = v;
        }
        lua_pop(L, 1);
    }
    return m;
}

std::vector<std::string> table_to_strvec(lua_State* L, int idx) {
    std::vector<std::string> v;
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) return v;
    const int n = static_cast<int>(luaL_len(L, idx));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t len = 0; const char* s = lua_tolstring(L, -1, &len); v.emplace_back(s, len);
        }
        lua_pop(L, 1);
    }
    return v;
}

// Read M.meta.flags: an array of the integer constants exposed via rana.flags.
// Anything that is not a known ScriptFlag enum value is ignored with a warning
// (a script referencing a wrong constant simply gets no flags, not a privilege).
std::set<ScriptFlag> table_to_flags(lua_State* L, int idx) {
    std::set<ScriptFlag> s;
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) return s;
    const int n = static_cast<int>(luaL_len(L, idx));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        if (lua_isinteger(L, -1)) {
            const lua_Integer v = lua_tointeger(L, -1);
            switch (v) {
                case static_cast<lua_Integer>(ScriptFlag::LongIdle):
                    s.insert(ScriptFlag::LongIdle);
                    break;
                default:
                    rana::log_msg("luafunctions", "unknown script flag %lld ignored",
                                  static_cast<long long>(v));
                    break;
            }
        }
        lua_pop(L, 1);
    }
    return s;
}

// Build the argv for rana.spawn(name, ...) from the allowlist entry. The entry
// is a real boundary, not metadata:
//   * vararg=true            -> freeform: the script's string/number args are
//                               appended verbatim (espeak, xdg-open, lights, ...).
//   * vararg=false (default) -> a FIXED argv (path + the literal `args` list).
//                               Any caller args fail closed. This is how
//                               privileged verbs (shutdown/reboot/suspend via
//                               sudo, docker container * mc-server) stay
//                               hardcoded in init.lua and the binary-allowlist
//                               boundary can never become an arbitrary shell.
static bool build_spawn_argv(lua_State* L, const LuaRuntime::BinaryEntry& entry,
                             const std::string& name, std::vector<std::string>& argv,
                             std::string& err) {
    argv.assign(1, entry.path);
    if (!entry.vararg) {
        if (lua_gettop(L) > 1) {
            err = "binary '" + name + "' takes no arguments";
            return false;
        }
        for (const auto& a : entry.args) argv.push_back(a);
        return true;
    }
    const int n = lua_gettop(L);
    for (int i = 2; i <= n; ++i) {
        if (lua_type(L, i) == LUA_TSTRING) {
            size_t len = 0; const char* s = lua_tolstring(L, i, &len); argv.emplace_back(s, len);
        } else if (lua_isnumber(L, i)) {
            argv.push_back(std::to_string(lua_tonumber(L, i)));
        }
    }
    return true;
}

int rana_spawn(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = rt->binaries().find(name);
    if (it == rt->binaries().end()) {
        lua_pushnil(L);
        lua_pushstring(L, "binary not in allowlist");
        return 2;
    }
    std::vector<std::string> argv;
    std::string err;
    if (!build_spawn_argv(L, it->second, name, argv, err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    SpawnResult sr = spawn_process("", argv, {}, rt->current_timeout(), /*capture_stdout=*/true);
    lua_newtable(L);
    lua_pushinteger(L, sr.exit_code); lua_setfield(L, -2, "exit_code");
    lua_pushstring(L, sr.stdout_text.c_str()); lua_setfield(L, -2, "stdout");
    lua_pushstring(L, sr.stderr_text.c_str()); lua_setfield(L, -2, "stderr");
    lua_pushboolean(L, sr.timed_out); lua_setfield(L, -2, "timed_out");
    lua_pushboolean(L, sr.spawn_failed); lua_setfield(L, -2, "spawn_failed");
    return 1;
}

int rana_spawn_capture(lua_State* L) {
    // Same as rana_spawn but returns the captured stdout string directly.
    LuaRuntime* rt = rt_from_up(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = rt->binaries().find(name);
    if (it == rt->binaries().end()) {
        lua_pushnil(L); lua_pushstring(L, "binary not in allowlist"); return 2;
    }
    std::vector<std::string> argv;
    std::string err;
    if (!build_spawn_argv(L, it->second, name, argv, err)) {
        lua_pushnil(L); lua_pushstring(L, err.c_str()); return 2;
    }
    SpawnResult sr = spawn_process("", argv, {}, rt->current_timeout(), true);
    lua_pushstring(L, sr.stdout_text.c_str());
    return 1;
}

int rana_http_post(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* url = luaL_checkstring(L, 1);
    // arg2: payload — a Lua table (serialized to JSON) or an already-encoded string.
    std::string body_s;
    if (lua_istable(L, 2)) {
        json::encode(L, 2, body_s);
    } else {
        size_t blen = 0; const char* b = luaL_checklstring(L, 2, &blen); body_s.assign(b, blen);
    }
    std::map<std::string, std::string> headers = table_to_map(L, 3);
    auto [status, resp] = http_do("POST", url, headers, body_s, rt->endpoints());
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, resp.data(), resp.size());
    return 2;
}

int rana_post_multipart(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* url = luaL_checkstring(L, 1);
    const char* field = luaL_checkstring(L, 2);
    const char* filename = luaL_optstring(L, 3, "file");
    const char* ctype = luaL_optstring(L, 4, "application/octet-stream");
    size_t dlen = 0; const char* data = luaL_checklstring(L, 5, &dlen);
    std::map<std::string, std::string> headers = table_to_map(L, 6);

    const std::string boundary = "----ranaPlutoBoundary7Q4k9";
    std::string body =
        "--" + boundary + "\r\n" +
        "Content-Disposition: form-data; name=\"" + field + "\"; filename=\"" +
        filename + "\"\r\n" +
        "Content-Type: " + ctype + "\r\n\r\n";
    body.append(data, dlen);
    body += "\r\n--" + boundary + "--\r\n";

    headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;
    auto [status, resp] = http_do("POST", url, headers, body, rt->endpoints());
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, resp.data(), resp.size());
    return 2;
}

int rana_decode_json(lua_State* L) {
    size_t len = 0; const char* s = luaL_checklstring(L, 1, &len);
    try { json::decode(L, std::string(s, len)); }
    catch (const std::exception& e) { lua_pushnil(L); lua_pushstring(L, e.what()); return 2; }
    return 1;
}

int rana_encode_json(lua_State* L) {
    std::string out;
    json::encode(L, 1, out);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int rana_get_system_prompt(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const std::string p = rt->get_system_prompt();
    lua_pushlstring(L, p.data(), p.size());
    return 1;
}

int rana_build_ask_reply(lua_State* L) {
    const char* action = luaL_checkstring(L, 1);
    std::string payload;
    if (!lua_isnoneornil(L, 2)) payload = luaL_checkstring(L, 2);
    const std::string bytes = build_ask_reply_bytes(action, payload);
    lua_pushlstring(L, bytes.data(), bytes.size());
    return 1;
}

// Conflate a single Lua args table into (target, params): the conventional
// "target" key is pulled out binary-safe (talk hands the raw WAV over through
// it) and the rest become plain string params. Lua callers pass ONE table
// instead of separate positional target/params — target is just a param now,
// and internal is the same table's flag.
// The inner Command still carries a separate target slot for scripting
// convenience (cmd.target), but nothing outside Lua has to know it exists.
static void args_to_target_params(lua_State* L, int idx,
                                  std::string& target,
                                  std::map<std::string, std::string>& params) {
    if (!lua_istable(L, idx)) return;
    lua_getfield(L, idx, "target");
    if (!lua_isnil(L, -1)) {
        size_t n = 0; const char* t = lua_tolstring(L, -1, &n); target.assign(t, n);
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    params = table_to_map(L, idx);
    params.erase("target");
    params.erase("internal");
}

// arg: does a args table's field hold a truthy value? Used to read the
// conventional "internal" flag without touching a non-table arg.
static bool args_bool(lua_State* L, int idx, const char* field) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, field);
    const bool v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

int rana_dispatch(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* key = luaL_checkstring(L, 1);
    std::string action = lua_isnoneornil(L, 2) ? "" : luaL_checkstring(L, 2);

    // One args table (3rd arg): { target = "…", internal = bool, ...params }.
    // target is BINARY-safe — the stt hop's raw WAV rides through it — which is
    // why it is read with the length-aware lua_tolstring (a truncated WAV
    // header below this would have broken STT at the first NUL byte).
    std::string target;
    std::map<std::string, std::string> params;
    args_to_target_params(L, 3, target, params);

    // `internal` marks a PIPELINE hop (talk → stt/ask): the script is known and
    // had better run, regardless of this daemon's [commands.allowed]. The default
    // is EDGE semantics — only commands this daemon is allowed to run execute
    // here; anything else returns forbidden and the caller forwards it to the
    // peer. This is the old lookup()/forward_only split.
    const bool internal = args_bool(L, 3, "internal");

    if (internal) {
        if (!rt->has_script(key)) {
            lua_newtable(L);
            lua_pushstring(L, "forbidden"); lua_setfield(L, -2, "status");
            lua_pushstring(L, "dispatch target not found (no script)");
            lua_setfield(L, -2, "message");
            return 1;
        }
    } else if (!rt->is_allowed(key)) {
        lua_newtable(L);
        lua_pushstring(L, "forbidden"); lua_setfield(L, -2, "status");
        lua_pushstring(L, "dispatch target not in [commands.allowed]");
        lua_setfield(L, -2, "message");
        return 1;
    }

    const RanaResult r = rt->run_command(key, action, target, params, {},
                                         /*enforce_allowed=*/!internal);
    lua_newtable(L);
    lua_pushstring(L, status_to_string(r.status).c_str()); lua_setfield(L, -2, "status");
    lua_pushstring(L, r.message.c_str()); lua_setfield(L, -2, "message");
    return 1;
}

// rana.has_script(key) → boolean: whether scripts/<key>.pluto exists on this
// daemon. Lets ask.pluto distinguish "forwardable to the peer" from unknown.
int rana_has_script(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* key = luaL_checkstring(L, 1);
    lua_pushboolean(L, rt->has_script(key));
    return 1;
}

// rana.respond(key, action, args?) -- hand a fresh inner Command to the PEER
// (client) daemon instead of running it here. args is one table (target rides
// inside it, see args_to_target_params). The counterpart to rana.dispatch
// (which executes locally). The outer run_command re-wraps the stashed command
// as FORWARD_TO_CLIENT; the agent relays it to its local daemon.
// Used by talk/ask to speak chat replies client-side and to relay intents the
// server does not own.
int rana_respond(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* key = luaL_checkstring(L, 1);
    std::string action = lua_isnoneornil(L, 2) ? "" : luaL_checkstring(L, 2);
    // One args table (3rd arg), binary-safe target included.
    std::string target;
    std::map<std::string, std::string> params;
    args_to_target_params(L, 3, target, params);
    rt->request_respond(key, action, target, params);
    return 0;
}

int rana_fetch_command(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_newtable(L);
    lua_pushstring(L, rt->current_key().c_str()); lua_setfield(L, -2, "key");
    lua_pushstring(L, rt->current_action().c_str()); lua_setfield(L, -2, "action");
    lua_pushstring(L, rt->current_target().c_str()); lua_setfield(L, -2, "target");
    lua_newtable(L);
    for (const auto& [k, v] : rt->current_params()) {
        lua_pushstring(L, v.c_str()); lua_setfield(L, -2, k.c_str());
    }
    lua_setfield(L, -2, "params");
    const std::string& d = rt->current_data_str();
    lua_pushlstring(L, d.data(), d.size()); lua_setfield(L, -2, "data");
    return 1;
}

int rana_build_response(lua_State* L) {
    uint64_t request_id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
    const char* status = luaL_checkstring(L, 2);
    std::string message = lua_isnoneornil(L, 3) ? "" : luaL_checkstring(L, 3);
    const std::string bytes = build_response_bytes(request_id, status_from_string(status), message);
    lua_pushlstring(L, bytes.data(), bytes.size());
    return 1;
}

int rana_get_config_path(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_pushstring(L, rt->config()->path.c_str());
    return 1;
}

int rana_get_socket_path(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_pushstring(L, rt->config()->daemon.socket_path.c_str());
    return 1;
}

int rana_get_llm_url(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_pushstring(L, rt->config()->llm.url.c_str());
    return 1;
}

int rana_get_llm_model(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_pushstring(L, rt->config()->llm.model.c_str());
    return 1;
}

int rana_get_stt_url(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_pushstring(L, rt->config()->stt.url.c_str());
    return 1;
}

int rana_get_stt_model(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_pushstring(L, rt->config()->stt.model.c_str());
    return 1;
}

int rana_get_workdir(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_pushstring(L, rt->config()->workdir.c_str());
    return 1;
}

int rana_allow_binaries(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    std::map<std::string, LuaRuntime::BinaryEntry> m;
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            LuaRuntime::BinaryEntry e;
            const std::string name = lua_tostring(L, -2);
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "path"); if (lua_type(L, -1) == LUA_TSTRING) e.path = lua_tostring(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "vararg"); e.vararg = lua_toboolean(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "args"); e.args = table_to_strvec(L, -1); lua_pop(L, 1);
            }
            m[name] = std::move(e);
        }
        lua_pop(L, 1);
    }
    rt->allow_binaries(m);
    return 0;
}

int rana_allow_endpoints(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    std::vector<std::tuple<std::string, std::string, int>> v;
    const int n = static_cast<int>(luaL_len(L, 1));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            std::string scheme, host; int port = 0;
            lua_getfield(L, -1, "scheme"); if (lua_type(L, -1) == LUA_TSTRING) scheme = lua_tostring(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "host"); if (lua_type(L, -1) == LUA_TSTRING) host = lua_tostring(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "port"); port = static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 1);
            v.emplace_back(scheme, host, port);
        }
        lua_pop(L, 1);
    }
    rt->allow_endpoints(v);
    return 0;
}

int rana_set_constants(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    rt->set_constants(table_to_map(L, 1));
    return 0;
}

int rana_set_fs_whitelist(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    rt->set_fs_whitelist(table_to_strvec(L, 1));
    return 0;
}

int rana_register_script_meta(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* key = luaL_checkstring(L, 1);
    std::string name, description, examples_json;
    std::vector<std::string> actions, keywords;
    std::map<std::string, std::string> params;
    std::set<ScriptFlag> flags;
    bool danger = false;
    uint32_t timeout_ms = 5000;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "name"); if (lua_type(L, -1) == LUA_TSTRING) name = lua_tostring(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "description"); if (lua_type(L, -1) == LUA_TSTRING) description = lua_tostring(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "timeout_ms"); timeout_ms = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 2, "actions"); actions = table_to_strvec(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "keywords"); keywords = table_to_strvec(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "params"); params = table_to_map(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "danger"); danger = lua_toboolean(L, -1); lua_pop(L, 1);
        // examples: serialize back to JSON via our encoder for the prompt.
        lua_getfield(L, 2, "examples");
        std::string ej; json::encode(L, -1, ej); examples_json = ej; lua_pop(L, 1);
        lua_getfield(L, 2, "flags"); flags = table_to_flags(L, -1); lua_pop(L, 1);
    }
    rt->register_script_meta(key, name, description, actions, keywords, params, timeout_ms, examples_json, danger, flags);
    return 0;
}

// rana.script_metas() → { [key] = { description, keywords = {...}, danger = bool } }
// Exposes the startup-scanned metas to the ask hop's fuzzy pre-router so a
// transcript can be routed to a script WITHOUT the LLM round-trip.
int rana_script_metas(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_newtable(L);
    for (const auto& [key, m] : rt->metas()) {
        lua_newtable(L);
        lua_pushstring(L, m.description.c_str()); lua_setfield(L, -2, "description");
        lua_createtable(L, static_cast<int>(m.keywords.size()), 0);
        int i = 1;
        for (const auto& kw : m.keywords) { lua_pushstring(L, kw.c_str()); lua_rawseti(L, -2, i++); }
        lua_setfield(L, -2, "keywords");
        lua_pushboolean(L, m.danger); lua_setfield(L, -2, "danger");
        lua_setfield(L, -2, key.c_str());
    }
    return 1;
}

int rana_log(lua_State* L) {
    const char* level = luaL_optstring(L, 1, "info");
    const char* msg = luaL_checkstring(L, 2);
    rana::log_msg(level, "%s", msg);
    return 0;
}

int rana_base64_encode(lua_State* L) {
    size_t len = 0; const char* s = luaL_checklstring(L, 1, &len);
    lua_pushstring(L, base64_encode(std::vector<uint8_t>(s, s + len)).c_str());
    return 1;
}

int rana_base64_decode(lua_State* L) {
    size_t len = 0; const char* s = luaL_checklstring(L, 1, &len);
    lua_pushlstring(L, base64_decode(std::string(s, len)).data(), base64_decode(std::string(s, len)).size());
    return 1;
}

// Convert raw on-the-wire audio into WAV bytes for the STT hop. `encoding` is
// "pcm16" or "wav"; sample_rate/channels apply to pcm16. Exposes audio.cpp's
// decode_to_wav to Pluto so the talk hop (scripts/talk.pluto) can be a rewriteable
// script instead of hardcoded C++. Returns nil for an unsupported encoding.
int rana_audio_decode_to_wav(lua_State* L) {
    size_t raw_len = 0;
    const char* raw = luaL_checklstring(L, 1, &raw_len);
    const std::string encoding = lua_isnoneornil(L, 2) ? "pcm16" : luaL_checkstring(L, 2);
    const uint32_t sample_rate = static_cast<uint32_t>(luaL_optinteger(L, 3, 16000));
    const uint8_t channels = static_cast<uint8_t>(luaL_optinteger(L, 4, 1));
    std::string wav;
    if (!rana::audio::decode_to_wav(std::string(raw, raw_len), encoding, sample_rate, channels, wav)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, wav.data(), wav.size());
    return 1;
}

const luaL_Reg kRanaLib[] = {
    {"spawn", rana_spawn},
    {"http_post", rana_http_post},
    {"http_post_multipart", rana_post_multipart},
    {"json_encode", rana_encode_json},
    {"json_decode", rana_decode_json},
    {"get_system_prompt", rana_get_system_prompt},
    {"build_ask_reply", rana_build_ask_reply},
    {"build_response", rana_build_response},
    {"dispatch", rana_dispatch},
    {"has_script", rana_has_script},
    {"script_metas", rana_script_metas},
    {"respond", rana_respond},
    {"fetch_command", rana_fetch_command},
    {"get_config_path", rana_get_config_path},
    {"get_socket_path", rana_get_socket_path},
    {"get_llm_url", rana_get_llm_url},
    {"get_llm_model", rana_get_llm_model},
    {"get_stt_url", rana_get_stt_url},
    {"get_stt_model", rana_get_stt_model},
    {"get_workdir", rana_get_workdir},
    {"allow_binaries", rana_allow_binaries},
    {"allow_endpoints", rana_allow_endpoints},
    {"register_script_meta", rana_register_script_meta},
    {"set_constants", rana_set_constants},
    {"set_fs_whitelist", rana_set_fs_whitelist},
    {"log", rana_log},
    {"base64_encode", rana_base64_encode},
    {"base64_decode", rana_base64_decode},
    {nullptr, nullptr},
};

// rana.audio submodule: the codec surface (decode_to_wav). Kept out of the main
// namespace so it reads as a distinct capability. Same lightuserdata upvalue
// convention as kRanaLib (rt_from_up reads upvalue 1).
const luaL_Reg kRanaAudioLib[] = {
    {"decode_to_wav", rana_audio_decode_to_wav},
    {nullptr, nullptr},
};

void remove_global(lua_State* L, const char* name) {
    lua_pushnil(L);
    lua_setglobal(L, name);
}

}  // namespace

void luaopen_rana(lua_State* L, LuaRuntime* rt) {
    luaL_newlibtable(L, kRanaLib);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, kRanaLib, 1);

    // rana.audio submodule: the codec surface (decode_to_wav). Kept out of the
    // main namespace so it reads as a distinct capability.
    lua_newtable(L);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, kRanaAudioLib, 1);
    lua_setfield(L, -2, "audio");

    // rana.flags: capability flag constants. Scripts opt in via
    // M.meta.flags = { rana.flags.LONG_IDLE } — the integer is the contract,
    // so a typo surfaces as an empty allow-flags set, not a silent privilege.
    lua_newtable(L);
    lua_pushinteger(L, static_cast<int>(ScriptFlag::LongIdle));
    lua_setfield(L, -2, "LONG_IDLE");
    lua_setfield(L, -2, "flags");

    // Make require("rana") resolve to this table.
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "rana");
    lua_pop(L, 1);
    lua_setglobal(L, "rana");
}

// ───────────────────────────────────────────────────────────────────────
// LuaRuntime
// ───────────────────────────────────────────────────────────────────────

lua_State* LuaRuntime::new_state() {
    lua_State* L = luaL_newstate();

#ifdef PLUTO_ETL_ENABLE
    // Per-script execution-time budget. Pluto's ETL caps every state at a fixed
    // wall-clock deadline (compile-time PLUTO_ETL_NANOS, default 1ms). Instead
    // of baking in a constant, pin this state's deadline to the current script
    // timeout: run_entrypoint() sets current_timeout_ to the script's own
    // budget (meta.timeout_ms, plus the FLAG_LONG_IDLE grant; 5s when the script
    // has no meta) before calling new_state(), so each script gets its own
    // budget. Blocking C hops (stt/ask HTTP) count wall time too -- the VM
    // re-checks the deadline on the next opcode -- so a script's total
    // runtime is bounded by its own timeout.
    L->l_G->deadline =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()
        + static_cast<std::time_t>(current_timeout_) * 1000000LL;
#endif

    // Hardening (always on, no config toggle): only open the libraries our
    // scripts are allowed to use. The Pluto default luaL_openlibs() opens
    // EVERYTHING and -- critically -- preloads ffi/socket/http/crypto/wasm into
    // package.preload, so `require("ffi")` would bypass our global stripping.
    // We instead pass an explicit load bitmask AND a ZERO preload mask, so the
    // dangerous libraries are never installed as globals AND can never be
    // require()'d. LUA_DBLIBK must stay (Pluto runs embedded startup code that
    // calls debug.getinfo); we strip the debug global right after.
    static constexpr int kRanaLoadLibs =
        LUA_GLIBK | LUA_LOADLIBK | LUA_COLIBK | LUA_DBLIBK |
        LUA_MATHLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK |
        PLUTO_JSONLIBK | PLUTO_BASE64LIBK | PLUTO_REGEXLIBK;
    luaL_openselectedlibs(L, kRanaLoadLibs, 0);

    // Sandbox: strip any filesystem / OS / package-loading / debug surface, and
    // any library that could reach the network or native code. Defense in depth:
    // these are not opened above, but we strip them explicitly.
    for (const char* g : {"io", "os", "debug", "load", "loadfile", "dofile",
                          "ffi", "socket", "http", "tls", "crypto", "wasm"}) {
        remove_global(L, g);
    }

    // Make `require` fail closed: no disk/cpath, no dynamic loader, empty
    // preload + searcher tables so nothing (incl. ffi/socket/http) resolves.
    if (lua_getglobal(L, "package") == LUA_TTABLE) {
        lua_pushstring(L, ""); lua_setfield(L, -2, "path");
        lua_pushstring(L, ""); lua_setfield(L, -2, "cpath");
        lua_pushnil(L); lua_setfield(L, -2, "loadlib");
        lua_pushstring(L, "preload");  lua_newtable(L); lua_settable(L, -3);
        lua_pushstring(L, "searchers"); lua_newtable(L); lua_settable(L, -3);
        lua_pushstring(L, "loaders");  lua_newtable(L); lua_settable(L, -3);
        lua_pop(L, 1);
    }

    // Purge the same from package.loaded so `require("os")` etc. cannot
    // resurrect a library after its global was removed.
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    for (const char* m : {"os", "io", "debug", "loadlib",
                          "ffi", "socket", "http", "crypto", "wasm"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, m);
    }
    lua_pop(L, 1);

    luaopen_rana(L, this);
    return L;
}

LuaRuntime::~LuaRuntime() = default;

bool LuaRuntime::is_allowed(const std::string& key) const {
    return allowed_.count(normalize_key(key)) > 0;
}

void LuaRuntime::allow_binaries(const std::map<std::string, BinaryEntry>& m) {
    for (const auto& [k, v] : m) binaries_.emplace(k, v);
}
void LuaRuntime::allow_endpoints(
    const std::vector<std::tuple<std::string, std::string, int>>& v) {
    for (const auto& [s, h, p] : v) endpoints_.push_back({s, h, p});
}
void LuaRuntime::set_fs_whitelist(const std::vector<std::string>& v) { fs_whitelist_ = v; }
void LuaRuntime::set_constants(const std::map<std::string, std::string>& m) { constants_ = m; }

void LuaRuntime::register_script_meta(const std::string& key, const std::string& name,
                                     const std::string& description,
                                     const std::vector<std::string>& actions,
                                     const std::vector<std::string>& keywords,
                                     const std::map<std::string, std::string>& params,
                                     uint32_t timeout_ms,
                                     const std::string& examples_json,
                                     bool danger,
                                     const std::set<ScriptFlag>& flags) {
    ScriptMeta m;
    m.name = name.empty() ? key : name;
    m.description = description;
    m.actions = actions;
    m.keywords = keywords;
    m.params = params;
    m.timeout_ms = timeout_ms;
    m.examples_json = examples_json;
    m.danger = danger;
    m.flags = flags;
    metas_[key] = std::move(m);
}

std::string LuaRuntime::get_system_prompt() const {
    std::ostringstream os;
    os << "You are a strict voice command routing engine. Output ONLY raw JSON: "
          "{\"action\":\"KEY\",\"payload\":\"TEXT\"}. No markdown blocks, no filler.\n\n";
    os << "# NOTE: this action vocabulary is wired to this rana-socketd daemon. "
          "ALWAYS emit one of the script KEYS below in the \"action\" field.\n\n";
    os << "\"action\" rules:\n";
    for (const auto& [key, m] : metas_) {
        os << "- \"" << key << "\": " << (m.description.empty() ? "(no description)" : m.description);
        if (!m.actions.empty()) {
            os << " (actions: " << m.actions[0];
            for (size_t i = 1; i < m.actions.size(); ++i) os << ", " << m.actions[i];
            os << ")";
        }
        os << "\n";
        if (!m.examples_json.empty()) os << "  examples: " << m.examples_json << "\n";
    }
    os << "If the request fits none of the above, use \"reply\" with a short spoken answer.\n";
    return os.str();
}

std::vector<std::string> LuaRuntime::available_scripts() const {
    std::vector<std::string> keys;
    DIR* d = opendir(scripts_dir_.c_str());
    if (!d) return keys;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string fn(e->d_name);
        if (fn.size() > 6 && fn.compare(fn.size() - 6, 6, ".pluto") == 0) {
            keys.push_back(fn.substr(0, fn.size() - 6));
        }
    }
    closedir(d);
    return keys;
}

bool LuaRuntime::init(const Config& cfg, std::string& err) {
    cfg_ = &cfg;
    scripts_dir_ = rana::resolve_scripts_dir(cfg);
    allowed_ = cfg.allowed;

    // 1) Run init.lua (the system interface registry).
    {
        const std::string init_path = scripts_dir_ + "/init.lua";
        lua_State* L = new_state();
        if (luaL_dofile(L, init_path.c_str()) != LUA_OK) {
            std::string e = lua_tostring(L, -1);
            lua_close(L);
            err = "init.lua failed: " + e;
            return false;
        }
        lua_close(L);
    }

    // 2) Scan scripts/ for *.pluto, read each meta table, cross-check allowlist.
    std::set<std::string> have;
    for (const auto& key : available_scripts()) {
        have.insert(key);
        const std::string path = scripts_dir_ + "/" + key + ".pluto";
        lua_State* L = new_state();
        if (luaL_dofile(L, path.c_str()) != LUA_OK) {
            std::string e = lua_tostring(L, -1);
            lua_close(L);
            err = "script " + key + ".pluto failed to load: " + e;
            return false;
        }
        // Module table is on the stack (top). Look for `meta`.
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "meta");
            if (lua_istable(L, -1)) {
                std::string name, description, examples_json;
                std::vector<std::string> actions, keywords;
                std::map<std::string, std::string> params;
                std::set<ScriptFlag> flags;
                bool danger = false;
                uint32_t timeout_ms = 5000;
                lua_getfield(L, -1, "name"); if (lua_type(L, -1) == LUA_TSTRING) name = lua_tostring(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "description"); if (lua_type(L, -1) == LUA_TSTRING) description = lua_tostring(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "timeout_ms"); timeout_ms = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
                lua_getfield(L, -1, "actions"); actions = table_to_strvec(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "keywords"); keywords = table_to_strvec(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "params"); params = table_to_map(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "danger"); danger = lua_toboolean(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "examples"); std::string ej; json::encode(L, -1, ej); examples_json = ej; lua_pop(L, 1);
                lua_getfield(L, -1, "flags"); flags = table_to_flags(L, -1); lua_pop(L, 1);
                register_script_meta(key, name, description, actions, keywords, params, timeout_ms, examples_json, danger, flags);
            }
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    // 3) Every allowed key should have a script file. A missing script is
    //    non-fatal: the command is dropped from the allowlist and a warning is
    //    logged so the daemon still starts (the command simply won't be usable).
    for (auto it = allowed_.begin(); it != allowed_.end();) {
        if (have.count(*it) == 0) {
            rana::log_msg("luafunctions",
                          "command '%s' is enabled in [commands.allowed] but "
                          "scripts/%s.pluto was not found — skipping (not loaded)",
                          it->c_str(), it->c_str());
            it = allowed_.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

// Extra wall-clock granted to scripts that declare FLAG_LONG_IDLE in their meta
// (on top of meta.timeout_ms) — for scripts that legitimately idle/sleep for a
// long time while waiting on slow upstreams.
constexpr uint32_t kLongIdleGrantMs = 60000;

RanaResult LuaRuntime::run_entrypoint(const std::string& key,
                                      const std::map<std::string, std::string>* params,
                                      const std::vector<uint8_t>* data,
                                      const std::string& action,
                                      const std::string& target) {
    RanaResult out;
    const std::string path = scripts_dir_ + "/" + key + ".pluto";

    current_key_ = key;
    current_action_ = action;
    current_target_ = target;
    if (params) current_params_ = *params; else current_params_.clear();
    if (data) current_data_ = *data; else current_data_.clear();
    auto mit = metas_.find(key);
    current_timeout_ = (mit != metas_.end()) ? mit->second.timeout_ms : 5000;
    if (mit != metas_.end() && mit->second.flags.count(ScriptFlag::LongIdle) > 0) {
        current_timeout_ += kLongIdleGrantMs;
    }

    lua_State* L = new_state();

    if (luaL_dofile(L, path.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_close(L);
        out = {Status_EXECUTION_FAILED, "script load error: " + e};
        return out;
    }

    // Module table on top. Extract the execute entry.
    if (!lua_istable(L, -1)) {
        lua_close(L);
        out = {Status_EXECUTION_FAILED, "script did not return a module table"};
        return out;
    }
    lua_getfield(L, -1, "execute");
    if (!lua_isfunction(L, -1)) {
        lua_close(L);
        out = {Status_EXECUTION_FAILED, "script has no execute function"};
        return out;
    }

    // The cmd table, mirroring the wire Command. target is pushed size-aware
    // (lua_pushlstring, not pushstring): a hop relays BINARY through target
    // (talk → stt hands the WAV), and a C-string push would truncate it at the
    // first NUL byte.
    lua_newtable(L);
    lua_pushstring(L, key.c_str()); lua_setfield(L, -2, "key");
    // action: nil (not "") when absent, so scripts can `cmd.action or "default"`
    // without tripping over Lua's truthy empty string.
    if (!action.empty()) {
        lua_pushstring(L, action.c_str()); lua_setfield(L, -2, "action");
    } else {
        lua_pushnil(L); lua_setfield(L, -2, "action");
    }
    lua_pushlstring(L, target.data(), target.size()); lua_setfield(L, -2, "target");
    lua_newtable(L);
    for (const auto& [k, v] : current_params_) {
        lua_pushstring(L, v.c_str()); lua_setfield(L, -2, k.c_str());
    }
    lua_setfield(L, -2, "params");
    if (data && !data->empty()) {
        lua_pushlstring(L, reinterpret_cast<const char*>(data->data()), data->size());
        lua_setfield(L, -2, "data");
    }

    // ETL/pcall wall-clock is covered by the per-script deadline set in
    // new_state() -- run_entrypoint picks the deadline before calling it -- so
    // no alarm here; the binary spawn path enforces its own timeout.
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_close(L);
        out = {Status_EXECUTION_FAILED, "script error: " + e};
        return out;
    }

    // Script return-value convention:
    //   nil / true  → OK, no message
    //   string      → OK with that message (STT transcript, result text, ...)
    //   number N    → OK if N == 0, else EXECUTION_FAILED "exit code N"
    //   false       → EXECUTION_FAILED
    //   table       → {status, message} passed through verbatim (the shape
    //                 rana.dispatch returns), or {ok = bool, message = ...} —
    //                 the failure-message form, so a failed hop leaves the LLM
    //                 something to reason about.
    if (lua_isnil(L, -1)) {
        out = {Status_OK, ""};
    } else if (lua_isboolean(L, -1)) {
        out = lua_toboolean(L, -1) ? RanaResult{Status_OK, ""}
                                   : RanaResult{Status_EXECUTION_FAILED, "script failed"};
    } else if (lua_isnumber(L, -1)) {
        const lua_Integer code = lua_tointeger(L, -1);
        out = code == 0 ? RanaResult{Status_OK, ""}
                        : RanaResult{Status_EXECUTION_FAILED, "exit code " + std::to_string(code)};
    } else if (lua_isstring(L, -1)) {
        size_t len = 0; const char* s = lua_tolstring(L, -1, &len);
        out = {Status_OK, std::string(s, len)};
    } else if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "status");
        const bool has_status = !lua_isnil(L, -1);
        std::string status_s = has_status ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        lua_getfield(L, -1, "message");
        std::string msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (has_status) {
            out = {status_from_string(status_s), msg};
        } else {
            lua_getfield(L, -1, "ok");
            const bool ok = lua_toboolean(L, -1);
            lua_pop(L, 1);
            out = ok ? RanaResult{Status_OK, msg} : RanaResult{Status_EXECUTION_FAILED, msg};
        }
    } else {
        out = {Status_EXECUTION_FAILED, "script returned an unexpected value"};
    }

    lua_close(L);
    return out;
}

RanaResult LuaRuntime::run_command(const std::string& key, const std::string& action,
                                   const std::string& target,
                                   const std::map<std::string, std::string>& params,
                                   const std::vector<uint8_t>& data,
                                   bool enforce_allowed) {
    // The [commands.allowed] gate is for the EDGE only: Executor always passes
    // enforce_allowed=true, so a non-allowlisted command is rejected here. A
    // script-to-script hop (rana.dispatch from talk/ask back into stt/ask)
    // passes enforce_allowed=false for scripts that exist but are not edge
    // addressable.
    if (enforce_allowed && !is_allowed(key)) {
        return RanaResult{Status_FORBIDDEN, "command '" + key + "' not in [commands.allowed]"};
    }
    ++command_depth_;

    const RanaResult r = run_entrypoint(key, &params, &data, action, target);

    // rana.respond() stashed a command for the peer daemon: only the OUTERMOST
    // run_command folds it into the result — base64 of the inner Command in
    // `message`, status FORWARD_TO_CLIENT — the only carry form the wire needs.
    // Inner rana.dispatch hops must not swallow the stash before the script that
    // set it has returned, so only command_depth_ == 1 consumes it.
    std::optional<PendingRespond> respond;
    if (command_depth_ == 1 && pending_respond_) {
        respond = std::move(*pending_respond_);
        pending_respond_.reset();
    }
    --command_depth_;

    if (respond) {
        flatbuffers::FlatBufferBuilder b(256);
        std::vector<flatbuffers::Offset<StrPair>> ps;
        for (const auto& [k, v] : respond->params) {
            ps.push_back(CreateStrPair(b, b.CreateString(k), b.CreateString(v)));
        }
        const auto cmd = CreateCommandDirect(b, 0,
            respond->key.c_str(), respond->action.c_str(), respond->target.c_str(),
            ps.empty() ? nullptr : &ps, nullptr);
        b.Finish(cmd);
        std::vector<uint8_t> inner(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
        return RanaResult{Status_FORWARD_TO_CLIENT, base64_encode(inner)};
    }

    return r;
}

bool LuaRuntime::has_script(const std::string& key) const {
    return metas_.count(key) > 0;
}

void LuaRuntime::request_respond(const std::string& key, const std::string& action,
                                 const std::string& target,
                                 const std::map<std::string, std::string>& params) {
    pending_respond_ = PendingRespond{key, action, target, params};
}

// Accessors used by the C bridge functions.
const std::map<std::string, LuaRuntime::BinaryEntry>& LuaRuntime::binaries() const { return binaries_; }
const std::vector<LuaRuntime::Endpoint>& LuaRuntime::endpoints() const { return endpoints_; }
uint32_t LuaRuntime::current_timeout() const { return current_timeout_; }
const std::string& LuaRuntime::current_key() const { return current_key_; }
const std::string& LuaRuntime::current_action() const { return current_action_; }
const std::string& LuaRuntime::current_target() const { return current_target_; }
const std::map<std::string, std::string>& LuaRuntime::current_params() const { return current_params_; }
std::string LuaRuntime::current_data_str() const {
    return std::string(reinterpret_cast<const char*>(current_data_.data()), current_data_.size());
}

}  // namespace rana
