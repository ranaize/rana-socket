#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef PLUTO_ETL_ENABLE
#include <lstate.h>
#endif

#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace rana {

namespace {

int rana_lib_searcher(lua_State* L);
void remove_global(lua_State* L, const char* name);
bool build_spawn_argv(lua_State* L, const LuaRuntime::BinaryEntry& entry,
                      const std::string& key, std::vector<std::string>& argv,
                      std::string& err);

}  // namespace

lua_State* LuaRuntime::new_state();
std::vector<std::string> LuaRuntime::available_scripts() const;

RanaResult LuaRuntime::run_entrypoint(const std::string& key,
                                     const std::map<std::string, std::string>* params,
                                     const std::vector<uint8_t>* data,
                                     const std::string& action,
                                     const std::string& target) {
    lua_State* L = new_state();
    std::string path = scripts_dir_ + "/" + key + ".pluto";
    if (luaL_dofile(L, path.c_str()) != LUA_OK) {
        std::string em = lua_tostring(L, -1);
        lua_close(L);
        return RanaResult{Status_EXECUTION_FAILED, em};
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_close(L);
        return RanaResult{Status_EXECUTION_FAILED, "module is not a table"};
    }
    lua_getfield(L, -1, "execute");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        lua_close(L);
        return RanaResult{Status_EXECUTION_FAILED, "missing execute"};
    }
    lua_newtable(L);
    if (params) {
        for (const auto& [k, v] : *params) {
            lua_pushlstring(L, v.data(), v.size());
            lua_setfield(L, -2, k.c_str());
        }
    }
    if (data) {
        lua_pushlstring(L, reinterpret_cast<const char*>(data->data()), data->size());
        lua_setfield(L, -2, "data");
    }
    lua_pushlstring(L, action.data(), action.size());
    lua_setfield(L, -2, "action");
    lua_pushlstring(L, target.data(), target.size());
    lua_setfield(L, -2, "target");
    const auto meta_it = metas_.find(key);
    const uint32_t timeout_ms = meta_it != metas_.end() ? meta_it->second.timeout_ms : 5000;
    bool long_idle = (meta_it != metas_.end() && meta_it->second.flags.count(ScriptFlag::LongIdle) > 0);
    current_timeout_ = timeout_ms;
    current_etl_timeout_ = long_idle ? (timeout_ms + 60000) : timeout_ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(current_etl_timeout_);
#ifdef PLUTO_ETL_ENABLE
    lstate_setdeadline(L, static_cast<int64_t>(current_etl_timeout_));
#endif
    if (lua_pcall(L, 1, 3, 0) != LUA_OK) {
        std::string em = lua_tostring(L, -1);
        lua_close(L);
        return RanaResult{Status_EXECUTION_FAILED, em};
    }
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#ifdef PLUTO_ETL_ENABLE
    lstate_setdeadline(L, 0);
#endif
    int top = lua_gettop(L);
    if (top >= 1 && lua_isstring(L, 1)) {
        std::string s(lua_tostring(L, 1));
        RanaResult r;
        r.status = status_from_string(s);
        if (top >= 2 && lua_isstring(L, 2)) r.message = lua_tostring(L, 2);
        lua_close(L);
        return r;
    }
    if (top >= 1 && lua_istable(L, 1)) {
        lua_getfield(L, 1, "status");
        std::string st = lua_isstring(L, -1) ? lua_tostring(L, -1) : "ok";
        lua_pop(L, 1);
        lua_getfield(L, 1, "message");
        std::string msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        lua_close(L);
        return RanaResult{status_from_string(st), msg};
    }
    lua_close(L);
    return RanaResult{Status_OK, ""};
}

RanaResult LuaRuntime::run_command(const std::string& key,
                                  const std::string& action,
                                  const std::string& target,
                                  const std::map<std::string, std::string>& params,
                                  const std::vector<uint8_t>& data,
                                  bool enforce_allowed) {
    std::string nk = normalize_key(key);
    if (enforce_allowed && allowed_.count(nk) == 0) {
        return RanaResult{Status_FORBIDDEN, "command not allowed"};
    }
    current_params_ = params;
    ++command_depth_;
    pending_respond_.reset();
    RanaResult r = run_entrypoint(nk, &current_params_, &data, action, target);
    std::optional<PendingRespond> respond;
    if (command_depth_ == 1 && pending_respond_) {
        respond = std::move(*pending_respond_);
        pending_respond_.reset();
    }
    --command_depth_;
    if (respond) {
        flatbuffers::FlatBufferBuilder b(256);
        std::vector<flatbuffers::Offset<StrPair>> ps;
        for (const auto& [k, v] : respond->params) {
            ps.push_back(CreateStrPair(b, b.CreateString(k), b.CreateString(v)));
        }
        const auto cmd = CreateCommandDirect(b, 0,
            respond->key.c_str(), respond->action.c_str(), respond->target.c_str(),
            ps.empty() ? nullptr : &ps, nullptr);
        b.Finish(cmd);
        std::vector<uint8_t> inner(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
        return RanaResult{Status_FORWARD_TO_CLIENT, base64_encode(inner)};
    }
    return r;
}
