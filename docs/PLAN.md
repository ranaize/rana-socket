# Enhancement Plan: Scriptable Command System for rana-socket

## Overview

This document outlines a plan to enhance `rana-socket` with a programmable script system. The core philosophy:

- **`[commands.*]` in TOML IS the command registry** — no `variant`, no `path`; the TOML key maps to a script in the selected runtime, and that script *is* the task (the legacy FlatBuffers union variant is what this system supersedes)
- **Scripts live in a `scripts/` folder** — script for command X is `scripts/X` (extension determined by chosen framework)
- **No framework decisions now** — language/runtime chosen after Phase 0 benchmarking

## Current Structure

The daemon dispatches commands via FlatBuffers union `CommandPayload`. Each union variant has a strongly-typed payload (e.g., `VolumeCmd`, `MediaCmd`). The `[commands.*]` TOML table maps these to executors using `variant` (string match) and `path` (filesystem location).

## Proposed Enhancement

### 1. `[commands.*]` is the Registry — No `variant`, No `path`

Both `variant` and `path` are redundant — the TOML key IS the command:

```toml
[commands.volume]
type = "script"
timeout_ms = 2000
```

Implicit mappings:
- TOML key `volume` → FlatBuffers union variant `VolumeCmd`
- TOML key `volume` → script file `scripts/volume.{ext}`
- TOML key `lookup_machine` → FlatBuffers union variant `LookupMachineCmd`
- TOML key `lookup_machine` → script file `scripts/lookup_machine.{ext}`

The executor dispatches by looking up the TOML key and matching the FlatBuffers payload union variant automatically — no string comparison for variant, no path lookup for script location.

> Note: this interim union-variant mapping is the *legacy* model the scriptable system is meant to supersede — see §7. Ultimately the TOML key resolves to a **script** (task function in the selected Lua/runtime), not a hardcoded union variant.

### 2. Scripts Folder — Implicit

A `scripts/` folder exists at daemon working directory. The script for `[commands.X]` is always `scripts/X` (with framework-specific extension). No path configuration needed.

The daemon scans `scripts/` on startup and registers any script files it finds, keyed by filename (without extension).

### 3. Language Framework — TBD Through Benchmarking (Phase 0)

No language/runtime decision is made upfront. Candidates evaluated:

| Language | Role | Embedding | Performance | Type Safety |
|----------|------|-----------|-------------|-------------|
| **Pallene / Titan** | Replace C for speed-critical paths | External compiler, linked as shared object | Near C (AoT compiled) | Static, explicit |
| **Teal** | Typed Lua with strong type annotations | Transpiles to standard Lua, or compiled to Lua | Same as Lua | Strong (typed) |
| **Ravi** | General-purpose embedded interpreter | Link as shared library into C++ daemon | 2-10x Lua 5.4 (JIT) | Optional, gradual |
| **MoonScript** | Developer ergonomics (Python-like) | External compiler, output is standard Lua | Same as Lua (after transpile) | Inherited from Lua (dynamic) |
| **Pluto** | Modern Lua fork with QOL features | External compiler, output is standard Lua | Same as Lua | Inherited from Lua (dynamic) |

**Benchmarking criteria** (Phase 0, 2-3 days):
- Startup latency: config load to first script ready
- Per-command overhead: frame received to script exit
- Memory overhead per concurrent script context
- Throughput: commands/second under concurrent clients
- Integration cost: C++ binding lines for FlatBuffers params, subprocess spawning, response generation
- Type ergonomics: effort to express volume/light/media commands

### 5. Runtime Integration (What Gets Implemented After Phase 0)

After language selection in Phase 0, the runtime provides:

1. **Script Loading**: Load scripts from `scripts/` folder on startup
2. **Parameter Passing**: Convert FlatBuffers fields to runtime-accessible format (env vars, table, etc.)
3. **Subprocess Spawning**: Delegate to existing `spawn_process` / `execvpe` mechanism
4. **Response Generation**: Map script exit code + output to FlatBuffers `Response`
5. **Sandboxing** (if applicable): Restrict filesystem/network based on chosen runtime

### 6. Roadmap

- **Phase 0** (2-3 days): Prototype top 2 language candidates; benchmark against rana-socket workloads; select primary + fallback
- **Phase 1** (2-3 weeks): Create `scripts/` folder; update config loader (implicit path, no `variant` validation); add `type = "script"` support; migrate example commands to new format
- **Phase 2** (1-2 weeks): Sandboxing capabilities; better script API (FlatBuffers field access); error reporting; timeout + resource limits
- **Phase 3** (long-term): Windmill-inspired metadata-driven interfaces; auto-generated FlatBuffers schemas from script comments; optional hot-reload
- **Phase 2 (task-handler)**: Task definitions move into the selected Lua system and map onto the `[commands.*]` registry (see §7) — replacing both the hardcoded FlatBuffers union variants and the `Executor::dispatch_inner` switch. The LLM keyword resolves through the script layer to a command-table key; no C++ alias table or typed-`Command` builder.

## Task Handler — Script-Defined Tasks Mapping to the Commands Table (future)

Correction to the earlier framing: a C++ `task-handler.cpp` that maps an LLM
keyword to a hardcoded typed `Command` (building `SpeakCmd`/`LightCmd`/…) defeats
the point of this plan. The whole point of the scriptable system is that the
**tasks are defined in the selected Lua (or other embedded) system**, and those
script functions map directly onto the `[commands.*]` registry — *not* onto
predefined union variants baked into the C++ API.

So the "task handler" is a property of the script layer, not the daemon:

- **Tasks live in scripts.** A Lua module defines task functions (e.g.
  `tasks.lights`, `tasks.movie`). The selected runtime (Phase 0) loads them from
  `scripts/`.
- **Mapping to the commands table.** Each script function registers itself — or
  is discovered by filename (§2) — as a `[commands.X]` entry. The TOML key maps
  to the script function; the script supplies the behavior. The key → union-variant
  rule from §1 is what this supersedes.
- **LLM keyword resolution.** An LLM keyword lands in the script layer and is
  resolved there to a `[commands.*]` key (a Lua table / dispatch), which then
  invokes the registered script function. No C++ switch, no typed-`Command`
  builder, no alias table in the daemon.

Implication: the **predefined hardcoded tasks currently integrated into the API**
(the FlatBuffers `CommandPayload` union and `Executor::dispatch_inner`) may not be
needed at all — they are exactly what this scriptable system is meant to replace.
Scripts become the single source of task behavior; the C++ side keeps only the
generic exec path (§5) plus a thin registry lookup.

This keeps the daemon thin and JSON-free: the script layer consumes the verified
`AskReply` FlatBuffer and resolves it to a command-table key, and the generic
executor runs the corresponding script — no per-task C++ code.

## Integration with Existing Systems

- **`variant` field dropped**: Removed from TOML validation entirely; the TOML key maps directly to the command/script (§1), no union-variant string match.
- **`path` field optional**: Scripts in `scripts/` need none; only non-scripted external executors (if any remain) supply a `path`.
- **No backward compatibility**: old `path`-based `.sh` configs and `.sh` invocations are **not** preserved — see Stale / Dropped Ideas. Configs adopt the `[commands.*]`-keyed `scripts/` layout directly.

## Conclusion

The scriptable command system for rana-socket is designed to be:
- **Framework-agnostic** — language choice through data-driven benchmarking
- **Minimal config** — `[commands.X]` key IS the registry; no `variant`, no `path`
- **Implicit scripts folder** — script is always `scripts/X`
- **Inspired by Windmill** — scripts become discoverable typed APIs without boilerplate

The immediate next step is Phase 0: benchmarking language candidates and selecting the primary runtime before any code changes beyond config loader updates.

## Stale / Dropped Ideas

- **Backward compatibility with existing `.sh` (former §4)** — STALE: **not needed**. We do not preserve old `path`-based shell-script configs or `.sh` invocations; scripts live in the implicit `scripts/` folder keyed by filename, and configs adopt the `[commands.*]`-keyed layout directly. No transition path for legacy `.sh` configs is required.
