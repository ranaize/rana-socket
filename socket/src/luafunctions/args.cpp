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
                           std::map<std::string, std::string>& params) {
    if (!lua_istable(L, idx)) return;
    lua_getfield(L, idx, "target");
    if (!lua_isnil(L, -1)) {
        size_t n = 0;
        const char* t = lua_tolstring(L, -1, &n);
        target.assign(t, n);
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    params = table_to_map(L, idx);
    params.erase("target");
    params.erase("internal");
}

bool args_bool(lua_State* L, int idx, const char* field) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, field);
    const bool v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

}  // namespace

}  // namespace rana
