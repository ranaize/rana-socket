# Plan: gated cloud (Hugging Face) fallback for the LLM/STT hops

**Status:** implemented (2026-10-14), then **revised** the same day to an ordered
provider chain (§0 supersedes the primary+fallback shape below). Pending a
TLS-enabled compile + HF smoke test on real hardware. Decisions locked in §11.
**Depends on:** the v0.0.1 → `main` handoff work (still uncommitted). This feature
lands *on top of* that branch, as separate commits, after the handoff is committed.
**Author:** review pass, 2026-10-14.

---

## TL;DR

When the primary LLM/STT endpoint (the local `tower`/LocalAI box) is
**unreachable**, the `ask`/`stt` hops may retry once against a
**Hugging Face** endpoint over HTTPS.

This is a **new outbound-egress capability**, so it is built as a stack of
independently-disableable layers, deny-by-default at each one:

| Layer | Knob | Default |
|---|---|---|
| L0 build | `--tls` premake flag → `RANA_ENABLE_TLS` | **off** — opt in; no TLS otherwise |
| L1 config | `[llm].fallback_url` / `[stt] fallback_url` present | **off** (absent = no cloud) |
| L1b policy | `[security] external_egress` master kill switch | `true` (kill switch) |
| L2 allowlist | cloud endpoint is **config-owned**, not script-addable | — |
| L3 script meta | `rana.flags.EXTERNAL_EGRESS` on the running script | **absent = denied** |
| L4 call site | the script decides per call (`allow_cloud`) | script-chosen |
| L5 secret | token injected by the bridge; never visible to Lua | — |
| L6 runtime | fallback only on transport failure/timeout; bounded; logged | — |

The HTTPS primitive is reachable **only** through a single new bridge path
(`rana.provider_post`, reaching a `type = "huggingface"` provider) — never
through the generic `rana.http_post`.

---

## 0. Revision (2026-10-14): ordered provider chains (supersedes primary+fallback)

The "primary + one fallback" special case is replaced by a **TOML-ordered provider
chain** per hop. There is no `[llm_order]` table: each provider carries an
optional `order`, and with no `order` anywhere the default is local-first — the
`[llm]` local (http) entry then the Hugging Face (https) entry, when enabled.

```toml
[[llm.provider]]
type       = "local"          # http ⇒ local tier (host must be in init.lua)
url        = "http://127.0.0.1:8080/v1/chat/completions"
model      = "command"
# order = 1                   # optional; omitted ⇒ 100 local / 200 huggingface

[[llm.provider]]
type       = "huggingface"    # https ⇒ gated cloud tier
url        = "https://api-inference.huggingface.co/models/<org>/<model>"
model      = "<org>/<model>"
api_key    = "hf_..."
# order = 2
```

- **`type`** (`"local"` | `"huggingface"`) is the ACL tier and is **validated
  against the URL scheme** at load (`local` ⇒ `http`, `huggingface` ⇒ `https`);
  a mismatch is rejected, so a label can never contradict the wire scheme.
- **`schema`** (`"openai"`, default; the only supported dialect) replaces
  `fallback_protocol`. HF's Inference Providers router speaks the same
  `/v1/chat/completions` + `/v1/audio/transcriptions` shape, so the `hf-*`
  dialects are gone (the field leaves room to re-add one).
- **`order`** (optional positive int). Providers sort by `order`; a provider with
  none gets a tier default (`local` = 100, `huggingface` = 200); ties keep file
  order. No `order` anywhere ⇒ local-then-HF.
- **Advance rule:** only a **transport failure/timeout** (status 0, including a
  refused cloud gate) advances to the next provider. A live 4xx/5xx is the
  upstream's own answer and stops the chain.
- **Bridge API:** `cloud_post`/`cloud_configured`/`get_cloud_*` and
  `get_llm_url/model`/`get_stt_url/model` are gone. Scripts use
  `rana.providers(kind)` → `{ {type, schema, model, name}, … }` (call order, no
  URL/token) and `rana.provider_post(kind, index, payload [, headers])` →
  `ok, body, status`. The bridge stays policy-free; the script owns the loop.
- **Security layers:** outbound is build (`--tls`) ∧ config
  (`[security].external_egress`) ∧ URL scheme; the old per-script outbound flag
  is gone. `api_key` on a `local` provider is rejected at load (never in
  cleartext). `EXTERNAL_EGRESS` is now an **inbound** opt-in — see §0a.
- **Config placement:** the client template drops `[llm]`/`[stt]` (it runs no
  ask/talk hop, so the block was dead); providers live in the server template.

### 0a. Revision (2026-10-14): `EXTERNAL_EGRESS` is inbound-only

The flag no longer gates *dialing* a huggingface provider (that is build + config
only, above). It now marks **which scripts an external answer may be routed
into** — the model's answer is the untrusted input, so *acting on it* is what
needs the opt-in:

- `lib/llm.lua` stamps `external = true` on the intent when the **answering**
  provider was `type = "huggingface"` (from the provider, never the reply text).
- `lib/route.lua` refuses to `dispatch`/`respond` an external intent unless the
  target script declared `rana.flags.EXTERNAL_EGRESS` (read via
  `rana.script_metas().external_egress`). A `reply` also needs `speak` itself to
  have opted in before it may be spoken. **Off by default.**
- `lib/refuse.lua` owns the refusal ladder: speak a reason (if `speak` opted in),
  else log and drop, else `SAFETY_GATE_FAILED`; never dispatches, never relays.
- Local (non-external) answers are unaffected. `ask`/`stt` therefore no longer
  declare the flag; `speak` must opt in if an HF reply should be spoken.
- The C bridge no longer tracks a per-script egress bit (`current_allow_egress_`
  / `EgressGuard` removed); `provider_unavailable_reason` drops `not_allowed`.
  It surfaces the flag through `rana.script_metas()` for `route.lua`.

Where the sections below describe `EXTERNAL_EGRESS` as gating the outbound dial,
**this section wins.**

---

The sections below describe the original primary+fallback design and are kept for
the threat-model rationale; where they conflict with this section, this section
wins.

---

## 1. Goal & scope

**Goal.** If the primary LLM/STT HTTP endpoint cannot be reached (connection
refused / DNS failure / timeout — *transport* failure, not a 4xx/5xx from a live
server), the hop makes one bounded attempt against a configured Hugging Face
endpoint and adapts its response.

**In scope**
- `scripts/lib/llm.lua` (`ask` hop) and `scripts/stt.pluto` (STT hop).
- A TLS-capable HTTP transport in the C bridge, behind a build flag.
- Per-script capability flag + per-call decision.
- Config keys, secret injection, docs, tests.

**Out of scope (explicitly)**
- Making `https` generally available to `rana.http_post`. (Deliberate: see §3.)
- Generic cloud egress for arbitrary scripts/hosts.
- A Hugging Face *proxy* / provider-selection UI; the operator sets one URL.
- Retrying on 5xx or on empty/unparseable replies (only transport failure —
  per the Q&A decision; see §14 open question).
- Voice-routed `power` cloud fallback — unrelated and stays refused.

---

## 2. Decisions locked (from the design Q&A)

1. **Transport:** implement TLS in the C bridge, gated behind a premake5 build
   flag that **defaults off** (`--tls` opts in). A no-TLS build stays
   byte-for-byte today's capability and cannot speak TLS.
   (Revised 2026-10-14: default flipped from on to off — deny-by-default.)
2. **Token:** new `api_key` in `[llm]`/`[stt]` config.
3. **Trigger:** fall back on **unreachable/timeout of the primary only**.
4. **Scope:** **LLM + STT** both get the fallback.
5. **Script metadata** may opt a script in (or out) of the capability.
6. Egress must be **operator-gated** ("only a safe portion") and **per-call
   controllable**.

---

## 3. Threat-model delta

The current model (docs/security.md §3) is: *no TLS at all; `https` is rejected
at the scheme check; generic `rana.http_post` talks plaintext HTTP to
allowlisted hosts only.* This plan adds a narrow, auditable exception. The
invariants we keep:

- **`https` is never reachable from generic `rana.http_post`.** The TLS path is
  wired into a *separate* binding (`rana.cloud_post`) whose destination comes
  from **config**, not from Lua. A script cannot point the TLS transport at an
  arbitrary host — it can only name a `kind` (`"llm"`/`"stt"`).
- **The token never enters the sandbox.** No `rana.get_*` returns `api_key`;
  the bridge injects `Authorization: Bearer …` itself and overrides any
  `Authorization` a script tries to set.
- **Deny-by-default capability.** A script must declare
  `rana.flags.EXTERNAL_EGRESS`; without it, `cloud_post` refuses even when the
  endpoint is configured and the script is allowed to run.
- **Build-time ablation.** Without `--tls` there is no OpenSSL linkage, no TLS
  code compiled, and `https` is rejected exactly as today. A user who never
  wants cloud egress ships a binary that *cannot* speak TLS.

Residual risk accepted (documented): the daemon now links OpenSSL when built
with TLS on, and will make authenticated requests to *one configured* host. This
is the surface we previously avoided (the old D3 decision); the layered gating
above is what makes it acceptable.

---

## 4. The layered model (centerpiece)

### L0 — Build layer (`RANA_ENABLE_TLS`)
Premake reads the `--tls` flag at generate time (**default off**; `--tls` opts
in). When on: `defines { "RANA_ENABLE_TLS" }` + link OpenSSL. When off (the
default): none of the TLS code compiles and `parse_url` rejects `https`, so the
default build has exactly today's capabilities.

### L1 — Config layer
The feature is **configured, not assumed**: it only exists if the operator sets
`fallback_url` for that hop. A `[security] external_egress = false` master switch
hard-disables it regardless of config (kill switch / fleet policy).

### L2 — Endpoint layer (egress **tiers**)
Two endpoint tiers, kept structurally separate:

- **Local tier** — `rana.allow_endpoints(...)` in `scripts/init.lua`, `http`
  only, reached via `rana.http_post`. **Unchanged** (LocalAI/Whisper today).
- **Cloud tier** — comes from **config** (`[llm]/[stt] fallback_url`), `https`
  only, reached **only** via `rana.cloud_post`. Scripts cannot add to it and
  cannot name its URL.

`rana.http_post` keeps matching only `endpoints_` (local); `cloud_post` matches
only `cloud_endpoints_`. Even if an operator also allowlisted the HF host in
`init.lua`, that would only be an `http` entry and still carry no secret.

### L3 — Per-script capability (`rana.flags.EXTERNAL_EGRESS`)
New `ScriptFlag::ExternalEgress = 2`, exposed to scripts. `run_entrypoint`
records, for the running script, whether egress is permitted
(`flag ∧ config ∧ security-switch ∧ TLS-built`). `cloud_post` enforces it. This
is the "scripts metadata may also specify it" requirement, and it is the layer
that keeps **unrelated commands** from ever using cloud egress.

### L4 — Per-call
The calling script decides whether *this* call may fall back:
`llm.ask(text, { cloud = true|false })` and `stt`'s `execute` reading
`cmd.params.cloud`. Default = on when L0–L3 permit; a caller can opt a specific
request out (e.g. not sending raw user audio/transcript for a private verb).
This is the "not desired for certain calls" requirement.

### L5 — Secret
`api_key` is read by C++ and injected as `Authorization: Bearer …` for cloud
calls only. It is never returned to Lua and never logged. Lua-supplied
`Authorization` on a cloud call is stripped/overridden.

### L5b — Trigger & budget
Fallback fires **only** on transport failure/timeout (http status `0` from
`http_do`). A live server returning 4xx/5xx is surfaced as today. The fallback
attempt gets its own bounded timeout (`fallback_timeout_ms`, default 15000) so
two sequential hops cannot blow the script's ETL budget; scripts that use the
fallback declare `LONG_IDLE`.

### L6 — Observability
One structured log line per fallback: hop kind, primary failure reason, whether
the fallback was attempted / permitted, outcome. **No token, and no raw user
text/audio** in logs by default.

---

## 5. Public API

### New bridge function
```
ok, body = rana.cloud_post(kind, payload [, headers])
```
- `kind`: `"llm"` | `"stt"` — selects `[llm]`/`[stt]` `fallback_url` + `api_key`.
- `payload`: a table (JSON-encoded) **or** a pre-encoded string (so STT can pass
  a `lib/multipart` body, or raw bytes for `hf-audio`).
- `headers`: optional table appended after the bridge's own headers
  (`Authorization` is stripped/overridden; `Content-Type` default:
  `application/json` for llm, `audio/wav` for raw stt).
- Returns `false, "<reason>"` when: not TLS-built, no `fallback_url`, security
  switch off, script lacks the flag, or transport failure. Reasons are stable
  strings (mirroring the `rana.spawn` reason style), e.g.
  `tls_not_built`, `not_configured`, `disabled`, `not_allowed`, `connect_failed`,
  `timeout`, `bad_args`.

### New flag constant
```
rana.flags.EXTERNAL_EGRESS   -- ScriptFlag::ExternalEgress = 2
```

### New getter (non-secret)
```
configured = rana.cloud_configured(kind)   -- bool: is this hop's fallback usable now?
```
Lets a script avoid a pointless attempt and log honestly. Exposes only a boolean
(no URL, no token).

### Unchanged
`rana.http_post`, `rana.allow_endpoints`, `rana.get_llm_url/model`,
`rana.get_stt_url/model`, `rana.get_system_prompt` — behavior identical.

---

## 5a. Where the Lua-side fallback lives

- `scripts/lib/llm.lua`: `M.ask(text, opts)` — after `rana.http_post` returns
  `ok == false` with `status == 0` (transport failure — NOT a live 4xx/5xx)
  **and** ops/flag permit, call `rana.cloud_post("llm", <hf-body>)`,
  then adapt the response (§8).
- `scripts/stt.pluto`: after a transport-failed primary `http_post`,
  `cloud_post("stt", …)`.

To make "unreachable" distinguishable from a live HTTP error, `rana.http_post`
now also returns the numeric status as a third value (`0` == transport failure);
existing `(ok, body)` callers are unaffected.

The C bridge stays dumb: it does not know about "primary vs fallback"; the
scripts own the two-step policy.

---

## 6. Script metadata & per-call

- `scripts/lib` consumers that egress to the cloud declare the capability:
  - `scripts/ask.pluto` → add `rana.flags.EXTERNAL_EGRESS`
  - `scripts/stt.pluto` → add `rana.flags.EXTERNAL_EGRESS`
  - (`talk.pluto` does not egress itself — the hops it dispatches govern.)
- Per call:
  - `llm.ask(text, opts)` — `opts.cloud` (default `true`), and a helper
    `llm.cloud_available()` = `rana.egress_enabled and ...`.
  - `stt`: `M.stt(wav, allow_cloud)`; `M.execute` maps
    `cmd.params.cloud == "false"` → `false`.
- `table_to_flags` gains `case ScriptFlag::ExternalEgress`.

---

## 7. Config schema

```toml
[security]
# Fleet/incident kill switch. false hard-disables ALL cloud egress even when a
# fallback_url is configured. Does not affect the local endpoint allowlist.
external_egress = true

[llm]
url   = "http://127.0.0.1:8080/v1/chat/completions"
model = "…"
# Cloud fallback (needs a RANA_ENABLE_TLS build). Only scripts that declare
# rana.flags.EXTERNAL_EGRESS may use it.
fallback_url      = "https://api-inference.huggingface.co/models/<org>/<model>"
api_key           = "hf_…"
fallback_protocol = "hf-text-generation"   # or "openai-chat" (default)
fallback_model    = "<org>/<model>"        # defaults to `model`
fallback_timeout_ms = 15000

[stt]
url        = "http://127.0.0.1:8080/v1/audio/transcriptions"
model      = "whisper-1"
timeout_ms = 30000
fallback_url      = "https://api-inference.huggingface.co/models/openai/whisper-large-v3"
api_key           = "hf_…"
fallback_protocol = "hf-audio"             # or "openai-audio" (default)
fallback_model    = "openai/whisper-large-v3"  # defaults to `model`
fallback_timeout_ms = 15000
```

Config-loader changes (`socket/src/config.{h,cpp}`):
- `allowed_llm_keys()`/`allowed_stt_keys()` += `fallback_url`,
  `fallback_model`, `api_key`, `fallback_protocol`, `fallback_timeout_ms`.
- Allow new top-level `[security]` (the top-level key check at config.cpp:124).
- `CloudConfig` struct shared by `LlmConfig`/`SttConfig`; a `SecurityConfig`
  with `external_egress`.
- **Do not** add fallback hosts to `scripts/init.lua` — the cloud tier is
  config-owned (L2).

`config/*.template.toml` gain commented-out examples + the `[security]` block.

---

## 8. Wire / protocol adaptation

The primary already speaks OpenAI-ish shapes. The fallback's dialect is
operator-declared via `fallback_protocol` because HF has two coexisting eras.

**LLM**
- `openai-chat` → body = same `{model, messages, temperature, max_tokens}`;
  parse `choices[1].message.content`.
- `hf-text-generation` (classic serverless) → body =
  `{ inputs = <system..user prompt>, parameters = { temperature, max_new_tokens, return_instructions=false } }`;
  parse `[1].generated_text`.
- `lib/llm.lua#parse_action` is unchanged; only the transport body + the
  "extract content" step change. Add array handling: HF returns a JSON **array**,
  so confirm `json_decode` yields a 1-based array and index it safely.

**STT**
- `openai-audio` → `lib/multipart` body (same as today), bearer token added.
- `hf-audio` → POST raw WAV bytes (`Content-Type: audio/wav`).
- Both parse `{"text": …}`, which `stt.pluto` already reads. Add a defensive
  `j[1]`/`generated_text` miss path.

**Timeouts.** `cloud_post` uses `[llm|stt] fallback_timeout_ms` (default 15000),
not the hop's full ETL budget, so two attempts stay inside the script deadline.

---

## 9. Exact code touchpoints

### C++ — `socket/src/luafunctions.h`
- `enum class ScriptFlag` += `ExternalEgress = 2`.
- `LuaRuntime`: add `cloud_endpoints_` (kind → `Endpoint`), `cloud_keys_`
  (kind → token), `cloud_timeout_ms_`, `current_allow_egress_` (+ accessor).
- Keep `Endpoint` as-is (`scheme/host/port`).

### C++ — `socket/src/luafunctions.cpp`
- `parse_url` (≈523): accept `https` (default port 443) **only** under
  `#ifdef RANA_ENABLE_TLS`; otherwise keep the existing reject message.
- `http_do` (≈563): factor the fd send/recv into a small transport that is
  either plaintext or TLS. TLS under `#ifdef RANA_ENABLE_TLS`:
  - `SSL_CTX` (lazy, process-wide; single-threaded daemon), `SSL_new`,
    SNI (`SSL_set_tlsext_host_name`), mandatory peer + hostname verification
    (`SSL_set1_host`), `TLS1_2` minimum, `SSL_get_verify_result() == X509_V_OK`.
  - Map `SO_RCVTIMEO` timeouts / `SSL_ERROR_WANT_READ` to transport failure;
    never throw (runs inside a `lua_CFunction`; use `rana_guard`).
  - `Host:` header default-port logic must use the scheme default (80/443).
- Endpoint matching: keep matching `endpoints_` for `http_do` (local). Add a
  `cloud_post` path that matches `cloud_endpoints_`.
- New binding `rana_cloud_post` + registration in `kRanaLib` (≈1178): validate
  `kind`, resolve endpoint/key/timeout, inject `Authorization`, strip a
  script-supplied one, call `http_do`, return `ok, body`.
- `luaopen_rana` (≈1210): expose `EXTERNAL_EGRESS`.
- `table_to_flags` (≈749): accept `ExternalEgress`.
- `run_entrypoint` (≈1457): set `current_allow_egress_` from
  `metas_[key].flags` ∧ config ∧ `[security]` ∧ TLS-built.
- `LuaRuntime::init` (≈1380): populate `cloud_endpoints_/cloud_keys_/timeouts`
  from `cfg_`.

### C++ — `socket/src/config.{h,cpp}`
- Structs: `LlmConfig`/`SttConfig` += `fallback_url`, `api_key`,
  `fallback_protocol`, `fallback_timeout_ms`; new `Security` struct.
- Key sets + top-level section allowlist (§6).

### Build
- `socket/premake5.lua`: register the `--tls` premake option (**default off**);
  when passed: define `RANA_ENABLE_TLS` and link OpenSSL — **static when the
  distro ships `libssl.a`/`libcrypto.a`, otherwise dynamic** (see §11.2). Force
  the dynamic path with `--tls-dynamic`.
- `premake5.lua`: shares the library-probe helper (`premake/helpers.lua`) with
  every subproject; future deps resolve system libs through it.
- `predep.toml`: pass `--tls` through the build stage's `flags` (predep feature
  pending; see `docs/predep-flags-spec.md`).

### Docker / install
- `Dockerfile` builder: install `libssl-dev` in the builder apt line (harmless
  when off) so a static OpenSSL is available. TLS is a `--tls` opt-in on the
  predep build stage, so the image default stays no-TLS.
- Runtime: add `libssl3` to the runtime apt line for the dynamic-link fallback;
  with a static build it is simply unused.
- `make socket`/`predep install` path: TLS stays a `--tls` build-stage opt-in; no
  artifact changes otherwise.

### Lua
- `scripts/lib/llm.lua`: `ask(text, opts)` two-step + protocol adapter;
  `cloud_available()`.
- `scripts/stt.pluto`: two-step + `allow_cloud` param.
- `scripts/ask.pluto`, `scripts/stt.pluto`: add `rana.flags.EXTERNAL_EGRESS`.
- `scripts/init.lua`: **no endpoint change**; add a comment pointing at the
  config-owned cloud tier + `[security]` switch.
- `scripts/lib/multipart.lua`: unchanged (reused for `openai-audio`).

### Docs
- `docs/security.md`: new "Outbound egress tiers" section — build flag, the
  `cloud_post`-only TLS reachability, token non-exposure, per-script flag, kill
  switch, and the explicit note that this supersedes the old "TLS not
  implemented" line.
- `README.md`: config reference rows + a "Cloud fallback" subsection.
- `config/*.template.toml`: commented examples + `[security]`.

### Tests
- `tests/test_lib.lua`: stub `rana.cloud_post` and assert
  - primary fail + flag + config → cloud called once;
  - primary fail + **no flag** → cloud **not** called;
  - primary success → cloud not called;
  - `opts.cloud=false` → cloud not called;
  - protocol adapters parse both `choices[].message.content` and
    `[{generated_text}]` / `{"text"}`.
- `tests/check_scripts.lua`: unchanged (already validates meta shape).
- Add a negative test that **no bridge binding** returns `api_key` (grep-style
  or a targeted assertion).

---

## 10. Implementation phases

1. **P1 — build flag + transport.** `--tls` plumbing, `#ifdef RANA_ENABLE_TLS`
   TLS transport in `http_do`, `parse_url` https. No new binding yet; prove the
   build toggles (TLS on/off) and that generic `http_post` still refuses https.
2. **P2 — config.** `[security]` + `[llm]/[stt]` fallback keys, structs, loader,
   templates. Loader tests.
3. **P3 — capability + binding.** `ScriptFlag::ExternalEgress`,
   `current_allow_egress_`, `rana.cloud_post`, `Authorization` injection,
   `run_entrypoint` wiring.
4. **P4 — scripts.** `lib/llm.lua` + `stt.pluto` two-step, protocol adapters,
   meta flags, per-call opts.
5. **P5 — docs + Docker** (+ tests in P3/P4).
6. **P6 — verification.** Compile both flag states where a toolchain exists;
   offline Lua tests; manual HF smoke test with a throwaway token.

Each phase is a separate commit (per the handoff plan's commit rule), and the
whole feature is one reviewable PR on top of the handoff branch.

---

## 11. Open questions / decisions to confirm

1. **Build default.** ✅ **Resolved: default off — `--tls` opts in.** The
   no-TLS build is exactly today's capability (deny-by-default).
2. **OpenSSL linkage.** ✅ **Resolved: static when the distro ships the archives,
   else dynamic.** Probe for `libssl.a`/`libcrypto.a` at premake time and pass
   their paths; fall back to `-lssl -lcrypto`. Packaging/redistribution is not a
   near-term concern, so a dynamic fallback is acceptable on arch/non-static
   distros.
3. **Kill-switch default.** ✅ **Resolved: `[security] external_egress` defaults
   `true`** (the configured `fallback_url` is the opt-in), with the switch acting
   as a fleet/incident override.
4. **Wire dialect.** ✅ **Resolved: one `schema = "openai"`** (default; the only
   supported value). HF's Inference Providers router is OpenAI-compatible, so the
   `hf-text-generation` / `hf-audio` dialects were dropped rather than carried as
   dead config; `schema` remains for a future second dialect.
5. **Provider order.** ✅ **Resolved: an ordered `[[llm.provider]]` /
   `[[stt.provider]]` chain with an optional per-provider `order`**, defaulting
   local-before-huggingface. Replaces both the fixed primary+fallback pair and
   the earlier `[llm_order]` idea. Per-provider `type` is the ACL tier and must
   match the URL scheme (see §0).
6. **Trigger breadth.** Locked to transport-failure-only. Should a live primary
   that returns 5xx also fall back? (Deferred; easy to add later as a config
   bool `fallback_on_5xx`.)
7. **Flag name.** `EXTERNAL_EGRESS` (generic, covers llm+stt) vs
   `CLOUD_LLM`/`CLOUD_STT`. Recommend generic.

---

## 12. Non-goals

- No change to the local endpoint allowlist or generic `rana.http_post`.
- No secret ever readable from Lua; no `api_key` in any getter or log.
- No new script-addressable hosts.
- No proxy sidecar required (that was the alternative design we rejected in
  favour of the build flag).
- No `power`/voice cloud path.
