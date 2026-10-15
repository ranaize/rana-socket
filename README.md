# rana-socket

FlatBuffers-framed command daemon (`rana-socketd`) — the typed socket layer that
`rana-voice-agent` talks to instead of executing commands locally.

- Transport: **AF_UNIX** or **TCP** (chosen per config)
- Frames: 4-byte big-endian length header + FlatBuffer `Command` payload
- Config: a single `rana-socket.toml` (example configs live in the `rana-deploy/config/` repo), each with a `[daemon]` table

## Layout

```
bin/Release/rana-socketd   built daemon
rana-deploy/config/*.toml  per-world config (daemon + command entries) — lives in rana-deploy
socket/                    server, protocol framing, executor, config loading
client/                    installable `rana-socket-client` Python package
dinit/                     dinit service units
```

## Building

```sh
make                 # release
```

Produces `bin/Release/rana-socketd`.

## Python client

`client/py/` is the installable `rana-socket-client` package. Its FlatBuffers
binding (`rana_socket/command_generated.py`) is generated, **not committed**, so
produce it once before installing:

```sh
make                                  # daemon build also drops it into the package
# or, for Python-only work (needs flatc 25.12.19, matching the pyproject pin):
./client/py/gen_command_generated.sh

pip install ./client/py
```

## Running

```sh
bin/Release/rana-socketd ../rana-deploy/config/server.toml
```

It needs a `rana-socket.toml` config path as its sole argument. JSON-like stderr is
never printed on startup; anything the daemon logs goes to the framers. To run
as a service, install the matching unit under `dinit/` and use `dinitctl start`.

## Unix Socket

```toml
[daemon]
socket_type    = "unix"                  # default
socket_path    = "/run/user/1000/rana-local.sock"
socket_permissions = 0o600               # default 0600
```

Start it (or via the dinit unit `rana-socketd-notebook`):

```sh
dinitctl start rana-socketd-notebook
```

Test from another shell with the C++ CLI client. `make` already builds it as
`bin/Release/rana-socket-client`:

```sh
bin/Release/rana-socket-client /run/user/1000/rana-local.sock speak "hello"
```

## Socket Addresses

The CLI takes one address argument:

- `unix`: just give the socket path, e.g. `/run/user/1000/rana-local.sock`
- `tcp`: give `tcp:IPv4:port`, e.g. `tcp:127.0.0.1:9000`

IPv4 peers must appear in the daemon's `allowed_ips` allow-list or the daemon
closes them on accept.

## Commands

The client builder maps each CLI verb onto the generic envelope
`Command{key, action, target, params, data}`; the daemon dispatches on `key`
(validated against `[commands.allowed]`) and hands the rest to the script.

`power` is destructive and gated: only a request carrying the client's
`--confirm` (`params.confirm = "true"`) runs — anything else fails closed with
`SAFETY_GATE_FAILED`.

```sh
# home automation
bin/Release/rana-socket-client <addr> light <room> <on|off|toggle>
bin/Release/rana-socket-client <addr> volume <0-100> [mute]
bin/Release/rana-socket-client <addr> power <shutdown|reboot|suspend> [--confirm]

# media
bin/Release/rana-socket-client <addr> media <play|pause|stop|playpause> [target]
bin/Release/rana-socket-client <addr> browser <url>
bin/Release/rana-socket-client <addr> movie <path> [start_seconds]

# mc-server (Minecraft) / lookup / speak
bin/Release/rana-socket-client <addr> mc-server <start|stop|restart> [world]
bin/Release/rana-socket-client <addr> machine <subnet> [timeout_ms]
bin/Release/rana-socket-client <addr> speak "text"

# ask — the LLM hop (daemon-only LocalAI caller)
bin/Release/rana-socket-client <addr> ask "what time is it"
```

`ask` sends a `Command{key="ask", action="ask", target=text}` to the daemon. The
daemon is the **only** caller of LocalAI: `scripts/ask.pluto` walks the
`[llm.provider]` chain (via `rana.providers` / `rana.provider_post`) and queries
the first reachable entry. The reply is parsed in Lua (never in C++) and routed with
`rana.dispatch` / `rana.respond`: intents this daemon owns run locally
(`reply`→`speak`, `open`→`browser`, `toggle`→`lights`, `...`→`power`); intents
owned by the peer daemon are handed back as a `FORWARD_TO_CLIENT` response. The
routed script's status/message becomes the reply. `ask` is enabled like any
other script — by listing it in the registry:

```toml
[commands.allowed]
"ask" = true
```

The daemon **never parses JSON in C++** and no shell hop is involved: the LLM
call, JSON decode, and intent routing all live in `scripts/ask.pluto` and its
reviewed helpers under `scripts/lib/`.

### Cloud fallback (optional, Hugging Face)

Each hop is an ordered **provider chain** in TOML (`[[llm.provider]]` /
`[[stt.provider]]`). Providers are tried top to bottom and only a **transport
failure/timeout** of one advances to the next; a live 4xx/5xx is the upstream's
own answer and stops the chain. With no `order` key the default is local-first
(local = 100, huggingface = 200), so a local entry is always tried before a
Hugging Face one. The Hugging Face tier is deny-by-default at every layer:

- **Build:** opt in with the premake `--tls` flag (`premake5 gmake --tls`, or the
  build stage's `flags` when driving the build through predep). Without it there
  is no TLS code and every `https://` URL is rejected. Static OpenSSL is
  preferred automatically; force dynamic with `--tls-dynamic`.
- **Config:** add the `type = "huggingface"` entry (`https` url + `api_key`) and,
  if you want a kill switch, `[security].external_egress`. An `api_key` on a
  `local` provider is rejected at load (it would travel in cleartext), and a
  `type` that disagrees with the URL scheme is rejected too.
- **Routing (inbound):** an answer that came from a huggingface provider is
  stamped external and may only be *acted on* by a script that opts in with
  `rana.flags.EXTERNAL_EGRESS` — **off by default**, so a remote model can reach
  nothing until the operator opts scripts in (including `speak`, if replies
  should be spoken). A refused route speaks/logs via `scripts/lib/refuse.lua`;
  it is never dispatched and never relayed to the peer. A caller can still opt
  one request out of the cloud entirely with `{ cloud = false }`.

The destination and the bearer token are owned by the bridge — scripts call
`rana.providers(kind)` to read the ordered list and
`rana.provider_post("llm"|"stt", index, payload)`, and never see the URL they
dial or the secret. See `docs/security.md` for the full egress model.

The response is framed the same way on the wire (4-byte big-endian length
header) and the client prints the daemon's `Response` status/message:

```
request_id=1 status=OK message=ok
```

A non-`OK` status (e.g. `UNKNOWN_COMMAND`, `INVALID_PAYLOAD`,
`SAFETY_GATE_FAILED`, `FORBIDDEN`, `FORWARD_TO_CLIENT`) means the frame parsed
but the executor rejected, deferred, or could not run it, so the client exits
non-zero.

## Config Reference

Key `[daemon]` options (all optional, with defaults):

| key                  | default            | meaning                                |
|----------------------|--------------------|----------------------------------------|
| `socket_type`        | `"unix"`           | `"unix"` or `"tcp"`                   |
| `socket_path`        | *(empty)*          | UDS path (unix only)                  |
| `socket_permissions` | `0o600`            | UDS file mode                          |
| `bind_address`       | `"127.0.0.1"`      | TCP bind host                          |
| `port`               | `0`                | TCP port (0 = assign ephemeral)        |
| `allowed_ips`        | *(empty)*          | allow-list for TCP peers               |

Command entries live under `[commands.*]` and are dispatched by their union
type; see `rana-deploy/config/client.toml` for examples and
`schema/command.fbs` for the command vocabulary.

`[llm]` / `[stt]` hold an ordered provider chain. Each `[[llm.provider]]` /
`[[stt.provider]]` entry:

| key          | default      | meaning                                              |
|--------------|--------------|------------------------------------------------------|
| `type`       | *(from url)* | `"local"` (http) or `"huggingface"` (https); must match the scheme |
| `url`        | *(required)* | request endpoint                                     |
| `model`      | *(empty)*    | request model name                                   |
| `schema`     | `"openai"`   | wire dialect; only `"openai"` is supported           |
| `api_key`    | *(empty)*    | bearer token; **huggingface only** (rejected on `local`) |
| `timeout_ms` | `15000`      | bounds this provider's single attempt                |
| `order`      | *(tier)*     | sort key; omitted ⇒ local `100`, huggingface `200`   |
| `name`       | *(empty)*    | optional log label                                   |

`schema = "openai"` means `/v1/chat/completions` for `[llm]` and
`/v1/audio/transcriptions` for `[stt]`.

`[security]`:

| key               | default | meaning                                                |
|-------------------|---------|--------------------------------------------------------|
| `external_egress` | `true`  | `false` hard-disables every huggingface provider (kill switch) |
