#include "json.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rana {

namespace json {

void encode(lua_State* L, int idx, std::string& out) {
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

void decode(lua_State* L, const std::string& s) {
    Parser ps{s.data(), s.data() + s.size(), L};
    ps.parse_value();
    ps.ws();
    if (ps.p != ps.end) throw std::runtime_error("json: trailing characters");
}

}  // namespace json

}  // namespace rana
