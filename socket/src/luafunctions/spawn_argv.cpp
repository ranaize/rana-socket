#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <map>
#include <string>
#include <vector>

namespace rana {

namespace {

bool build_spawn_argv(lua_State* L, const LuaRuntime::BinaryEntry& entry,
                      const std::string& key, std::vector<std::string>& argv,
                      std::string& err) {
    if (entry.vararg) {
        const int top = lua_gettop(L);
        if (top > 1) {
            for (int i = 2; i <= top; ++i) {
                size_t n = 0;
                const char* s = luaL_checklstring(L, i, &n);
                argv.push_back(std::string(s, n));
            }
        }
        return true;
    }
    if (lua_gettop(L) > 1) {
        err = "binary '" + key + "' has a fixed argv; do not pass extra arguments";
        return false;
    }
    argv = entry.args;
    if (argv.empty()) {
        err = "binary '" + key + "' has empty args list";
        return false;
    }
    return true;
}

}  // namespace

}  // namespace rana
