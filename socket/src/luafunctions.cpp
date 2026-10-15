#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // execvpe
#endif

#include "luafunctions.h"

#include "audio.hpp"

// The HTTP transport, JSON, base64 and process-spawn concerns live in their own
// translation units; the bridge below only calls their public entry points.
#include "http.h"
#include "json.h"

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
#include <sys/time.h>
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
#include <exception>
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
// Bridge status helpers
// ───────────────────────────────────────────────────────────────────────

namespace {
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
                // Integers must not serialize as "16000.000000".
                const double d = lua_tonumber(L, -1);
                double ip = 0;
                if (std::modf(d, &ip) == 0.0 && std::fabs(d) < 1e15) {
                    v = std::to_string(static_cast<long long>(ip));
                } else {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.17g", d);
                    v = buf;
                }
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
                case static_cast<lua_Integer>(ScriptFlag::ExternalEgress):
                    s.insert(ScriptFlag::ExternalEgress);
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
//                               hardcoded in init.pluto and the binary-allowlist
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
        } else {
            // Fail loudly rather than silently dropping an argument: a table or
            // boolean here is a script bug (e.g. forgetting table.unpack).
            err = "binary '" + name + "' argument " + std::to_string(i - 1) +
                  " must be a string or number";
            return false;
        }
    }
    return true;
}

// rana.spawn(name, ...) → a normalized result table. It ALWAYS returns a table
// (never nil), so a caller can index it safely:
//   { ok = bool, code = int, out = stdout, err = stderr, reason = string|nil }
// `reason` is one of "not_allowed" | "bad_args" | "spawn_failed" | "timeout" |
// "exit" (nil on success). `err` carries a human string in every failure case.
int rana_spawn(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* name = luaL_checkstring(L, 1);

    auto push_result = [&](bool ok, int code, const std::string& out,
                           const std::string& err, const char* reason) -> int {
        lua_newtable(L);
        lua_pushboolean(L, ok);   lua_setfield(L, -2, "ok");
        lua_pushinteger(L, code); lua_setfield(L, -2, "code");
        lua_pushlstring(L, out.data(), out.size()); lua_setfield(L, -2, "out");
        lua_pushlstring(L, err.data(), err.size()); lua_setfield(L, -2, "err");
        if (reason) { lua_pushstring(L, reason); lua_setfield(L, -2, "reason"); }
        return 1;
    };

    auto it = rt->binaries().find(name);
    if (it == rt->binaries().end()) {
        return push_result(false, -1, "", "binary not in allowlist", "not_allowed");
    }
    std::vector<std::string> argv;
    std::string err;
    if (!build_spawn_argv(L, it->second, name, argv, err)) {
        return push_result(false, -1, "", err, "bad_args");
    }
    SpawnResult sr = spawn_process("", argv, {}, rt->current_timeout(), /*capture_stdout=*/true);
    if (sr.spawn_failed) {
        return push_result(false, -1, sr.stdout_text,
                           sr.stderr_text.empty() ? "spawn failed" : sr.stderr_text, "spawn_failed");
    }
    if (sr.timed_out) {
        return push_result(false, -1, sr.stdout_text, "command timed out", "timeout");
    }
    if (sr.exit_code != 0) {
        return push_result(false, sr.exit_code, sr.stdout_text, sr.stderr_text, "exit");
    }
    return push_result(true, 0, sr.stdout_text, sr.stderr_text, nullptr);
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
    auto [status, resp] = http_do("POST", url, headers, body_s, rt->endpoints(),
                                  rt->current_etl_timeout());
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, resp.data(), resp.size());
    // Third value: the HTTP status (0 == transport failure). Callers that only
    // care about success ignore it; the provider chain uses it to distinguish
    // "unreachable" (0, advance to the next provider) from a live 4xx/5xx (the
    // upstream's own answer, which stops the chain).
    lua_pushinteger(L, status);
    return 3;
}

// rana.providers(kind) -> { {type, schema, model, name}, ... }
//
// The ordered provider chain for `kind` ("llm"/"stt"). Only non-secret fields are
// exposed: the URL and any bearer token stay in the C bridge. An empty table is
// returned for an unknown kind or a hop with no configured providers.
int rana_providers(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* kind = luaL_checkstring(L, 1);
    const auto* provs = rt->providers(kind);
    lua_newtable(L);
    if (!provs) return 1;
    int i = 0;
    for (const auto& p : *provs) {
        ++i;
        lua_newtable(L);
        lua_pushstring(L, p.type.c_str());   lua_setfield(L, -2, "type");
        lua_pushstring(L, p.schema.c_str()); lua_setfield(L, -2, "schema");
        lua_pushstring(L, p.model.c_str());  lua_setfield(L, -2, "model");
        lua_pushstring(L, p.name.c_str());   lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, i);
    }
    return 1;
}

// rana.provider_post(kind, index, payload [, headers]) -> ok, body, status
//
// POST to the index-th (1-based) provider of the `kind` chain — the ONLY bridge
// entry point that speaks TLS. The destination and bearer token come from CONFIG,
// never from Lua. A "huggingface" provider is gated three ways: built with
// RANA_ENABLE_TLS, [security].external_egress true, and the URL is https (routing
// an external answer back into a script is a separate, inbound gate — see
// rana_script_metas' external_egress). A "local" provider may only dial a host
// already in the scripts/init.pluto endpoint allowlist. The third value is the HTTP
// status (0 == transport failure or a gate refusal), matching rana.http_post.
int rana_provider_post(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const std::string kind = luaL_checkstring(L, 1);
    const int index = static_cast<int>(luaL_checkinteger(L, 2));

    auto fail = [&](const std::string& reason) -> int {
        lua_pushboolean(L, false);
        lua_pushlstring(L, reason.data(), reason.size());
        lua_pushinteger(L, 0);
        return 3;
    };

    const auto* provs = rt->providers(kind);
    if (!provs || index < 1 || static_cast<size_t>(index) > provs->size()) {
        return fail("bad_args");
    }
    const std::string reason = rt->provider_unavailable_reason(kind, index);
    if (!reason.empty()) return fail(reason);
    const ProviderConfig& p = (*provs)[index - 1];

    std::string body_s;
    if (lua_istable(L, 3)) {
        json::encode(L, 3, body_s);
    } else {
        size_t blen = 0; const char* b = luaL_checklstring(L, 3, &blen); body_s.assign(b, blen);
    }
    std::map<std::string, std::string> headers = table_to_map(L, 4);

    std::vector<Endpoint> allow;
    if (p.type == "huggingface") {
        // The bridge owns Authorization on a cloud call: drop any script-supplied
        // one so a script cannot smuggle a different credential.
        headers.erase("Authorization");
        headers["Authorization"] = "Bearer " + p.api_key;
        // Restrict the dial to this exact configured endpoint.
        Url u; std::string e;
        if (!parse_url(p.url, u, e)) return fail(e.empty() ? "bad url" : e);
        allow.push_back({u.scheme, u.host, u.port});
    } else {
        // A local provider may only reach a host the sandbox already allowlisted.
        allow = rt->endpoints();
    }

    auto [status, resp] = http_do("POST", p.url, headers, body_s, allow, p.timeout_ms);
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, resp.data(), resp.size());
    lua_pushinteger(L, status);
    return 3;
}

int rana_decode_json(lua_State* L) {
    size_t len = 0; const char* s = luaL_checklstring(L, 1, &len);
    try { json::decode(L, std::string(s, len)); }
    catch (const std::exception& e) { lua_pushnil(L); lua_pushstring(L, e.what()); return 2; }
    return 1;
}

int rana_get_system_prompt(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const std::string p = rt->get_system_prompt();
    lua_pushlstring(L, p.data(), p.size());
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
        lua_pushboolean(L, m.flags.count(ScriptFlag::ExternalEgress) > 0);
        lua_setfield(L, -2, "external_egress");
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

// Each dot-separated segment of a module name must be [A-Za-z0-9_]+ so a name can
// never contain path separators or traversal.
bool valid_module_name(const std::string& s) {
    if (s.empty()) return false;
    size_t start = 0;
    for (;;) {
        const size_t dot = s.find('.', start);
        const size_t end = (dot == std::string::npos) ? s.size() : dot;
        if (end == start) return false;  // empty segment ("a..b", leading/trailing dot)
        for (size_t i = start; i < end; ++i) {
            const char c = s[i];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return true;
}

// The ONLY loader installed in a script state: it resolves require("lib.<name>")
// to a reviewed file under <scripts_dir>/lib/<name>.pluto and refuses everything
// else (both other namespaces and any traversal). This lets shared Lua helpers be
// factored out idiomatically without reopening package.path/searchers.
int rana_lib_searcher(lua_State* L) {
    LuaRuntime* rt = static_cast<LuaRuntime*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* name = luaL_checkstring(L, 1);
    const std::string mod(name);
    if (mod.compare(0, 4, "lib.") != 0 || !valid_module_name(mod)) {
        lua_pushfstring(L, "module '%s' is not available", name);
        return 1;  // string => "not found", require tries the next searcher
    }
    std::string rel = mod;
    for (char& c : rel) if (c == '.') c = '/';
    const std::string path = rt->scripts_dir() + "/" + rel + ".pluto";
    if (::access(path.c_str(), R_OK) != 0) {
        lua_pushfstring(L, "module '%s' not found", name);
        return 1;
    }
    if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
        return 1;  // leave the loader's error message as the searcher result
    }
    lua_pushstring(L, path.c_str());
    return 2;  // loader + path
}

// Wrap a bridge function so a C++ exception can never unwind across the Lua C
// boundary (which would std::terminate the daemon). On any exception, surface a
// (nil, message) pair; Lua callers are expected to handle it.
template <int (*F)(lua_State*)>
int rana_guard(lua_State* L) {
    try {
        return F(L);
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    } catch (...) {
        lua_pushnil(L);
        lua_pushstring(L, "internal error");
        return 2;
    }
}

const luaL_Reg kRanaLib[] = {
    {"spawn", rana_guard<rana_spawn>},
    {"http_post", rana_guard<rana_http_post>},
    {"providers", rana_guard<rana_providers>},
    {"provider_post", rana_guard<rana_provider_post>},
    {"json_decode", rana_guard<rana_decode_json>},
    {"get_system_prompt", rana_guard<rana_get_system_prompt>},
    {"dispatch", rana_guard<rana_dispatch>},
    {"has_script", rana_guard<rana_has_script>},
    {"script_metas", rana_guard<rana_script_metas>},
    {"respond", rana_guard<rana_respond>},
    {"allow_binaries", rana_guard<rana_allow_binaries>},
    {"allow_endpoints", rana_guard<rana_allow_endpoints>},
    {"log", rana_guard<rana_log>},
    {nullptr, nullptr},
};

// rana.audio submodule: the codec surface (decode_to_wav). Kept out of the main
// namespace so it reads as a distinct capability. Same lightuserdata upvalue
// convention as kRanaLib (rt_from_up reads upvalue 1).
const luaL_Reg kRanaAudioLib[] = {
    {"decode_to_wav", rana_guard<rana_audio_decode_to_wav>},
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
    // EXTERNAL_EGRESS is an INBOUND opt-in (may an external answer reach me);
    // it is not needed to dial a huggingface provider.
    lua_newtable(L);
    lua_pushinteger(L, static_cast<int>(ScriptFlag::LongIdle));
    lua_setfield(L, -2, "LONG_IDLE");
    lua_pushinteger(L, static_cast<int>(ScriptFlag::ExternalEgress));
    lua_setfield(L, -2, "EXTERNAL_EGRESS");
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
        + static_cast<std::time_t>(current_etl_timeout_) * 1000000LL;
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
    // preload table, and a searcher list containing ONLY our restricted lib/
    // loader — so nothing (incl. ffi/socket/http) resolves.
    if (lua_getglobal(L, "package") == LUA_TTABLE) {
        lua_pushstring(L, ""); lua_setfield(L, -2, "path");
        lua_pushstring(L, ""); lua_setfield(L, -2, "cpath");
        lua_pushnil(L); lua_setfield(L, -2, "loadlib");
        lua_pushstring(L, "preload");  lua_newtable(L); lua_settable(L, -3);
        // searchers: exactly ONE loader — the restricted lib/ searcher. Pluto's
        // built-in Lua/C/preload searchers are all absent, so require() of
        // ffi/socket/os/... cannot resolve, while the reviewed scripts/lib/*
        // helpers stay reachable as require("lib.<name>").
        lua_pushstring(L, "searchers");
        lua_newtable(L);
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, rana_lib_searcher, 1);
        lua_rawseti(L, -2, 1);
        lua_settable(L, -3);
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
        // init.pluto is the system-interface registry (run once in init()), not a
        // command module: it returns no meta and must never be dispatched.
        if (fn == "init.pluto") continue;
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

    // 1) Run init.pluto (the system interface registry).
    {
        const std::string init_path = scripts_dir_ + "/init.pluto";
        lua_State* L = new_state();
        if (luaL_dofile(L, init_path.c_str()) != LUA_OK) {
            std::string e = lua_tostring(L, -1);
            lua_close(L);
            err = "init.pluto failed: " + e;
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

    if (params) current_params_ = *params; else current_params_.clear();
    auto mit = metas_.find(key);
    current_timeout_ = (mit != metas_.end()) ? mit->second.timeout_ms : 5000;
    current_etl_timeout_ = current_timeout_;
    if (mit != metas_.end() && mit->second.flags.count(ScriptFlag::LongIdle) > 0) {
        current_etl_timeout_ += kLongIdleGrantMs;
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
const std::vector<Endpoint>& LuaRuntime::endpoints() const { return endpoints_; }
uint32_t LuaRuntime::current_timeout() const { return current_timeout_; }
uint32_t LuaRuntime::current_etl_timeout() const { return current_etl_timeout_; }

// ── Provider chains ([llm.provider]/[stt.provider]) ──────────────────

bool LuaRuntime::tls_built() {
#ifdef RANA_ENABLE_TLS
    return true;
#else
    return false;
#endif
}

const std::vector<ProviderConfig>* LuaRuntime::providers(const std::string& kind) const {
    if (!cfg_) return nullptr;
    if (kind == "llm") return &cfg_->llm.providers;
    if (kind == "stt") return &cfg_->stt.providers;
    return nullptr;
}

std::string LuaRuntime::provider_unavailable_reason(const std::string& kind, int index) const {
    const auto* provs = providers(kind);
    if (!provs || index < 1 || static_cast<size_t>(index) > provs->size()) return "bad_args";
    const ProviderConfig& p = (*provs)[index - 1];
    if (p.type == "huggingface") {
        if (!tls_built()) return "tls_not_built";
        if (cfg_ && !cfg_->security.external_egress) return "disabled";
    }
    // A local provider is additionally checked against endpoints_ inside http_do.
    return "";
}

}  // namespace rana
