# Pluto

Pluto (<https://github.com/PlutoLang/Pluto>) is the language **every** file under
`scripts/` is written in. It is a strict superset of Lua 5.5 — every valid Lua
program is valid Pluto — with a set of ergonomic extensions. In this repo the
extended syntax is **always on** (the daemon does not define
`PLUTO_COMPATIBLE_MODE`), so both `.pluto` and `.lua` parse identically; the
`.pluto` extension is a repo convention plus what the loaders scan for.

## Version

Pin **`0.13.1`** — the same version the daemon embeds and CI downloads
(`https://github.com/PlutoLang/Pluto/releases/download/0.13.1/Linux.X64.zip`).
The archive contains `pluto` (interpreter), `plutoc` (compiler), `libpluto.so`,
and `libplutostatic.a`. Run it with `pluto`.

Extended syntax is always available; the daemon additionally defines
`PLUTO_ILP_ENABLE` (infinite-loop protection) and `PLUTO_ETL_ENABLE` (a
per-state wall-clock deadline). Those are *runtime guards*, not syntax gates.

## How this repo runs Pluto

```sh
pluto tests/check_scripts.lua   # parses + executes init.pluto and every script
pluto tests/test_lib.lua        # loads lib/*.pluto with a stubbed rana bridge
```

The daemon`'s embedded runtime opens only a subset of the stdlib and strips
`io`/`os`/`package`/`debug`/`ffi`/`socket`/`http`/`crypto`/`wasm`. A construct
that works under the standalone interpreter may still be unavailable in-script
if it needs one of those; stick to what `lib/*.pluto` already uses.

## Syntax the repo relies on

All of the following is validated against Pluto `0.13.1`.

```pluto
-- compound assignment
n += 1; s ..= "!"; flags |= MASK

-- nil-coalescing (nil only; `or` is falsy-based and also catches false)
local name = opts.name ?? "default"
opts ??= {}                      -- assign only when currently nil

-- ternary
local label = n > 0 ? "pos" : "neg"

-- safe navigation: nil instead of an index error at any link
local city = user?.address?.city

-- string interpolation (expression inside { }; # is the length op)
rana.log("info", $"provider {i} ({p.type}) unreachable, trying next")

-- default arguments
local function clip(s, n = 2000) ... end

-- lambdas: expression and block forms
local double = |x| -> x * 2
local shout  = |x| -> do return x .. "!" end

-- `continue` in a numeric/generic loop
for i = 1, 5 do
    if i % 2 == 0 then continue end
    work(i)
end

-- iterate a TABLE directly: value-only, or index+value
for v in arr do ... end
for idx, val in arr do ... end

-- `in`: substring test for a string, value search for a table
if "action is" in text then ... end
if action in { "start", "stop", "restart" } then ... end

-- switch, statement form (`:` bodies) and expression form (needs `do`)
switch level do
case "reply":
    return respond(...)
case "log":
    return nil
end

local payload = switch key do
    case "browser" -> url ?? ""
    default        -> text
end

-- `!` and `!=` are the short not / not-equal
if !ok or a != b then ... end
```

## Gotchas

- **`switch` requires `do`** in both forms: `switch x do ... end`. Omitting `do`
  is a syntax error.
- **`default` is a keyword.** You cannot name a variable, field, or parameter
  `default`. (The repo renamed `verbs.infer`'s parameter to `fallback`.)
- `??` is **nil-only**; `or` is falsy-based. Swap `or` for `??` only where the
  value can never legitimately be `false` or `0`.
- `= nil` as a default argument is a no-op (nil already triggers the default) and
  emits a `bad-practice` warning — omit it.
- `continue` is **not allowed inside a `switch`** body.
- A `switch` statement whose case body falls through warns
  (`unannotated-fallthrough`); end each case with `return`/`break`/`continue` or
  annotate with `--@fallthrough`.
- `for x as i in t` is **not** valid; use `for i, x in t do`.
- Re-declaring `_` in nested loops warns (`var-shadow`); name the inner index
  `_i`.
- `table.freeze` is **not compiled into the daemon runtime** — use the
  `setmetatable` pattern in `scripts/lib/refuse.pluto` for immutability.
- The embedded runtime's `require` only resolves `lib.*` -> `scripts/lib/*.pluto`;
  there is no `package.path`. Don't reach for stdlib modules.

## Migrating a `.lua` script to `.pluto`

1. Rename the file to `.pluto` (the loader and `init.pluto` discovery look for
   `.pluto`; update any references, the `Dockerfile`, tests, and docs).
2. Update the header comment and any `lib/*.lua` references to `.pluto`.
3. Apply the features above **behavior-preservingly**. The high-value swaps:
   - `x and x.f or d` -> `x?.f ?? d`; `a or b` -> `a ?? b` when `a` is nil-only.
   - `if not x then` -> `if !x then`; `x ~= y` -> `x != y`.
   - `"a" .. b .. "c"` -> `$"a {b} c"`.
   - `if a == 1 or a == 2 ...` -> `if a in { 1, 2 } then`.
   - nested `if key == "x" then ... elseif ...` returning values -> `switch`.
   - `if p and p.f then` guards -> `p?.f`.
   - trailing default-arg dance (`n or 2000`) -> `n = 2000`.
4. Keep `ipairs` where iteration order is semantically required (fuzzy loops,
   first-match verb inference) — do not swap to `pairs`.
5. Rewrite any `verbs.infer(..., default)` argument (the name `default` is now a
   keyword; the repo renamed the parameter to `fallback`, so callers are
   unaffected positionally).
6. Run `pluto tests/check_scripts.lua` and `pluto tests/test_lib.lua`; fix every
   warning the interpreter prints.

## Running / pinning

```sh
# fetch the pinned interpreter (Linux x64)
curl -fsSL -o /tmp/pluto.zip \
  https://github.com/PlutoLang/Pluto/releases/download/0.13.1/Linux.X64.zip
unzip -q -o /tmp/pluto.zip -d /tmp/pluto
/tmp/pluto/pluto -v            # -> Pluto 0.13.1 (Lua 5.5.1)
```

If the daemon's embedded runtime version changes, bump this pin **and** the CI
workflow (`.github/workflows/ci.yml`) together so script checks track the
runtime.
