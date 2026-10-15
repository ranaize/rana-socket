# AGENTS.md

Guidance for AI agents (and humans) working in **rana-socket**. Keep it read
before you touch `scripts/` or the Lua bridge; the deeper rationale lives in
`plan.md`, `plan-hf-fallback.md`, `docs/security.md`, and the source comments.

## What this is

`rana-socketd` is a FlatBuffers-framed command daemon. A client
(`rana-voice-agent`, the C++ CLI, the Python client) sends one
`Command{key, action, target, params, data}` frame per request. The daemon
dispatches on `key` and runs **a Pluto script** under `scripts/`.

C++ owns the socket, the framing, config parsing, and the *escape hatches*
(process spawn, HTTP, provider chains). **Policy lives in the reviewed scripts.**
There is no shell-out dispatch table in C++.

## Build, run, test

```sh
make                       # release -> bin/Release/rana-socketd (+ CLI client)
premake5 gmake --tls       # optional: build TLS (Hugging Face provider chain)
premake5 gmake --tls-dynamic   # optional: force dynamic -lssl -lcrypto
```

- TLS is **off by default** and compiled out without a flag; `RANA_ENABLE_TLS`
  is the only macro that gates it. Builds driven by `predep` pass the flag via
  the stage's `flags` list (see `docs/predep-flags-spec.md`).
- Run: `bin/Release/rana-socketd <config.toml>`; the client takes one address
  (`/path/sock` or `tcp:host:port`).

Tests run under **Pluto** (pinned to the same version the daemon embeds —
currently `0.13.1`), not stock Lua:

```sh
pluto tests/check_scripts.lua   # load-check init.pluto + every scripts/*.pluto
pluto tests/test_lib.lua        # unit tests for scripts/lib/*.pluto
```

CI installs the pinned `0.13.1` release asset and runs both. Run them locally
with the interpreter from <https://github.com/PlutoLang/Pluto/releases> before
pushing. `luajit`/`lua5.4` **cannot** parse the scripts.

## The script layer (read before editing `scripts/`)

- **Pluto, not Lua.** Every script file is `.pluto` and may use Pluto syntax
  (interpolation, `??`, `?.`, ternary, switch, `continue`, default args, ...).
  See `skills/pluto.md`.
- `scripts/init.pluto` runs **once** at startup and is the **single source of
  truth for the escape-hatch boundary**: `rana.allow_binaries{...}` and
  `rana.allow_endpoints{...}`. A new host binary or outbound endpoint is added
  **only here**, with the narrowest possible shape (prefer a hardcoded `args`
  argv over `vararg = true`). It is not a command module and returns no `meta`.
- Every other `scripts/<key>.pluto` returns `{ meta = <table>, execute = <fn> }`.
  The daemon scans `scripts/*.pluto` at startup and **hard-fails** on a bad one.
- Shared helpers live in `scripts/lib/<name>.pluto` and are reached with
  `require("lib.<name>")`. A **restricted searcher** resolves only `lib.*`;
  `package.path`/`cpath` are empty and `io`, `os`, `package`, `debug`, `load`,
  `loadfile`, `dofile`, `ffi`, `socket`, `http`, `crypto`, `wasm` are stripped
  from the state. Do not depend on any other module.

### `meta` (what the C side reads)

| field        | type              | meaning |
|--------------|-------------------|---------|
| `name`       | string            | human label (defaults to `key`) |
| `description`| string            | one line; fed to the LLM system prompt **and** the fuzzy router |
| `actions`    | string[]          | inner verbs (first is advertised to the model) |
| `keywords`   | string[]          | fuzzy-router vocabulary |
| `params`     | map<string,string>| name -> description |
| `timeout_ms` | integer           | per-script budget (default 5000) |
| `examples`   | {action,target}[] | few-shot examples in the system prompt |
| `danger`     | boolean           | fuzzy router matches it on an **exact keyword phrase only** |
| `flags`      | `{ rana.flags.* }`| `LONG_IDLE` adds a 60s grant; `EXTERNAL_EGRESS` opts the script in to external answers |

### `rana` bridge

| call | returns | notes |
|------|---------|-------|
| `rana.spawn(name, ...)` | `{ ok, code, out, err, reason? }` | `name` must be in `init.pluto`; wrap with `lib/exec` |
| `rana.http_post(url, body[, headers])` | `ok, body, status` | **http only**; URL must be in `init.pluto` |
| `rana.providers(kind)` | `{ {type, schema, model, name}, ... }` | `kind` = `"llm"`/`"stt"`; no URL/token |
| `rana.provider_post(kind, i, body[, headers])` | `ok, body, status` | the **only** TLS caller; `i` is 1-based |
| `rana.json_decode(s)` | `table\|nil` | |
| `rana.get_system_prompt()` | string | |
| `rana.dispatch(key, action, {target=..., params=...})` | `{status, message}` | runs locally |
| `rana.respond(key, action, {target=..., params=...})` | — | stashes a `FORWARD_TO_CLIENT` for the peer |
| `rana.has_script(key)` | boolean | |
| `rana.script_metas()` | `map<key,{description, keywords, danger, external_egress}>` | |
| `rana.log(level, msg)` | — | `debug`/`info`/`warning`/`error` |
| `rana.audio.decode_to_wav(data, enc, rate, channels)` | string | |
| `rana.flags.LONG_IDLE`, `rana.flags.EXTERNAL_EGRESS` | integer | for `meta.flags` |

`status` (`3rd` return of the HTTP calls) is `0` on a transport failure/gate
refusal and the real HTTP code otherwise: only `0` advances a provider chain;
a live 4xx/5xx is the upstream's own answer.

### Result convention

`execute()` returns (see `scripts/lib/result.pluto`):

- a `{status, message}` table built by `result.ok/error/forbidden/invalid/safety`;
- a bare `nil`/`true`/string, which the executor coerces;
- nothing, when a relay was stashed with `rana.respond` (nil means OK).

### Egress / the provider chain

- A huggingface provider is usable only when the build has TLS **and**
  `[security].external_egress` is true, per call (`{ cloud = false }` opts out).
- An answer that came from a huggingface provider is stamped `external`; it may
  only be *acted on* by a script that opts in with `rana.flags.EXTERNAL_EGRESS`
  (**off by default**). Refusals go through `scripts/lib/refuse.pluto` and are
  never dispatched or relayed. Never trust the reply text for origin.

## Conventions

- Keep conversions **behavior-preserving** unless the task says otherwise.
- Comments explain *why* (the boundary, the failure mode), not *what*.
- Prefer the reviewed helpers (`lib/exec`, `lib/result`, `lib/verbs`,
  `lib/multipart`, `lib/fuzzy`, `lib/llm`, `lib/route`, `lib/refuse`) over new
  ad-hoc code; extend the helper instead.
- A new command needs: `scripts/<key>.pluto`, an `[commands.allowed]` entry, and
  (if it spawns) a binary in `init.pluto`.
- Do not add `table.freeze` (not compiled into the daemon runtime); use the
  hand-rolled `setmetatable` pattern in `lib/refuse.pluto` for immutability.
- Run both tests under `pluto` before finishing. `check_scripts` will fail a
  `.pluto` file that does not return a command module — keep `init.pluto`
  excluded from that scan (it is handled separately).

## Where things live

```
socket/src/luafunctions.cpp   Lua bridge, sandbox, script scan, run_entrypoint
socket/src/config.{h,cpp}     config + scripts-dir discovery (looks for init.pluto)
scripts/init.pluto            binary + endpoint allowlists (the trust boundary)
scripts/<key>.pluto           command modules
scripts/lib/*.pluto           reviewed shared helpers
tests/check_scripts.lua       load-check (run under pluto)
tests/test_lib.lua            unit tests (run under pluto)
skills/pluto.md              Pluto language guide for this repo
docs/security.md              full egress/sandbox model
```
