# Enhancement Plan: Scriptable Command System for rana-socket

## Overview

This document outlines a plan to enhance `rana-socket` with a programmable script system using **Pluto** (modern Lua 5.5 fork) as the embedded runtime. The core philosophy:

- **One generic FlatBuffers payload** — the 11 strongly-typed union payloads collapse into a single `Command{key, action, target, params, data}`; the script owns the vocabulary
- **`[commands.allowed]` in TOML is the command registry** — a plain set of enabled scripts; no `variant`, no `path`; `scripts/<key>.pluto` IS the task
- **Scripts carry their own meta** — name, description, actions, params, examples; the ask-hop system prompt is derived from them, not from TOML or hardcoded tables
- **Pluto** — chosen for built-in sandboxing (`PLUTO_ETL_ENABLE`, `PLUTO_ILP_ENABLE`, `PLUTO_NO_BINARIES`), rich stdlib (`json`, `http`, `regex`), modern syntax (`switch`, ternary, string interpolation), and drop-in C API compatibility with standard Lua

## Runtime Decision: Pluto

Pluto is a fork of PUC-Rio Lua 5.5 with extensions. It embeds via the standard Lua C API (`lua.h`, `lauxlib.h`, `lualib.h`) and compiles from C source into a static library — compatible with the existing `-static` build profile.

| Criterion | Pluto Fit |
|-----------|-----------|
| Sandboxing | Built-in: `PLUTO_ETL_ENABLE`, `PLUTO_ILP_ENABLE`, `PLUTO_NO_BINARIES`, `PLUTO_LOADFILE_HOOK`, `PLUTO_DISABLE_COMPILED` |
| Stdlib | `json`, `http`, `socket`, `regex`, `crypto`, `bigint`, `scheduler` — reduces vendored dependencies |
| Syntax | `switch`/`case`, ternary `?:`, `??`, string interpolation, `class`, `continue` |
| C++ integration | Drop-in Lua 5.5 C API, static-linkable from C source |
| Performance | Same as Lua 5.5 interpreter (no JIT — acceptable for command dispatch workloads) |

## Current Structure

The daemon dispatches commands via FlatBuffers union `CommandPayload`. Each union variant has a strongly-typed payload (e.g., `VolumeCmd`, `MediaCmd`). The `[commands.*]` TOML table maps these to executors using `variant` (string match) and `path` (filesystem location). §1 replaces this with a single generic `Command` and a `[commands.allowed]` registry.

Key files:
- `socket/src/executor.h/.cpp` — dispatch core, 644 lines, exhaustive switch on union, `spawn_process` (fork/execvpe)
- `socket/src/config.h/.cpp` — strict TOML loader, `CommandEntry` struct with `variant`/`type`/`path`/`binary`/`workdir`/`timeout_ms`
- `socket/src/main.cpp` — entry point, config load, socket accept loop
- `schema/command.fbs` — FlatBuffers schema (11 command types + `CommandPayload` union; superseded by the generic `Command`, §1)

## Proposed Enhancement

### 1. Flattened FlatBuffers Schema — Generic Command; Script-Owned Vocabulary

Today's schema declares 11 strongly-typed payload tables (`VolumeCmd`, `MediaCmd`, `LightCmd`, …) joined into a `CommandPayload` union. In practice almost every one is already **"action + target"**: `MediaCmd`=(Play/Pause/Stop), `LightCmd`=(Toggle/On/Off), `PowerCmd`=(Shutdown/Reboot/Standby), `McServerCmd`=(Start/Stop/Restart). The differences are only the extra fields (`target:string`, `start_seconds:uint64`, `subnet:string`, …).

So the union collapses into **one generic envelope**. The `key` names the script; the vocabulary (`action`, extra params) belongs to the script, not the schema.

```fbs
// schema/command.fbs (new shape)
namespace rana;

table StrPair {          // named extra; string values, script coerces (tonumber etc.)
    k:string;
    v:string;
}

table Command {
    request_id:uint64;
    key:string;          // → scripts/<key>.pluto; must be listed in [commands.allowed]
    action:string;       // script-defined vocabulary: "play"|"pause"|"toggle"|"set"|...
    target:string;       // primary operand: text, url, room, world, path
    params:[StrPair];    // extras: level, start_seconds, subnet, timeout_ms, ...
    data:[ubyte];        // binary payload (TalkCmd raw audio, …)
}

table Response { request_id:uint64; status:Status; message:string; }
root_type Command;
```

`Status` drops `TYPE_MISMATCH` — there is no variant to mismatch anymore. `AskReply` is unchanged.

#### 1.1 Legacy → generic mapping

| Legacy table | key | action | target | params |
|--------------|-----|--------|--------|--------|
| `VolumeCmd` | `volume` | `set` \| `mute` | — | `level`, `mute` |
| `MediaCmd` | `media` | `play` \| `pause` \| `stop` | device | — |
| `LightCmd` | `lights` | `on` \| `off` \| `toggle` | room | — |
| `PowerCmd` | `power` | `shutdown` \| `reboot` \| `standby` | — | `confirm` |
| `McServerCmd` | `mc_server` | `start` \| `stop` \| `restart` | — | `world` |
| `PlayMovieCmd` | `play_movie` | `play` | path | `start_seconds` |
| `LaunchBrowserCmd` | `browser` | `open` | url | — |
| `LookupMachineCmd` | `lookup_machine` | `lookup` | — | `subnet`, `timeout_ms` |
| `SpeakCmd` | `speak` | `speak` | text | — |
| `AskCmd` | `ask` | `ask` | text | — |
| `TalkCmd` | `talk` | `transcribe` | — | `encoding`, `sample_rate`, `channels` (in `data`) |

The daemon never validates `action`/`params` — it only checks `key` against `[commands.allowed]`, loads `scripts/<key>.pluto`, and passes the rest through to the script.

> Security note: this moves validation from **compile-time schema enums** to **runtime allowlists**. The trust boundary shifts from "the FlatBuffers union is the firewall" to "the config allowlist + `init.lua` binary/network allowlists + the sandbox (§4) are the firewall." That is the intended trade — adding a command becomes "drop a script in, flip one TOML key," and the sandbox is the enforcement point.

#### 1.2 Config — `[commands.allowed]` only

Per-command tables disappear. The config now holds just the daemon settings and the **set of enabled scripts**:

```toml
# config/server.toml (new shape)
[daemon]
socket_type = "tcp"
bind_address = "0.0.0.0"
port = 9000
allowed_ips = ["172.28.0.0/16"]

# The command registry: exactly which scripts this machine exposes.
# A Command whose key is not listed here is refused (Status_FORBIDDEN).
# Everything else about a script (timeouts, descriptions, vocabulary)
# lives in the script's own meta table (§1.3).
[commands.allowed]
volume = true
media = true
browser = true
speak = true
lights = true
ask = true
talk = true
```

No `variant`, no `type`, no `path`, no `binary`, no `workdir`, no per-command `timeout_ms`, no `[stt]` table (STT settings move to constants / script meta). The workspace/laptop configs differ only in which scripts are enabled.

#### 1.3 Inline script meta — the LLM-visible contract

Each script carries its own metadata so the machine is self-describing. The daemon reads the meta at startup (no doc-parsing) and `rana.get_system_prompt()` renders it into the ask-hop system prompt — **the scripts are the vocabulary**, replacing `gen_system_prompt.py`'s TOML-derived list:

```pluto
-- scripts/volume.pluto
local meta = {
    name        = "volume",
    description = "Set the audio volume 0-100, or mute.",
    actions     = { "set", "mute" },
    params      = { level = "0-100", mute = "bool" },
    timeout_ms  = 2000,
    examples    = { { action = "set", params = { level = 50 } } },
}
```

| Field | Purpose |
|-------|---------|
| `name` | command key; must equal the filename |
| `description` | what the script does, for the LLM routing prompt |
| `actions` | accepted values for `Command.action` |
| `params` | doc for the accepted `StrPair` keys (types/ranges) |
| `timeout_ms` | per-script execution budget (overrides the default) |
| `examples` | few-shot samples the LLM prompt includes |

Phase 4 may add Windmill-style doc-comment extraction, but the `meta` table is the authoritative runtime contract from day one.

### 2. Scripts Folder — Implicit, Self-Describing

A `scripts/` folder exists at daemon working directory. The script for command key `X` is always `scripts/X.pluto` (plus optional `scripts/X/scratch/` for its writable scratch, §4.3). No path configuration needed.

The daemon scans `scripts/` on startup and:
1. Registers every `.pluto` file, keyed by filename without extension
2. **Loads each script's `meta` table** (§1.3) — this is what feeds the LLM routing prompt; no meta parse, the daemon just reads the returned table
3. Cross-checks against `[commands.allowed]` — a script present in `scripts/` but not enabled in the config is loaded but refused at dispatch; a key enabled in config but missing from `scripts/` is a startup error

Dispatch: `Command.key` → config allowlist check → `scripts/<key>.pluto`. No variant match, no path lookup, no `type` field.

### 3. Runtime: Pluto (Lua 5.5 Fork)

Pluto is statically compiled from C source and linked into `rana-socketd`. The daemon manages a single `lua_State*` (or per-connection states if concurrency requires isolation). Scripts are loaded from `scripts/` and executed via `lua_pcall`. Dispatch is by `Command.key` only — the executor never switches on a union type.

Build integration:
- Pluto source vendored into `vendor/pluto/`
- New `pluto` static library target in `premake5.lua`
- Linked into `rana-socketd` alongside `rana-serializer` and `pthread`

### 4. Sandboxing Architecture

Pluto scripts run untrusted commands — the LLM may generate arbitrary action strings. Sandboxing is not optional; it is the security boundary.

#### 4.1 Compile-Time Sandboxing (`luaconf.h` toggles)

```c
#define PLUTO_DISABLE_COMPILED       // Prevent loading pre-compiled bytecode
#define PLUTO_NO_BINARIES            // Eliminate package.loadlib entirely
#define PLUTO_ETL_ENABLE             // Execution time limits (per-call)
#define PLUTO_ILP_ENABLE             // Infinite loop prevention (iteration count)
```

These are set in a patched `luaconf.h` shipped with the vendored Pluto source.

#### 4.2 Filesystem Sandboxing — Whitelist Only

Scripts may **only** read/write within `scripts/` and a per-command scratch directory. No access to `/`, `/home`, `/tmp` (except via approved helpers), or any path outside the daemon's working directory.

Implementation in `luafunctions.cpp`:

```cpp
// Whitelist of allowed filesystem prefixes (resolved at daemon startup)
static const std::vector<std::string> kFsWhitelist = {
    "scripts/",                    // script source directory (read-only for non-init scripts)
    "scripts/<command>/scratch/",  // per-command writable scratch (created on demand)
};

// Custom luaL_loadfilex hook — rejects any path not in kFsWhitelist
static int sandboxed_loadfile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::string resolved = resolve_relative(path);
    if (!is_whitelisted(resolved, kFsWhitelist)) {
        return luaL_error(L, "access denied: path '%s' outside sandbox", path);
    }
    // delegate to standard loadfile
    ...
}
```

Scripts **cannot**:
- Use `io.open`, `io.popen`, `io.output` — the `io` library is excluded via `luaL_openselectedlibs`
- Use `os.execute`, `os.rename`, `os.remove`, `os.tmpname` — the `os` library is excluded
- Use `require("ffi")` — `PLUTO_NO_BINARIES` + FFI excluded from selected libs
- Access `/proc`, `/sys`, `/dev` — no path resolution escapes the whitelist
- Use `debug` library — excluded from selected libs

#### 4.3 Script Isolation — No Cross-Script Writes

Each command script runs in its own `lua_State*` (created per-dispatch, destroyed on completion). Scripts share **nothing** — no shared globals, no shared registry, no shared memory.

Per-command scratch directories prevent filesystem-level crosstalk:
```
scripts/
  volume.pluto           # source (read-only at runtime)
  volume/scratch/        # writable by volume.pluto only
  browser.pluto          # source
  browser/scratch/       # writable by browser.pluto only
```

#### 4.4 Kernel / System Call Restrictions

- No `os.execute` / `io.popen` — subprocess spawning only via the approved `rana.spawn()` C function (see §6)
- No `ffi` — cannot call arbitrary C functions or syscalls
- No `debug` library — cannot introspect/modify the VM state
- No `loadstring` with dynamic content — `PLUTO_DISABLE_COMPILED` prevents bytecode loading
- Process spawning delegated to the daemon's `spawn_process` (fork/execvpe with PATH sanitization) — scripts never touch `fork`/`exec` directly
- Network egress is **allowlisted, not blocked**: the `rana.http_*` primitives in `luafunctions.cpp` validate the destination against the endpoint table set in `init.lua` (`rana.allow_endpoints`) before any connection is made. Default: only the server's LocalAI (`http://localai:8080`). No `socket`/`http` stdlib modules are exposed directly to scripts — all network goes through the C bridge.

### 5. `init.lua` — System Interface Registry

`init.lua` lives at `scripts/init.lua` and runs once at daemon startup, before any command scripts. It defines the **allowlist of system applications** and exposes them as a Lua-compatible API.

#### 5.1 Purpose

`init.lua` is the single source of truth for:
1. Which system binaries the scripts may invoke (and with what arguments)
2. Which network endpoints scripts may reach (via `rana.allow_endpoints`)
3. The Lua-side API surface that scripts call (wrapping C functions from `luafunctions.cpp`)
4. Global constants (paths, timeouts, feature flags)

#### 5.2 Structure

```lua
-- scripts/init.lua
-- Executed once at daemon startup. Defines the system interface.

local rana = require("rana")  -- C-side module (luafunctions.cpp)

-- System binary allowlist: name → { path, allowed_args_pattern }
-- Scripts call rana.spawn("espeak", text) — the C side validates against this table.
-- NOTE: rana-ask and rana-stt are NOT here — they are in-process Pluto scripts
-- (§8), invoked directly by the executor, never as subprocesses.
rana.allow_binaries({
    espeak    = { path = "/usr/bin/espeak-ng",  args = { "max", "string" } },
    xdg_open  = { path = "/usr/bin/xdg-open",   args = { "max", "string" } },
    docker    = { path = "/usr/bin/docker",      args = { "compose", "string" } },
})

-- Whitelisted network endpoints (checked in luafunctions.cpp before any
-- rana.http_* call leaves the daemon): the server's own LocalAI.
rana.allow_endpoints({
    { scheme = "http", host = "localai", port = 8080 },
})

-- Global constants accessible to all scripts
rana.set_constants({
    config_path  = rana.get_config_path(),
    socket_path  = rana.get_socket_path(),
    llm_url      = rana.get_llm_url(),
    llm_model    = rana.get_llm_model(),
    stt_url      = rana.get_stt_url(),
    stt_model    = rana.get_stt_model(),
})

-- Sandbox enforcement
rana.set_fs_whitelist({
    "scripts/",                               -- read access to script sources
    rana.get_workdir() .. "/scripts/*/scratch/" -- per-command scratch (write)
})

-- NOTE: there is no command/variant registration here. The registry is
-- [commands.allowed] in TOML (§1.2) + the scripts/ scan (§2); each script
-- self-describes via its meta table (§1.3). init.lua only defines the
-- system interface: binaries, endpoints, constants, filesystem sandbox.
```

#### 5.3 Boot Sequence

```
daemon start
  → load config (TOML: [daemon] + [commands.allowed])
  → create lua_State* (sandboxed luaL_openselectedlibs)
  → execute init.lua (binary/endpoint allowlists, constants, fs whitelist)
  → scan scripts/:
      for each scripts/*.pluto: compile chunk + read its meta table (§1.3)
      cross-check against [commands.allowed]
  → load internal hop scripts: scripts/ask.pluto, scripts/stt.pluto
  → enter accept loop
  → on Command{key,...}:
      allowlist check (key ∈ [commands.allowed]) → else Status_FORBIDDEN
      create isolated lua_State*
      inject compiled chunk + {action, target, params, data} as Lua
      call script's execute(...) under the sandbox
      read return table → FlatBuffers Response
      destroy lua_State*
  → on key="ask"/"talk":
      run ask.pluto / stt.pluto in-process (no subprocess, no shell tools)
```

### 6. `luafunctions.cpp` — C-to-Lua Bridge

`luafunctions.cpp` (or `lua_functions.cpp` for consistency with the codebase) implements the C-side functions that `init.lua` and command scripts call. These are registered as the `"rana"` Lua module.

#### 6.1 Module Structure

```cpp
// luafunctions.cpp

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "config.h"
#include "executor.h"  // for spawn_process

// ── Spawn / Process ──

// rana.spawn(name, arg1, arg2, ...)
// Validates `name` against the binary allowlist, then delegates to spawn_process.
// Returns { exit_code, stdout, stderr, timed_out }.
static int rana_spawn(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    // Validate against allowlist set by rana.allow_binaries()
    ...
    // Build argv from Lua args
    // Call spawn_process(workdir, argv, env, timeout_ms)
    // Push result table
}

// rana.spawn_capture(name, arg1, ...)
// Same as spawn but returns stdout as a string (general utility; the
// ask/stt hops are now in-process and do not use it).
static int rana_spawn_capture(lua_State* L);

// ── HTTP / JSON (ask & stt hop support, §8) ──

// rana.http_post(url, headers, body) → { status, body }
// Single JSON POST to a whitelisted endpoint (checked against init.lua's
// rana.allow_endpoints table). No shell, no curl.
static int rana_http_post(lua_State* L);

// rana.post_multipart(url, fields, files) → { status, body }
// multipart/form-data POST carrying the WAV bytes (STT hop).
static int rana_post_multipart(lua_State* L);

// rana.decode_json(str) → table    /    rana.encode_json(tbl) → string
// JSON (de)serialization backed by Pluto's json stdlib.
static int rana_decode_json(lua_State* L);
static int rana_encode_json(lua_State* L);

// rana.get_system_prompt() → string
// Builds the ask-hop system prompt from the loaded scripts' meta tables
// (§1.3) — the scripts ARE the vocabulary. Replaces gen_system_prompt.py
// and every hardcoded action→variant table.
static int rana_get_system_prompt(lua_State* L);

// rana.build_ask_reply(action, payload) → AskReply FlatBuffer bytes
// Constructs the verified AskReply in-process. Replaces the
// rana-serializer CLI call inside rana-ask.sh.
static int rana_build_ask_reply(lua_State* L);

// rana.get_request_audio() → string
// Hands the decoded TalkCmd WAV bytes to stt.pluto without exposing a
// /tmp path to the sandbox.
static int rana_get_request_audio(lua_State* L);

// ── FlatBuffers / Command Dispatch ──

// rana.dispatch(action, target, params)
// Runs scripts/<script-or-key>.pluto's execute() directly (inter-script
// routing, e.g. task → lights). If `script` is a [commands.allowed] key it
// re-checks the allowlist; the hop-level variant has no union to build.
// Returns { status, message }.
static int rana_dispatch(lua_State* L);

// rana.fetch_command() → key, action, target, params, data
// Retrieves the in-flight generic Command for the executing script
// (used by internal hops like ask/stt to read the originating request).
static int rana_fetch_command(lua_State* L);

// rana.build_response(request_id, status, message)
// Wraps a Response FlatBuffer. Used by scripts that need to construct
// responses manually (e.g., forwarding).
static int rana_build_response(lua_State* L);

// ── Config / Constants ──

// rana.get_config_path() → string
// rana.get_socket_path() → string
// rana.get_llm_url() → string
// rana.get_llm_model() → string
// rana.get_stt_url() → string
// rana.get_stt_model() → string
// rana.get_workdir() → string

// ── Registry (called from init.lua) ──

// rana.allow_binaries(table)
// Sets the binary allowlist. Called once from init.lua.
// Stores as C++ map<string, BinaryAllowEntry>.
static int rana_allow_binaries(lua_State* L);

// rana.allow_endpoints(table)
// Sets the network egress allowlist (scheme/host/port). Called once from
// init.lua. Enforced by the rana.http_* primitives.
static int rana_allow_endpoints(lua_State* L);

// rana.register_script_meta(key, meta_table)
// Records one script's meta (§1.3). Invoked by the startup scan for each
// scripts/*.pluto; feeds rana.get_system_prompt() and the config cross-check.
static int rana_register_script_meta(lua_State* L);

// rana.set_constants(table)
// Stores global constants for script access.
static int rana_set_constants(lua_State* L);

// rana.set_fs_whitelist(table)
// Sets the filesystem sandbox whitelist.
static int rana_set_fs_whitelist(lua_State* L);

// ── Utility ──

// rana.log(level, message)
// Writes to daemon's stderr log (syslog-compatible).
static int rana_log(lua_State* L);

// rana.base64_encode(data) / rana.base64_decode(data)
// Exposes the existing base64 functions to scripts.
static int rana_base64_encode(lua_State* L);
static int rana_base64_decode(lua_State* L);

// ── Module Registration ──

static const luaL_Reg rana_lib[] = {
    { "spawn",               rana_spawn },
    { "spawn_capture",       rana_spawn_capture },
    { "http_post",           rana_http_post },
    { "post_multipart",      rana_post_multipart },
    { "decode_json",         rana_decode_json },
    { "encode_json",         rana_encode_json },
    { "get_system_prompt",   rana_get_system_prompt },
    { "build_ask_reply",     rana_build_ask_reply },
    { "get_request_audio",   rana_get_request_audio },
    { "dispatch",            rana_dispatch },
    { "fetch_command",       rana_fetch_command },
    { "build_response",      rana_build_response },
    { "get_config_path",     rana_get_config_path },
    { "get_socket_path",     rana_get_socket_path },
    { "get_llm_url",         rana_get_llm_url },
    { "get_llm_model",       rana_get_llm_model },
    { "get_stt_url",         rana_get_stt_url },
    { "get_stt_model",       rana_get_stt_model },
    { "get_workdir",         rana_get_workdir },
    { "allow_binaries",      rana_allow_binaries },
    { "allow_endpoints",     rana_allow_endpoints },
    { "register_script_meta", rana_register_script_meta },
    { "set_constants",       rana_set_constants },
    { "set_fs_whitelist",    rana_set_fs_whitelist },
    { "log",                 rana_log },
    { "base64_encode",       rana_base64_encode },
    { "base64_decode",       rana_base64_decode },
    { NULL, NULL }
};

// Called from main.cpp after luaL_newstate()
int luaopen_rana(lua_State* L) {
    luaL_newlib(L, rana_lib);
    return 1;
}
```

#### 6.2 Why `luafunctions.cpp` Instead of More C++ Classes

The existing `Executor` class is tightly coupled to FlatBuffers types — every `run_*` handler takes a typed pointer (`const VolumeCmd*`, etc.). The scriptable system replaces this with a generic path:

1. `luafunctions.cpp` provides thin C wrappers (`rana.spawn`, `rana.dispatch`) that scripts call
2. The generic dispatcher in `executor.cpp` reads the single `Command{key,action,target,params,data}`, allowlists `key`, injects the fields into the script, reads the return value
3. Per-command C++ handlers (`run_volume`, `run_browser`, `dispatch_inner`, …) are **deleted** — their logic moves into `.pluto` scripts

This keeps the C++ side minimal: `luafunctions.cpp` (~450 lines), `executor.cpp` (generic dispatch only, ~200 lines), `config.cpp` (a handful of fields instead of `CommandEntry`).

### 7. Script Example — Volume Control

`scripts/volume.pluto`:

```lua
local rana = require("rana")

-- §1.3: the LLM-visible contract; the daemon reads this at startup.
local meta = {
    name        = "volume",
    description = "Set the audio volume 0-100, or mute.",
    actions     = { "set", "mute" },
    params      = { level = "0-100", mute = "bool" },
    timeout_ms  = 2000,
    examples    = { { action = "set", params = { level = 50 } } },
}

local function execute(cmd)
    -- cmd = { key="volume", action="set", target="", params={...}, data=... }
    -- cmd.params is a string-keyed table (StrPair list converted by the C bridge;
    -- values are strings, script coerces).
    local level = tonumber(cmd.params.level) or 50
    local mute  = cmd.params.mute == "true"

    rana.log("info", "setting volume to " .. level)

    local result = rana.spawn("espeak",
        "--volume=" .. (mute and "0" or tostring(level)),
        "volume set"
    )

    if result.timed_out then
        return { status = "error", message = "volume command timed out" }
    end
    if result.exit_code ~= 0 then
        return { status = "error", message = "espeak failed: " .. result.stderr }
    end
    return { status = "ok", message = "volume set to " .. level }
end

return { meta = meta, execute = execute }
```

### 8. Hop Migration — `rana-ask.sh` / `rana-stt.sh` Become Pluto Scripts

The current LLM and STT hops are external shell scripts spawned via `spawn_process`. In the Pluto world they become **in-process scripts** — no subprocess, no `curl`, no `jq`, no `python3`. Pluto's `http` and `json` stdlibs cover the wire work; the AskReply FlatBuffer is built in-process by `luafunctions.cpp`.

| Legacy file | Becomes | Supersedes |
|-------------|---------|------------|
| `scripts/rana-ask.sh` | `scripts/ask.pluto` | `curl`, `jq`, `rana-serializer` (ask-hop role) |
| `scripts/rana-stt.sh` | `scripts/stt.pluto` | `curl`, `jq` |
| `scripts/gen_system_prompt.py` | `luafunctions.cpp` → `rana.get_system_prompt()` | `python3`, `tomllib` |

These are ordinary `[commands.allowed]` scripts like any other — `ask.pluto` and `stt.pluto` are just not meant for user dispatch. The executor calls them internally by key, exactly like a client would:
- `Executor::run_ask_text` → loads `scripts/ask.pluto` instead of spawning `rana-ask.sh`
- `Executor::run_talk` → loads `scripts/stt.pluto` instead of spawning `rana-stt.sh`

#### 8.1 `scripts/ask.pluto` — the LLM hop

```pluto
local rana = require("rana")

local function ask(question)
    local sys = rana.get_system_prompt()   -- rendered from scripts' meta (§1.3)
    local body = {
        model = rana.get_llm_model(),
        messages = {
            { role = "system", content = sys },
            { role = "user",   content = question },
        },
        response_format = { type = "json_object" },
        temperature = 0,
        max_tokens = 256,
    }

    if not question or question == "" then
        -- No question: answer safely without touching LocalAI.
        return rana.build_ask_reply("reply", "I did not catch that.")
    end

    local resp = rana.http_post(rana.get_llm_url() .. "/v1/chat/completions",
                                nil,                      -- headers
                                rana.encode_json(body))    -- raw JSON body
    if not resp or resp.status ~= 200 then
        return rana.build_ask_reply("reply", "Sorry, I had no answer.")
    end

    local data = rana.decode_json(resp.body)
    local content = data and data.choices and data.choices[1]
                    and data.choices[1].message and data.choices[1].message.content
    if not content or content == "" then
        return rana.build_ask_reply("reply", "Sorry, I had no answer.")
    end

    -- rana.build_ask_reply() builds the AskReply FlatBuffer in-process —
    -- no rana-serializer subprocess, no JSON parsing in the daemon's C++.
    local routed = rana.decode_json(content)  -- { action = "...", payload = "..." }
    return rana.build_ask_reply(routed.action, routed.payload)
end

return { ask = ask }
```

The daemon's stdout-capture contract for the ask hop is gone: `ask.pluto` returns the AskReply **directly** as a Lua table (or byte blob from `rana.build_ask_reply`), an in-process handoff that is json-free on the C++ side.

#### 8.2 `scripts/stt.pluto` — the STT hop

```pluto
local rana = require("rana")

-- The decoded WAV bytes never touch the filesystem from the script's side:
-- the C++ executor decodes TalkCmd audio, then hands the bytes in via
-- rana.get_request_audio() — no /tmp path is exposed to the sandbox.
local function stt()
    local wav = rana.get_request_audio()          -- WAV bytes as a Lua string
    if not wav or wav == "" then
        return "", "missing audio"
    end
    -- rana.post_multipart() returns { status, body }: a multipart/form-data POST
    -- carrying the WAV bytes to LocalAI (STT hop). No curl, no temp file path.
    local resp = rana.post_multipart(
        rana.get_stt_url() .. "/v1/audio/transcriptions",
        { model = rana.get_stt_model() },
        { { name = "file", filename = "input.wav",
            mime = "audio/wav", data = wav } })
    if not resp or resp.status ~= 200 then
        return "", "stt request failed"
    end
    local data = rana.decode_json(resp.body)
    return (data and data.text) or ""
end

return { stt = stt }
```

`Executor::run_talk` then feeds the returned transcript into `ask.pluto` exactly as it does today — the C++ flow, audio decode → stt → ask → dispatch, is unchanged.

#### 8.3 New `luafunctions.cpp` primitives required by the hops

| Primitive | Purpose | Replaces |
|-----------|---------|----------|
| `rana.http_post(url, headers, body)` | JSON POST to LocalAI | `curl -sS -f -X POST` |
| `rana.post_multipart(url, fields, files)` | multipart upload of WAV bytes | `curl -F file=@...` |
| `rana.decode_json(str)` / `rana.encode_json(tbl)` | JSON (de)serialization | `jq` |
| `rana.get_system_prompt()` | render the ask system prompt from loaded scripts' `meta` tables (§1.3) | `gen_system_prompt.py` + `tomllib` + the hardcoded action→variant table |
| `rana.build_ask_reply(action, payload)` | construct the verified `AskReply` FlatBuffer in-process | `rana-serializer` CLI (ask-hop role) |
| `rana.get_request_audio()` | hand the decoded WAV bytes to the STT script without exposing `/tmp` | temp-file path plumbed through the sandbox |

> If raw multipart upload proves awkward in Lua (field ordering / MIME edge cases), the fallback is a C++ primitive in `luafunctions.cpp` — **not** reverting to a shell hop. The `http`+`json` parts are proven in Pluto; only the multipart wrapper may need C++ assistance.

#### 8.4 Runtime dependency impact

Once the hops are in-process, the daemon runtime image drops `curl`, `jq`, and `python3` from the hop path (see `Dockerfile`). `espeak-ng` stays (a system binary invoked via `rana.spawn`, now allow-listed in `init.lua`). The `rana-serializer` binary keeps its **audio codec** role (TalkCmd decode) but loses its ask-hop CLI role.

### 9. Task Handler — Script-Defined Tasks Mapping to the Command Registry

The C++ `dispatch_inner` currently hard-codes an action→payload mapping (reply/open_browser/search_web/toggle_lights/shutdown), each building a typed FlatBuffers `Command`. With the generic schema this whole layer disappears:

- **Tasks live in scripts.** Each `.pluto` script defines `execute(cmd)` and self-describes via `meta` (§1.3).
- **Mapping to the registry.** `[commands.allowed]` + the `scripts/` scan are the registry — no C++ alias table, no variant map, no typed-`Command` builder (`key` is already in the generic envelope).
- **LLM keyword resolution.** `ask.pluto` gets the LLM's `{action, payload}` and resolves it to a `key` via the routing prompt metadata, then hands the fields to that script — either directly (`rana.dispatch`, in-process) or by emitting the generic `Command` back to the client for forwarding. No C++ switch.

Implication: the **hardcoded commands currently integrated into the API**
(the `CommandPayload` union and `Executor::dispatch_inner`) are gone — they are
exactly what this scriptable system replaces. Scripts become the single source
of task behavior; the C++ side keeps only the generic dispatch path (§6) plus
the `[commands.allowed]` allowlist.

### 10. Roadmap

- **Phase 0** (3-5 days): Vendor Pluto; build as static lib; prove `lua_State*` creation + script loading + `lua_pcall` in `executor.cpp`; create `luafunctions.cpp` with `rana.spawn`, `rana.log`, `rana.http_post`, `rana.build_ask_reply`; test with a single command script
- **Phase 1** (2-3 weeks): **Flatten `schema/command.fbs`** to generic `Command{key,action,target,params,data}`; collapse `config.cpp` to `[daemon]` + `[commands.allowed]`; write `init.lua`; implement `rana.allow_binaries`/`rana.allow_endpoints`/`rana.register_script_meta`; scan `scripts/` + read meta at startup; migrate 3-4 commands to `.pluto` scripts (volume, browser, lights, speak); port `rana-ask.sh` → `ask.pluto` and `rana-stt.sh` → `stt.pluto` (§8); build the routing prompt from meta; drop `curl`/`jq`/`python3` from the hop path in the `Dockerfile`
- **Phase 2** (1-2 weeks): Full sandboxing (`PLUTO_ETL_ENABLE`, `PLUTO_ILP_ENABLE`, `PLUTO_NO_BINARIES`, `luaL_openselectedlibs`); filesystem whitelist enforcement; per-command scratch isolation; `rana.dispatch` for inter-command routing; error reporting + timeout handling; network endpoint allowlist for the `http_*` primitives
- **Phase 3** (1-2 weeks): Delete every `run_*` handler and `dispatch_inner` from `executor.cpp`; delete the `Status` `TYPE_MISMATCH`; `executor.cpp` becomes generic dispatch only (~200 lines); migrate remaining commands (mc_server, power, play_movie, lookup_machine, talk)
- **Phase 4** (long-term): Windmill-inspired metadata-driven interfaces (doc-comment → meta extraction); optional hot-reload of scripts without daemon restart

### 11. Integration with Existing Systems

- **Strongly-typed payloads dropped**: The 11 `*Cmd` tables and `CommandPayload` union collapse into the generic `Command` (§1). `TYPE_MISMATCH` removed from `Status`. Clients build `key`/`action`/`target`/`params`/`data` directly.
- **`[commands.allowed]` replaces `[commands.*]`**: per-command `variant`/`type`/`path`/`binary`/`workdir`/`timeout_ms` are gone; the config is a set of enabled script keys. All per-script tuning lives in the script meta.
- **No backward compatibility**: old `path`-based `.sh` configs, `.sh` invocations, and typed-payload clients are **not** preserved — see Stale / Dropped Ideas.
- **`dispatch_inner` replaced**: deleted; LLM actions resolve to keys in the script layer (§9).
- **Hops are in-process**: `rana-ask.sh`, `rana-stt.sh`, and `gen_system_prompt.py` are deleted; their behavior moves into `ask.pluto` / `stt.pluto` / `luafunctions.cpp` (§8). `curl`, `jq`, and `python3` leave the runtime image.

### 12. Conclusion

The scriptable command system for rana-socket is designed to be:
- **Pluto-powered** — embedded Lua 5.5 with built-in sandboxing and rich stdlib
- **Schema-light** — one generic `Command{key,action,target,params,data}`; no per-command payloads, no union, no variant checks
- **Self-describing** — every script ships its own `meta` table that feeds the LLM routing prompt
- **Secure by default** — `[commands.allowed]` + filesystem whitelist, no `io`/`os`/`debug`/`ffi`, per-command isolation, execution time limits, infinite loop prevention, endpoint allowlist
- **Hop-internal** — the LLM/STT hops run as in-process Pluto scripts, removing the subprocess + shell-tooling layer entirely
- **Minimal config** — `[commands.allowed]` + daemon settings only
- **Implicit scripts folder** — script is always `scripts/X.pluto`
- **Thin C++ layer** — `luafunctions.cpp` (~450 lines) provides the bridge; `executor.cpp` (~200 lines) is generic dispatch only

## Stale / Dropped Ideas

- **Backward compatibility with existing `.sh` (former §4)** — STALE: **not needed**. We do not preserve old `path`-based shell-script configs or `.sh` invocations; scripts live in the implicit `scripts/` folder keyed by filename, and configs adopt the `[commands.allowed]` layout directly. No transition path for legacy `.sh` configs is required.
- **Strongly-typed FlatBuffers payloads** — STALE: the 11 `*Cmd` tables + `CommandPayload` union collapse into the generic `Command{key,action,target,params,data}` (§1). Clients and daemon speak one envelope; the script owns the vocabulary. `Status.TYPE_MISMATCH` is removed.
- **Per-command TOML tables (`[commands.*]`)** — STALE: replaced by `[commands.allowed]` (a set of enabled script keys) + inline script `meta`. No `variant`/`type`/`path`/`binary`/`workdir`/`timeout_ms` in config.
- **Language benchmarking (Phase 0)** — STALE: Pluto is selected. No further candidate evaluation needed.
- **Pallene/Titan, Teal, Ravi, MoonScript** — STALE: not selected. Removed from consideration.
- **`rana-ask.sh` / `rana-stt.sh` / `gen_system_prompt.py` as subprocess hops** — STALE: converted to in-process Pluto scripts (`ask.pluto`, `stt.pluto`) and a C++ primitive (`rana.get_system_prompt()`). The `curl`/`jq`/`python3` toolchain they required is no longer part of the runtime image.
- **`rana-serializer` CLI as the ask-hop codec** — STALE: the AskReply FlatBuffer is now built in-process by `luafunctions.cpp` (`rana.build_ask_reply`). `rana-serializer` keeps only its audio-codec role for TalkCmd.
