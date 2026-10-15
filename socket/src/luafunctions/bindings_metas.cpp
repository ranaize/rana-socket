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

}  // namespace

int rana_script_metas(lua_State* L) {
    LuaRuntime* rt = rt_from_up(L);
    lua_newtable(L);
    for (const auto& [key, m] : rt->metas()) {
        lua_newtable(L);
        lua_pushstring(L, m.description.c_str());
        lua_setfield(L, -2, "description");
        lua_newtable(L);
        for (size_t i = 0; i < m.keywords.size(); ++i) {
            lua_pushstring(L, m.keywords[i].c_str());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "keywords");
        lua_pushboolean(L, m.danger);
        lua_setfield(L, -2, "danger");
        bool ee = (m.flags.count(ScriptFlag::ExternalEgress) > 0);
        lua_pushboolean(L, ee);
        lua_setfield(L, -2, "external_egress");
        lua_setfield(L, -2, key.c_str());
    }
    return 1;
}

int rana_log(lua_State* L) {
    const char* lvl = luaL_checkstring(L, 1);
    const char* msg = luaL_checkstring(L, 2);
    rana::log_msg("script", "[%s] %s", lvl, msg);
    return 0;
}

int rana_audio_decode_to_wav(lua_State* L) {
    size_t dlen = 0;
    const char* data = luaL_checklstring(L, 1, &dlen);
    const std::string enc = luaL_optstring(L, 2, "pcm16");
    const int sr = static_cast<int>(luaL_optinteger(L, 3, 16000));
    const int ch = static_cast<int>(luaL_optinteger(L, 4, 1));
    std::string wav = audio::decode_to_wav(std::string(data, dlen), enc, sr, ch);
    lua_pushlstring(L, wav.data(), wav.size());
    return 1;
}
