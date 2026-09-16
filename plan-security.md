# Plan: fix Pluto build + harden the embedded scripting runtime

Status: **implementation done**, awaiting rebuild verification by the user.

## Context
`rana-socket` embeds Pluto to run LLM-generated `.pluto` scripts. Two problems:

1. **Build was broken** — `errors.txt` showed a wall of
   `undefined reference to lua_*`. Root cause: the `pluto` premake project globs
   `vendor/pluto/src/**.c`, but Pluto 0.13.1 ships **C++** sources
   (`lapi.cpp`, `lffi.cpp`, ...). So `libpluto.a` compiled **zero** files and the
   whole Lua API was undefined at link.
2. **Runtime was unsafe by default** — `new_state()` called `luaL_openlibs(L)`,
   which not only opens every library but preloads `ffi`/`socket`/`http`/
   `crypto`/`wasm` into `package.preload`. Stripping those globals afterwards did
   nothing, because `require("ffi")` still worked.

## Changes

### Build: move Pluto out of in-tree compile into a predep `make` stage
The previous approach compiled Pluto's ~160 `.cpp` files (plus Soup) inside
premake. That was both fragile (the original `**.c` glob matched nothing → empty
`libpluto.a`) and slow. Instead, predep now **builds Pluto as a shared library**
and `rana-socketd` links the prebuilt `libpluto.so`.

- `predep.toml`: `[[vendor]] pluto` gained `builder = "pluto"`; added
  `[[stages]] name = "pluto" type = "make"` with
  `source = "root://vendor/pluto/src"`, `targets = ["libpluto.so"]`, and the
  hardening flags passed via `MYCFLAGS`
  (`PLUTO_DISABLE_COMPILED`, `PLUTO_NO_BINARIES`, `PLUTO_DISABLE_HTTP_COMPLETELY`,
  `PLUTO_ETL_ENABLE`, `PLUTO_ILP_ENABLE`). The `build` stage now depends on
  `["vendor", "pluto"]`. `libpluto.so` is added to the install artifacts so it
  ships next to `rana-socketd`.
- `socket/premake5.lua`: removed the in-tree `project "pluto"` StaticLib.
  `rana-socketd` now does `libdirs { vendor/pluto/src }`,
  `links { "pluto", ... }` and sets an rpath
  (`-Wl,-rpath,$$ORIGIN` + the build path) so the `.so` is found next to the
  binary at runtime. Release config dropped `-static` (incompatible with a shared
  lib) but keeps `-static-libgcc`/`-static-libstdc++`.
- Soup is built as part of Pluto's own `libpluto.so` target (its Makefile under
  `vendor/pluto/src/vendor/Soup/soup`), so no separate Soup step is needed.

### Runtime hardening (`socket/src/luafunctions.cpp`, `new_state()`)
- Replaced `luaL_openlibs(L)` with
  `luaL_openselectedlibs(L, kRanaLoadLibs, 0)`.
  - `kRanaLoadLibs` opens only: `_G`, `package`, `coroutine`, `debug`
    (needed for Pluto startup, then stripped), `math`, `string`, `table`,
    `utf8`, `json`, `base64`, `regex`.
  - Preload mask `0` → nothing registered for `require`.
- Sandbox now also strips `ffi`, `socket`, `http`, `tls`, `crypto`, `wasm` and
  **empties** `package.preload` / `package.searchers` / `package.loaders`,
  nils `package.loadlib`, and purges `package.loaded` for all of the above.
- Always-on (no config toggle).

### Docs
- New `docs/security.md`: threat model + the four defense layers + operational
  rules (never revert to `luaL_openlibs`).

## Verification (user must run)
```
predep                 # vendors pluto, builds libpluto.so via the make stage,
                       # then premake5 + builds rana-socketd
```
Expected:
- `vendor/pluto/src/libpluto.so` is produced by the `pluto` make stage.
- `rana-socketd` links cleanly against it (no `undefined reference to lua_*`).
- The old `errors.txt` wall of link errors is gone.
- `ldd socket/bin/Release/rana-socketd` shows `libpluto.so => .../vendor/pluto/src/libpluto.so`.

## Deferred (optional, defense-in-depth)
- Define `PLUTO_FFI_CALL_HOOK` / `PLUTO_HTTP_REQUEST_HOOK` to reject FFI/HTTP at
  the call-site. The runtime mask already makes those unreachable, so this is
  not required. Note: if enabled, the `extern bool` symbols must be defined
  inside `libpluto.so` (same archive) to avoid a link-order undefined-reference
  issue.
