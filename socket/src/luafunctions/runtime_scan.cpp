#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef PLUTO_ETL_ENABLE
#include <lstate.h>
#endif

#include <algorithm>
#include <fstream>
#include <dirent.h>
#include <set>
#include <string>
#include <vector>

namespace rana {

namespace {

int rana_lib_searcher(lua_State* L);
void remove_global(lua_State* L, const char* name);

}  // namespace

lua_State* LuaRuntime::new_state() {
    lua_State* L = luaL_newstate();

#ifdef PLUTO_ETL_ENABLE
    lstate_setdeadline(L, 0);
#endif
    luaL_openlibs(L);
    remove_global(L, "io");
    remove_global(L, "os");
    remove_global(L, "package");
    remove_global(L, "debug");
    remove_global(L, "ffi");
    remove_global(L, "socket");
    remove_global(L, "http");
    remove_global(L, "crypto");
    remove_global(L, "wasm");
    lua_getglobal(L, "package");
    lua_pushnil(L);
    lua_setfield(L, -2, "path");
    lua_pushnil(L);
    lua_setfield(L, -2, "cpath");
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, rana_lib_searcher, 1);
    lua_setfield(L, -2, "searchers");
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, rana_lib_searcher, 1);
    lua_setfield(L, -2, "loaders");
    lua_pop(L, 1);
    luaopen_rana(L, this);
    return L;
}

std::vector<std::string> LuaRuntime::available_scripts() const {
    std::vector<std::string> keys;
    DIR* d = opendir(scripts_dir_.c_str());
    if (!d) return keys;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string fn(e->d_name);
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
    scripts_dir_ = cfg.scripts_dir;
    if (scripts_dir_.empty()) {
        const char* envd = std::getenv("RANA_SCRIPTS_DIR");
        if (envd && envd[0]) scripts_dir_ = envd;
        else {
            const char* home = std::getenv("HOME");
            scripts_dir_ = home ? std::string(home) + "/.config/rana-socket/scripts" : "scripts";
        }
    }
    if (scripts_dir_.back() == '/' || scripts_dir_.back() == '\\') scripts_dir_.pop_back();

    {
        std::string init_path = scripts_dir_ + "/init.pluto";
        lua_State* L = new_state();
        if (luaL_dofile(L, init_path.c_str()) != LUA_OK) {
            err = lua_tostring(L, -1);
            lua_close(L);
            return false;
        }
        lua_close(L);
    }

    std::set<std::string> have;
    for (const auto& key : available_scripts()) {
        std::string path = scripts_dir_ + "/" + key + ".pluto";
        lua_State* L = new_state();
        if (luaL_dofile(L, path.c_str()) != LUA_OK) {
            err = "scripts/" + key + ".pluto failed: " + std::string(lua_tostring(L, -1));
            lua_close(L);
            return false;
        }
        if (lua_istable(L, -1)) {
            const int idx = lua_gettop(L);
            std::string name, desc;
            lua_getfield(L, idx, "name"); if (lua_isstring(L, -1)) name = lua_tostring(L, -1); lua_pop(L, 1);
            lua_getfield(L, idx, "description"); if (lua_isstring(L, -1)) desc = lua_tostring(L, -1); lua_pop(L, 1);
            std::vector<std::string> actions = table_to_strvec(L, idx);
            lua_getfield(L, idx, "actions"); auto a2 = table_to_strvec(L, -1); if (!a2.empty()) actions = a2; lua_pop(L, 1);
            std::vector<std::string> keywords = table_to_strvec(L, idx);
            lua_getfield(L, idx, "keywords"); auto k2 = table_to_strvec(L, -1); if (!k2.empty()) keywords = k2; lua_pop(L, 1);
            std::map<std::string, std::string> params;
            lua_getfield(L, idx, "params"); params = table_to_map(L, -1); lua_pop(L, 1);
            uint32_t timeout_ms = 5000;
            lua_getfield(L, idx, "timeout_ms"); if (lua_isnumber(L, -1)) timeout_ms = static_cast<uint32_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
            std::string examples_json;
            lua_getfield(L, idx, "examples"); json::encode(L, -1, examples_json); lua_pop(L, 1);
            bool danger = false;
            lua_getfield(L, idx, "danger"); if (lua_toboolean(L, -1)) danger = true; lua_pop(L, 1);
            std::set<ScriptFlag> flags;
            lua_getfield(L, idx, "flags"); flags = table_to_flags(L, -1); lua_pop(L, 1);
            register_script_meta(key, name, desc, actions, keywords, params, timeout_ms, examples_json, danger, flags);
            have.insert(key);
        }
        lua_pop(L, 1);
        lua_close(L);
    }

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
