#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <map>
#include <string>
#include <vector>

namespace rana {

namespace {

std::map<std::string, std::string> table_to_map(lua_State* L, int idx);

}  // namespace

int rana_http_post(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* url = luaL_checkstring(L, 1);
    std::string body_s;
    if (lua_istable(L, 2)) {
        json::encode(L, 2, body_s);
    } else {
        size_t blen = 0;
        const char* b = luaL_checklstring(L, 2, &blen);
        body_s.assign(b, blen);
    }
    std::map<std::string, std::string> headers = table_to_map(L, 3);
    auto [status, resp] = http_do("POST", url, headers, body_s, rt->endpoints(),
                                  rt->current_etl_timeout());
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, resp.data(), resp.size());
    lua_pushinteger(L, status);
    return 3;
}

int rana_providers(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const std::string kind = luaL_checkstring(L, 1);
    const auto* provs = rt->providers(kind);
    lua_newtable(L);
    if (provs) {
        for (size_t i = 0; i < provs->size(); ++i) {
            const auto& p = (*provs)[i];
            lua_newtable(L);
            lua_pushstring(L, p.type.c_str());
            lua_setfield(L, -2, "type");
            lua_pushstring(L, p.schema.c_str());
            lua_setfield(L, -2, "schema");
            lua_pushstring(L, p.model.c_str());
            lua_setfield(L, -2, "model");
            lua_pushstring(L, p.name.c_str());
            lua_setfield(L, -2, "name");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    }
    return 1;
}

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
        size_t blen = 0;
        const char* b = luaL_checklstring(L, 3, &blen);
        body_s.assign(b, blen);
    }
    std::map<std::string, std::string> headers = table_to_map(L, 4);

    std::vector<Endpoint> allow;
    if (p.type == "huggingface") {
        headers.erase("Authorization");
        headers["Authorization"] = "Bearer " + p.api_key;
        Url u;
        std::string e;
        if (!parse_url(p.url, u, e)) return fail(e.empty() ? "bad url" : e);
        allow.push_back({u.scheme, u.host, u.port});
    } else {
        allow = rt->endpoints();
    }

    auto [status, resp] = http_do("POST", p.url, headers, body_s, allow, p.timeout_ms);
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, resp.data(), resp.size());
    lua_pushinteger(L, status);
    return 3;
}

int rana_decode_json(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    try {
        json::decode(L, std::string(s, len));
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
    return 1;
}

int rana_get_system_prompt(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const std::string p = rt->get_system_prompt();
    lua_pushlstring(L, p.data(), p.size());
    return 1;
}
