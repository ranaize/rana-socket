#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <map>
#include <string>
#include <vector>

namespace rana {

namespace {

int rana_lib_searcher(lua_State* L);
void remove_global(lua_State* L, const char* name);
bool build_spawn_argv(lua_State* L, const LuaRuntime::BinaryEntry& entry,
                      const std::string& key, std::vector<std::string>& argv,
                      std::string& err);

}  // namespace

int rana_spawn(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    const char* name = luaL_checkstring(L, 1);
    const auto it = rt->binaries().find(name);
    if (it == rt->binaries().end()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "binary not allowed");
        return 2;
    }
    std::vector<std::string> argv;
    std::string err;
    if (!build_spawn_argv(L, it->second, name, argv, err)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    SpawnResult sr = spawn_process("", argv, {}, rt->current_timeout(), /*capture_stdout=*/true);
    lua_newtable(L);
    lua_pushboolean(L, !sr.timed_out && !sr.spawn_failed);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, sr.exit_code);
    lua_setfield(L, -2, "code");
    lua_pushlstring(L, sr.stdout_text.data(), sr.stdout_text.size());
    lua_setfield(L, -2, "out");
    lua_pushlstring(L, sr.stderr_text.data(), sr.stderr_text.size());
    lua_setfield(L, -2, "err");
    if (sr.timed_out) {
        lua_pushstring(L, "timeout");
        lua_setfield(L, -2, "reason");
    }
    if (sr.spawn_failed) {
        lua_pushstring(L, "spawn_failed");
        lua_setfield(L, -2, "reason");
    }
    return 1;
}
