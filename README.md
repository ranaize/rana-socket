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

The client builder builds one typed FlatBuffer per sub-command; the daemon
dispatches the exact union variant the schema declares:

```sh
# home automation
bin/Release/rana-socket-client <addr> light <room> <on|off|toggle>
bin/Release/rana-socket-client <addr> volume <0-100> [mute]
bin/Release/rana-socket-client <addr> power <shutdown|standby|reboot> [--confirm]

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
daemon is the **only** caller of LocalAI: `init.lua` registers the LLM endpoint, and
`scripts/ask.pluto` queries it via `rana.http_post`, then builds the `rana::AskReply`
FlatBuffer in-process with `rana.build_ask_reply` (schema in `schema/ask_reply.fbs`).
**The daemon never parses JSON in C++** — it verifies and reads the buffer, then
re-dispatches internally (`reply`→`speak`, `open_browser`→`browser`,
`toggle_lights`→`lights`, `shutdown`→`power`). The routed script's status/message
becomes the reply.

To enable it, the daemon config needs a `[commands.ask]` entry pointing at the hop
script, e.g.:

```toml
[commands.ask]
variant = "AskCmd"
type = "script"
path = "/usr/local/bin/rana-ask.sh"
timeout_ms = 30000
```

The response is framed the same way on the wire (4-byte big-endian length
header) and the client prints the daemon's `Response` status/message:

```
request_id=1 status=OK message=ok
```

A non-`OK` status (e.g. `UNKNOWN_COMMAND`, `TYPE_MISMATCH`, `INVALID_PAYLOAD`,
`SAFETY_GATE_FAILED`, `FORBIDDEN`) means the frame parsed but the executor
rejected or could not run it, so the client exits non-zero.

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
