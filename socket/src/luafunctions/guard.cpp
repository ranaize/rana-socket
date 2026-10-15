#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <string>
#include <vector>

namespace rana {

namespace {

template <int (*F)(lua_State*)>
int rana_guard(lua_State* L) {
    try {
        return F(L);
    } catch (const std::exception& e) {
        lua_pushboolean(L, false);
        lua_pushstring(L, e.what());
        lua_pushinteger(L, 0);
        return 3;
    } catch (...) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "lua bridge exception");
        lua_pushinteger(L, 0);
        return 3;
    }
}

}  // namespace

}  // namespace rana
