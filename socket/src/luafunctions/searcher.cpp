#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace rana {

namespace {

bool valid_module_name(const std::string& s) {
    if (s.empty()) return false;
    if (s.find('/') != std::string::npos) return false;
    if (s.find('\\') != std::string::npos) return false;
    if (s.find('.') != std::string::npos) return false;
    for (char c : s) {
        if (std::isalnum(c)) continue;
        if (c == '_') continue;
        return false;
    }
    return true;
}

int rana_lib_searcher(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    LuaRuntime* rt = rt_from_up(L);
    const std::string scripts_dir = rt->scripts_dir();
    std::string mod(name);
    const std::string lib_prefix = "lib.";
    if (mod.rfind(lib_prefix, 0) != 0) {
        lua_pushnil(L);
        return 1;
    }
    mod = mod.substr(lib_prefix.size());
    if (!valid_module_name(mod)) {
        lua_pushnil(L);
        return 1;
    }
    std::string path = scripts_dir + "/lib/" + mod + ".pluto";
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        lua_pushnil(L);
        return 1;
    }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (luaL_loadbufferx(L, src.data(), src.size(), path.c_str(), "bt") != LUA_OK) {
        lua_error(L);
        return 1;
    }
    return 1;
}

void remove_global(lua_State* L, const char* name) {
    lua_pushnil(L);
    lua_setglobal(L, name);
}

}  // namespace

}  // namespace rana
