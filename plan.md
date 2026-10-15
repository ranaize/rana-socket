# Plan: v0.0.1 → main handoff

Status: **all phases implemented** (0–5), except the two items that need a real
C/C++ toolchain (C++ compile verification, the e2e smoke test) and the stash
drops, which need human action. Nothing is committed yet.

Scope: make the scripted Pluto daemon safe and coherent to merge into `main`.
Full review notes (with file/line evidence) are in the session; this file is the
actionable checklist and the record of what changed.

Guiding rule: the **C bridge is the trust boundary** (tiny, allowlisted,
never-throws across the Lua boundary); the **trusted Lua stdlib** owns ergonomics;
**scripts stay thin glue**.

---

## Phase 0 — repo hygiene (non-code)

- [ ] Drop the 3 superseded stashes (`git stash list`: *good pluto*, *WIP*,
      *big pluto change*). They are older drafts of the same work and only add
      confusion. **Requires human confirmation before dropping.**
- [x] Removed the stale generated artifacts that referenced the deleted
      `serializer/`: `rana-serializer.make`, `rana-serializer-cli.make`,
      `socket/bin/Release/{rana-ask.sh,rana-stt.sh,gen_system_prompt.py}` (+ the
      stale `rana-serializer` binary). (`Makefile` + `*.make` are gitignored;
      local only.)

## Phase 1 — merge blockers

- [x] **README**: rewrote the `ask` section to `[commands.allowed] ask = true`
      (the parser rejects `[commands.ask]`). Removed `AskCmd`,
      `/usr/local/bin/rana-ask.sh`, `TYPE_MISMATCH`, and the
      `rana.build_ask_reply`/`AskReply` flow description. It now uses
      `rana.respond` + `rana.dispatch`. Also fixed the `power
      <shutdown|standby|reboot>` line (→ `reboot|suspend`) and the stale
      "one typed FlatBuffer per sub-command" wording, and added a Python-client
      install section.
- [x] **Python client packaging** (D1 resolved): keep the generated module
      **out of git** (repo convention — root `.gitignore` ignores
      `schema/generated/`, and the in-package copy was deleted in `138be74`), and
      wire up generation instead:
      - `premake5.lua` `gen_schema_daemon()` now `cp`s the generated module into
        `client/py/rana_socket/` on every daemon build;
      - `client/py/gen_command_generated.sh` regenerates it standalone for
        Python-only work;
      - `client/py/.gitignore` and the `commands.py` docstring/README document it.
      NOTE: the harness mounts `.git` read-only, so the previously-created
      untracked copy could not be unstaged with `git rm --cached` from here — the
      working copy is removed; drop the index entry before committing if it still
      shows (`git rm --cached client/py/rana_socket/command_generated.py`).
- [x] **premake `fb_include`**: dropped the stale
      `vendor/flatbuffers/flatbuffers-24.3.25/include`; now
      `FLATBUFFERS_INCLUDE` or `/usr/local/include` (system `flatbuffers`
      headers + the pinned `flatc` 25.12.19, matching the Dockerfile).

## Phase 2 — C bridge correctness (bugfixes, no semantics change)

All items implemented in `socket/src/luafunctions.{cpp,h}`. **Not yet compiled**
(no C/C++ toolchain in this environment) — verify with `make` on a real host.

- [x] **`rana.spawn` nil-safety**: always returns a table
      `{ ok, code, out, err, reason }` (`reason` ∈
      `nil|not_allowed|bad_args|spawn_failed|timeout|exit`). All 9 spawn call
      sites ported via `lib/exec.run` (see Phase 3).
- [x] **Exception safety**: the `rana_guard<F>` trampoline wraps every registered
      `lua_CFunction` (kRanaLib + kRanaAudioLib); C++ exceptions no longer cross
      the Lua boundary.
- [x] **URL/port parsing**: `parse_int` (strtol, base-aware) replaces the
      throwing `std::stoi/stoul` in `parse_url`, the HTTP status line, and the
      chunk-size parse.
- [x] **HTTP socket timeout**: `set_socket_timeout` (SO_RCVTIMEO/SO_SNDTIMEO)
      bounds `http_do`, using the script's ETL deadline.
- [x] **https honesty**: `parse_url` now rejects non-`http` schemes instead of
      silently connecting in the clear; `docs/security.md` corrected (TLS,
      searchers, fs-whitelist, `rana.command` claims).
- [x] **Numeric params**: `table_to_map` formats integral numbers without a
      decimal (`16000`, not `16000.000000`).
- [x] **Long-idle leak**: split `current_timeout_` (spawn) from
      `current_etl_timeout_` (VM deadline + HTTP hops); spawn no longer inherits
      the +60s LONG_IDLE grant.
- [x] **`Host` header**: includes the port when non-default.

## Phase 3 — trusted Lua stdlib + thin scripts

- [x] **Restricted searcher**: `new_state()` installs exactly ONE loader
      (`rana_lib_searcher`) resolving `require("lib.<name>")` →
      `scripts_dir_/lib/<name>.lua`; module names are `[A-Za-z0-9_]` segments, so
      traversal and every other namespace stay unloadable.
- [x] `lib/result.lua` — `ok/error/forbidden/invalid` constructors + the status
      strings the bridge understands.
- [x] `lib/exec.lua` — `run(name, ...) -> (ok, result|message)` over the
      normalized spawn; failure messages name the binary.
- [x] Port the 9 command scripts (`speak`, `lights`, `media`, `volume`, `power`,
      `browser`, `play_movie`, `lookup_machine`, `mc_server`): each ~5-line
      failure ladder collapses to `exec.run` + `result.*`. Verified offline with
      `lua /tmp/opencode/test_libs.lua {ok,fail}` → 9/9 on both paths.
- [x] `lib/route.lua` — owns the `has_script → dispatch → forbidden → respond`
      tail duplicated in `ask.pluto`, plus the `DEFAULT_ACTION` map.
- [x] `lib/fuzzy.lua` — the stopword/tokenize/overlap router (from `ask.pluto`),
      `extract_url`, and `route` (verbatim logic, incl. the `play_movie`
      URL-payload branch).
- [x] `lib/verbs.lua` — shared free-text verb inference (`fold`/`infer`/
      `first_number`) for power/mc_server/media/lights/volume.
- [x] `lib/llm.lua` — `extract_json` / `parse_action` + `ask` (fuzzy-then-LLM).
- [x] `lib/multipart.lua` — multipart/form-data body builder (replaces the body
      hand-rolled in `stt.pluto` that the removed `http_post_multipart` fed).
- [x] Port `ask`, `talk`, `stt` (the hop chain) to the helpers; behaviour
      preserved (statuses collapse to the same enum: `error`/`invalid` →
      `EXECUTION_FAILED`/`INVALID_PAYLOAD`).
- [x] Ship the helpers: `Dockerfile` now also copies `scripts/lib/` to
      `/opt/rana/scripts/lib/` (it only shipped `scripts/*.pluto` before, so every
      `require("lib.*")` would have failed at runtime in the image).

## Phase 4 — remove dead surface

Verified registered-but-unused (or stored-but-never-read):
`build_ask_reply`, `build_response`, `fetch_command`, `json_encode`,
`http_post_multipart`, `register_script_meta`, `base64_encode`,
`base64_decode`, `rana_spawn_capture` (unregistered), `constants_`,
`fs_whitelist_`, and `schema/ask_reply.fbs` + `ask_reply_generated.h`.

- [x] Remove them — done in `socket/src/luafunctions.{cpp,h}`, `premake5.lua`,
      `socket/premake5.lua`, and `schema/ask_reply.fbs` (+ the generated header).
      The `LuaRuntime::register_script_meta` **method** stays (the startup scan
      uses it); only its now-dead Lua binding went. The `base64_encode`/`json::encode`
      C helpers stay (still used by the FORWARD path and the scan). Also dropped
      the orphaned `get_config_path`/`get_socket_path`/`get_workdir` getters and
      their `current_*` accessors/members.
- [x] Note: `build_ask_reply`/`build_response` are also an attack surface
      (scripts forging raw FlatBuffers), so removal is a security win.

## Phase 5 — tests / CI

- [x] Script load-check (`tests/check_scripts.lua`): `init.lua` runs and every
      `scripts/*.pluto` returns a module table with `meta` + `execute` (the daemon
      hard-fails startup on one bad script).
- [x] Lib unit tests (`tests/test_lib.lua`): verbs/multipart/result/route/fuzzy/llm
      under a stubbed `rana` bridge.
- [ ] One end-to-end smoke test: build command → frame → executor → `speak`.
      **Blocked here** — needs a built daemon (no C/C++ toolchain in this
      environment). Add alongside the Docker build.
- [x] Minimal CI workflow (`.github/workflows/ci.yml`): Lua load-check + unit
      tests. (The build job is deferred with the e2e test above.)

---

## Decisions

- **D1 (python gen) — RESOLVED**: keep generated code out of git (matches the
  root `.gitignore` and the deliberate deletion in `138be74`). Generation is now
  wired into `premake5.lua`'s `gen_schema_daemon` (copies into the package) plus
  the standalone `client/py/gen_command_generated.sh`; `pip install` needs the
  file present first (documented in README + the `commands.py` docstring).
- **D2 (power confirm) — RESOLVED (enforced)**: `power.pluto` requires
  `cmd.params.confirm` (the clients send `params.confirm = "true"` only when
  `--confirm`/`confirm=True`), and otherwise fails closed with
  `result.safety(...)` → `SAFETY_GATE_FAILED`. This also means a voice-routed
  power intent (no confirm) is refused — deliberate.
- **D3 (https/TLS)**: reject `https` now (honest, no half-implementation) vs.
  implement TLS. Default: reject now, note as a future extension.

## Out of scope (flagged, not done here)

- Multi-client concurrency (daemon handles one connection at a time).
- Per-binary freeform-arg validation beyond the existing `--`/option-injection
  surface.
- Richer audio codecs (extension point already isolated in `audio.hpp`).
