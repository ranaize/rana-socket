#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef PLUTO_ETL_ENABLE
#include <lstate.h>
#endif

#include <chrono>
#include <set>
#include <string>
#include <vector>
#include <thread>

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

uint32_t LuaRuntime::current_timeout() const { return current_timeout_; }
uint32_t LuaRuntime::current_etl_timeout() const { return current_etl_timeout_; }

bool LuaRuntime::tls_built() {
#ifdef RANA_ENABLE_TLS
    return true;
#else
    return false;
#endif
}

const std::vector<ProviderConfig>* LuaRuntime::providers(const std::string& kind) const {
    if (cfg_ == nullptr) return nullptr;
    if (kind == "llm") return &cfg_->llm_providers;
    if (kind == "stt") return &cfg_->stt_providers;
    return nullptr;
}

std::string LuaRuntime::provider_unavailable_reason(const std::string& kind, int index) const {
    const auto* provs = providers(kind);
    if (!provs || index < 1 || static_cast<size_t>(index) > provs->size()) return "bad_args";
    const ProviderConfig& p = (*provs)[index - 1];
    if (p.type == "huggingface") {
        if (!tls_built()) return "tls_not_built";
        if (!cfg_ || !cfg_->external_egress) return "disabled";
    }
    return {};
}

bool LuaRuntime::has_script(const std::string& key) const {
    return metas_.count(key) > 0;
}

void LuaRuntime::request_respond(const std::string& key, const std::string& action,
                                const std::string& target,
                                const std::map<std::string, std::string>& params) {
    pending_respond_ = PendingRespond{key, action, target, params};
}
