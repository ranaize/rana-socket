#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "command_generated.h"
#include "config.h"

struct lua_State;  // opaque; defined by the embedded Pluto/Lua headers in the .cpp

namespace rana {

// Script capability flags. Scripts opt in via M.meta.flags = { <regged ints> }.
// The enum (not a magic string) is the contract: C++ owns the meaning and the
// enforcement points, scripts own the assignment. Values are exposed to Pluto as
// integer constants in the rana.flags table so scripts can't typo them.
enum class ScriptFlag : int {
    LongIdle = 1,  // add a generous idle/sleep budget on top of meta.timeout_ms
};

// ── Shared result types ────────────────────────────────────────────
// Disambiguation vs the flatbuffer namespace: this is the daemon-internal
// result of running a command script.
struct RanaResult {
    Status status = Status_OK;
    std::string message;
};
// Forwarding is rare and is expressed IN the result, not around it: when a
// script calls rana.respond(...), the outermost run_command encodes the
// inner Command as base64 in `message` and sets status=FORWARD_TO_CLIENT.
// The wire response (status + message) then needs no extra wrapper.

// A command a script asks the daemon to hand to the PEER (client daemon) as a
// fresh inner Command. Set via rana.respond(key, action, target, params) and
// consumed only by the outermost LuaRuntime::run_command, which re-wraps it as
// a FORWARD_TO_CLIENT response the agent relays to its local daemon.
struct PendingRespond {
    std::string key;
    std::string action;
    std::string target;
    std::map<std::string, std::string> params;
};

// ── Process spawning (used by rana.spawn and the legacy fork path) ──
struct SpawnResult {
    int exit_code = -1;
    bool timed_out = false;
    bool spawn_failed = false;
    std::string stderr_text;
    std::string stdout_text;
};

// argv[0] is the executable; no shell involved. When capture_stdout is true the
// child's stdout is collected into the result, otherwise inherited by the daemon.
SpawnResult spawn_process(const std::string& workdir,
                         const std::vector<std::string>& argv,
                         const std::vector<std::string>& extra_env,
                         uint32_t timeout_ms,
                         bool capture_stdout = false);

// RFC4648 base64 (no newline).
std::string base64_encode(const std::vector<uint8_t>& in);
std::string base64_decode(const std::string& in);

// ── Lua runtime: owns the (sandboxed) Pluto interpreter ──────────
// One LuaRuntime is created at daemon startup. It loads init.lua (the system
// interface registry) and scans scripts/ for *.pluto files and their meta
// tables. Per command it spins up an isolated lua_State* (created fresh,
// destroyed on completion) so scripts share nothing.
class LuaRuntime {
public:
    LuaRuntime() = default;
    ~LuaRuntime();

    // Load init.lua, scan scripts/, cross-check against [commands.allowed].
    bool init(const Config& cfg, std::string& err);

    // Run a normal command script (scripts/<key>.pluto) and return its result.
    // enforce_allowed=false lets scripts call a non-allowlisted script in-process
    // (a hop): the [commands.allowed] gate is for the edge only. When the script
    // called rana.respond(...) at the top level, the returned RanaResult carries
    // the base64 inner Command with status=FORWARD_TO_CLIENT.
    RanaResult run_command(const std::string& key,
                           const std::string& action,
                           const std::string& target,
                           const std::map<std::string, std::string>& params,
                           const std::vector<uint8_t>& data,
                           bool enforce_allowed = true);

    // True if scripts/<key>.pluto exists (regardless of [commands.allowed]).
    bool has_script(const std::string& key) const;

    // Stash a command for the PEER daemon (see PendingRespond). Consumed by the
    // outermost run_command; the script does not need to return a value.
    void request_respond(const std::string& key, const std::string& action,
                         const std::string& target,
                         const std::map<std::string, std::string>& params);

    // ── registry accessors (called from the C bridge) ──
    bool is_allowed(const std::string& key) const;
    std::string scripts_dir() const { return scripts_dir_; }
    const Config* config() const { return cfg_; }

    // One script's self-description, read from its meta at startup. description
    // + keywords + danger feed the ask hop's fuzzy pre-router (transcript →
    // best-matching script, LLM skipped).
    struct ScriptMeta {
        std::string name;
        std::string description;
        std::vector<std::string> actions;
        std::vector<std::string> keywords;
        std::map<std::string, std::string> params;
        uint32_t timeout_ms = 5000;
        std::string examples_json;
        bool danger = false;  // exact keyword phrase match only (no partial)
        std::set<ScriptFlag> flags;
    };

    // key → ScriptMeta for every scanned scripts/*.pluto (feed for the fuzzy
    // pre-router / system prompt).
    const std::map<std::string, ScriptMeta>& metas() const { return metas_; }

    // Per-dispatch context + allowlists visible to the C bridge functions.
    // A vararg binary (espeak, xdg-open, ...) appends the script's args verbatim;
    // a non-vararg `args` list is a hardcoded argv and rana.spawn(name) must be
    // called with NO further arguments (enforced in the C bridge).
    struct BinaryEntry {
        std::string path;
        bool vararg = false;
        std::vector<std::string> args;
    };
    struct Endpoint { std::string scheme; std::string host; int port; };

    const std::map<std::string, BinaryEntry>& binaries() const;
    const std::vector<Endpoint>& endpoints() const;
    uint32_t current_timeout() const;
    const std::string& current_key() const;
    const std::string& current_action() const;
    const std::string& current_target() const;
    const std::map<std::string, std::string>& current_params() const;
    std::string current_data_str() const;

    // Registry mutators invoked by init.lua.
    void allow_binaries(const std::map<std::string, BinaryEntry>& m);
    void allow_endpoints(const std::vector<std::tuple<std::string, std::string, int>>& v);
    void set_fs_whitelist(const std::vector<std::string>& v);
    void set_constants(const std::map<std::string, std::string>& m);
    void register_script_meta(const std::string& key, const std::string& name,
                             const std::string& description,
                             const std::vector<std::string>& actions,
                             const std::vector<std::string>& keywords,
                             const std::map<std::string, std::string>& params,
                             uint32_t timeout_ms,
                             const std::string& examples_json,
                             bool danger,
                             const std::set<ScriptFlag>& flags);

    // Build the ask-hop system prompt from the registered script metas.
    std::string get_system_prompt() const;

private:
    // Create a freshly sandboxed lua_State with the rana module registered.
    lua_State* new_state();
    std::vector<std::string> available_scripts() const;

    // Run scripts/<key>.pluto, always dispatching to its M.execute(cmd) entry;
    // the cmd table is built from params/data/action/target. Returns the script's
    // RanaResult (see the return-value convention at the interpreter in
    // luafunctions.cpp). Scripts reach each other only via rana.dispatch.
    RanaResult run_entrypoint(const std::string& key,
                              const std::map<std::string, std::string>* params,
                              const std::vector<uint8_t>* data,
                              const std::string& action,
                              const std::string& target);

    const Config* cfg_ = nullptr;
    std::string scripts_dir_;
    std::set<std::string> allowed_;
    std::map<std::string, BinaryEntry> binaries_;
    std::vector<Endpoint> endpoints_;
    std::vector<std::string> fs_whitelist_;
    std::map<std::string, std::string> constants_;
    std::map<std::string, ScriptMeta> metas_;

    // Per-dispatch context available to scripts via rana.fetch_command.
    std::string current_key_;
    std::string current_action_;
    std::string current_target_;
    std::map<std::string, std::string> current_params_;
    std::vector<uint8_t> current_data_;
    uint32_t current_timeout_ = 5000;

    // Nesting depth of run_command (1 = outermost edge dispatch). rana.dispatch
    // hops re-enter run_command; pending_respond_ must survive them and be
    // consumed only by the outermost frame.
    int command_depth_ = 0;

    std::optional<PendingRespond> pending_respond_;

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;
};

// Registers the "rana" module into an already-created state, binding the engine
// as upvalue #1 of every C function. Called from LuaRuntime::new_state().
void luaopen_rana(lua_State* L, LuaRuntime* rt);

}  // namespace rana
