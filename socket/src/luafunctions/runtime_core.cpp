#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef PLUTO_ETL_ENABLE
#include <lstate.h>
#endif

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace rana {

namespace {

int rana_lib_searcher(lua_State* L);
void remove_global(lua_State* L, const char* name);

}  // namespace

LuaRuntime::~LuaRuntime() = default;

bool LuaRuntime::is_allowed(const std::string& key) const {
    return allowed_.count(normalize_key(key)) > 0;
}

void LuaRuntime::allow_binaries(const std::map<std::string, BinaryEntry>& m) {
    for (const auto& [k, v] : m) binaries_.emplace(k, v);
}

void LuaRuntime::allow_endpoints(const std::vector<std::tuple<std::string, std::string, int>>& v) {
    for (const auto& [s, h, p] : v) endpoints_.push_back({s, h, p});
}

void LuaRuntime::register_script_meta(const std::string& key, const std::string& name,
                                     const std::string& description,
                                     const std::vector<std::string>& actions,
                                     const std::vector<std::string>& keywords,
                                     const std::map<std::string, std::string>& params,
                                     uint32_t timeout_ms,
                                     const std::string& examples_json,
                                     bool danger,
                                     const std::set<ScriptFlag>& flags) {
    ScriptMeta m;
    m.name = name.empty() ? key : name;
    m.description = description;
    m.actions = actions;
    m.keywords = keywords;
    m.params = params;
    m.timeout_ms = timeout_ms == 0 ? 5000 : timeout_ms;
    m.examples_json = examples_json;
    m.danger = danger;
    m.flags = flags;
    metas_.emplace(key, std::move(m));
}

std::string LuaRuntime::get_system_prompt() const {
    std::ostringstream ss;
    ss << "You are a command router for rana-socketd. Return exactly one JSON object like {\"key\":\"...\",\"action\":\"...\",\"target\":\"...\",\"params\":{...},\"data\":\"...\"} when the user wants the daemon to act. Use the empty string for action/target when not applicable. Available commands:\n";
    for (const auto& [k, m] : metas_) {
        ss << "- " << k << ": " << m.description;
        if (!m.actions.empty()) ss << " (actions: " << m.actions[0];
        for (size_t i = 1; i < m.actions.size(); ++i) ss << ", " << m.actions[i];
        if (!m.actions.empty()) ss << ")";
        ss << "\n";
        if (!m.keywords.empty()) {
            ss << "  keywords: ";
            for (size_t i = 0; i < m.keywords.size(); ++i) {
                if (i) ss << ", ";
                ss << m.keywords[i];
            }
            ss << "\n";
        }
        if (!m.params.empty()) {
            ss << "  params: ";
            bool first = true;
            for (const auto& [pk, pd] : m.params) {
                if (!first) ss << ", ";
                first = false;
                ss << pk << " (" << pd << ")";
            }
            ss << "\n";
        }
        if (!m.examples_json.empty()) {
            ss << "  examples: " << m.examples_json << "\n";
        }
    }
    ss << "If no command fits, answer conversationally as plain text (do not emit JSON).";
    return ss.str();
}
