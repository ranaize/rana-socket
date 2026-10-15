#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace rana {

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
            size_t klen = 0;
            const char* k = lua_tolstring(L, -2, &klen);
            if (lua_isstring(L, -1)) {
                size_t vlen = 0;
                const char* v = lua_tolstring(L, -1, &vlen);
                m.emplace(std::string(k, klen), std::string(v, vlen));
            } else if (lua_isnumber(L, -1)) {
                double d = lua_tonumber(L, -1);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", d);
                m.emplace(std::string(k, klen), std::string(buf));
            } else if (lua_isboolean(L, -1)) {
                m.emplace(std::string(k, klen), lua_toboolean(L, -1) ? "true" : "false");
            }
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
        if (lua_isstring(L, -1)) {
            size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            v.emplace_back(s, len);
        }
        lua_pop(L, 1);
    }
    return v;
}

std::set<ScriptFlag> table_to_flags(lua_State* L, int idx) {
    std::set<ScriptFlag> out;
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) return out;
    const int n = static_cast<int>(luaL_len(L, idx));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        if (lua_isinteger(L, -1)) {
            const int f = static_cast<int>(lua_tointeger(L, -1));
            if (f == static_cast<int>(ScriptFlag::LongIdle)) out.insert(ScriptFlag::LongIdle);
            if (f == static_cast<int>(ScriptFlag::ExternalEgress)) out.insert(ScriptFlag::ExternalEgress);
        }
        lua_pop(L, 1);
    }
    return out;
}

}  // namespace

}  // namespace rana
