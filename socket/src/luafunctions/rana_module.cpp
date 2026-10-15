#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "audio.hpp"

namespace rana {

namespace {

template <int (*F)(lua_State*)>
int rana_guard(lua_State* L);

int rana_spawn(lua_State* L);
int rana_http_post(lua_State* L);
int rana_providers(lua_State* L);
int rana_provider_post(lua_State* L);
int rana_decode_json(lua_State* L);
int rana_get_system_prompt(lua_State* L);
int rana_dispatch(lua_State* L);
int rana_has_script(lua_State* L);
int rana_respond(lua_State* L);
int rana_allow_binaries(lua_State* L);
int rana_allow_endpoints(lua_State* L);
int rana_script_metas(lua_State* L);
int rana_log(lua_State* L);
int rana_audio_decode_to_wav(lua_State* L);

}  // namespace

const luaL_Reg kRanaLib[] = {
    {"spawn", rana_guard<rana_spawn>},
    {"http_post", rana_guard<rana_http_post>},
    {"providers", rana_guard<rana_providers>},
    {"provider_post", rana_guard<rana_provider_post>},
    {"json_decode", rana_guard<rana_decode_json>},
    {"get_system_prompt", rana_guard<rana_get_system_prompt>},
    {"dispatch", rana_guard<rana_dispatch>},
    {"has_script", rana_guard<rana_has_script>},
    {"script_metas", rana_guard<rana_script_metas>},
    {"respond", rana_guard<rana_respond>},
    {"allow_binaries", rana_guard<rana_allow_binaries>},
    {"allow_endpoints", rana_guard<rana_allow_endpoints>},
    {"log", rana_guard<rana_log>},
    {nullptr, nullptr},
};

const luaL_Reg kRanaAudioLib[] = {
    {"decode_to_wav", rana_guard<rana_audio_decode_to_wav>},
    {nullptr, nullptr},
};

void luaopen_rana(lua_State* L, LuaRuntime* rt) {
    lua_newtable(L);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, kRanaLib, 1);
    lua_newtable(L);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, kRanaAudioLib, 1);
    lua_setfield(L, -2, "audio");
    lua_pushinteger(L, static_cast<int>(ScriptFlag::LongIdle));
    lua_setfield(L, -2, "LONG_IDLE");
    lua_pushinteger(L, static_cast<int>(ScriptFlag::ExternalEgress));
    lua_setfield(L, -2, "EXTERNAL_EGRESS");
    lua_setglobal(L, "rana");
}

}  // namespace rana
