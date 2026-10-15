#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <map>
#include <string>

namespace rana {

namespace {

void args_to_target_params(lua_State* L, int idx,
                           std::string& target,
                           std::map<std::string, std::string>& params);
bool args_bool(lua_State* L, int idx, const char* field);

}  // namespace

int rana_dispatch(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* key = luaL_checkstring(L, 1);
    std::string action = lua_isnoneornil(L, 2) ? "" : luaL_checkstring(L, 2);
    std::string target;
    std::map<std::string, std::string> params;
    args_to_target_params(L, 3, target, params);
    const bool internal = args_bool(L, 3, "internal");
    RanaResult r = rt->run_command(key, action, target, params, std::vector<uint8_t>(), !internal);
    lua_pushstring(L, status_to_string(r.status).c_str());
    lua_pushlstring(L, r.message.data(), r.message.size());
    return 2;
}

int rana_has_script(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* key = luaL_checkstring(L, 1);
    lua_pushboolean(L, rt->has_script(key));
    return 1;
}

int rana_respond(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* key = luaL_checkstring(L, 1);
    std::string action = lua_isnoneornil(L, 2) ? "" : luaL_checkstring(L, 2);
    std::string target;
    std::map<std::string, std::string> params;
    args_to_target_params(L, 3, target, params);
    rt->request_respond(key, action, target, params);
    return 0;
}

int rana_allow_binaries(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    if (!lua_istable(L, 1)) return 0;
    std::map<std::string, LuaRuntime::BinaryEntry> m;
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING) { lua_pop(L, 1); continue; }
        const std::string key(lua_tostring(L, -2));
        if (lua_istable(L, -1)) {
            LuaRuntime::BinaryEntry be;
            lua_getfield(L, -1, "path");
            if (lua_isstring(L, -1)) be.path = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "vararg");
            if (lua_toboolean(L, -1)) be.vararg = true;
            lua_pop(L, 1);
            lua_getfield(L, -1, "args");
            if (lua_istable(L, -1)) {
                const int n = static_cast<int>(luaL_len(L, -1));
                for (int i = 1; i <= n; ++i) {
                    lua_rawgeti(L, -1, i);
                    if (lua_isstring(L, -1)) be.args.push_back(lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            if (!be.path.empty()) m.emplace(key, std::move(be));
        }
        lua_pop(L, 1);
    }
    rt->allow_binaries(m);
    return 0;
}

int rana_allow_endpoints(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    std::vector<std::tuple<std::string, std::string, int>> v;
    if (lua_istable(L, 1)) {
        const int n = static_cast<int>(luaL_len(L, 1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            if (lua_istable(L, -1)) {
                std::string scheme, host;
                int port = 0;
                lua_getfield(L, -1, "scheme");
                if (lua_type(L, -1) == LUA_TSTRING) scheme = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "host");
                if (lua_type(L, -1) == LUA_TSTRING) host = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "port");
                port = static_cast<int>(lua_tointeger(L, -1));
                lua_pop(L, 1);
                v.emplace_back(scheme, host, port);
            }
            lua_pop(L, 1);
        }
    }
    rt->allow_endpoints(v);
    return 0;
}
