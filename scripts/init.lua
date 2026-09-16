-- scripts/init.lua
-- Executed once at daemon startup, before any command scripts. Defines the
-- system interface: which binaries/endpoints scripts may reach, the constants
-- they can read, and the filesystem sandbox. This is the single source of truth
-- for the security boundary — the scripts never touch os/io/package/debug.

local rana = require("rana")

-- System binary allowlist: name -> { path, vararg? , args? }. Scripts call
-- rana.spawn("espeak", text); the C side validates the name here.
--   vararg = true      -> script passes its own string/number args verbatim
--                         (freeform text tools only: espeak, xdg-open, ...).
--   args = { ... }     -> a HARDCODED argv (path + the literal list); the
--                         script calls rana.spawn(name) with NO arguments.
--   (both absent)      -> path with zero args.
-- rana-ask / rana-stt are NOT here — they are in-process Pluto scripts
-- (ask.pluto / stt.pluto) invoked directly by the executor.
rana.allow_binaries({
	espeak = { path = "/usr/bin/espeak-ng", vararg = true },
	xdg_open = { path = "/usr/bin/xdg-open", vararg = true },
	lights = { path = "/usr/bin/rana-lights", vararg = true },
	media = { path = "/usr/bin/rana-media", vararg = true },
	-- Privileged verbs: hardcoded argv, so no script ever passes arguments beyond
	-- the fixed boundary. The daemon user is in wheel with NOPASSWD sudo.
	shutdown = { path = "/usr/bin/sudo", args = { "shutdown" } },
	reboot = { path = "/usr/bin/sudo", args = { "reboot" } },
	suspend = { path = "/usr/bin/sudo", args = { "suspend" } },
	-- The only docker surface: the minecraft server container. No generic bare
	-- docker -- any other container operation requires a deliberate new entry.
	minecraft_status = { path = "/usr/bin/docker", args = { "container", "inspect", "mc-server" } },
	minecraft_start = { path = "/usr/bin/docker", args = { "container", "start", "mc-server" } },
	minecraft_stop = { path = "/usr/bin/docker", args = { "container", "stop", "mc-server" } },
	minecraft_restart = { path = "/usr/bin/docker", args = { "container", "restart", "mc-server" } },
	movie = { path = "/usr/bin/rana-play", vararg = true },
	lookup = { path = "/usr/bin/rana-lookup", vararg = true },
})

-- Whitelisted network endpoints (checked in luafunctions.cpp before any
-- rana.http_* call leaves the daemon). The daemon runs NATIVELY on the host and
-- the configs point at the host-published LocalAI port (127.0.0.1:8080); the
-- "localai" hostname is kept for the in-container role. Port 0 = any port.
rana.allow_endpoints({
	{ scheme = "http", host = "127.0.0.1", port = 8080 },
	{ scheme = "http", host = "localai", port = 8080 },
})

-- Global constants accessible to all scripts.
rana.set_constants({
	config_path = rana.get_config_path(),
	socket_path = rana.get_socket_path(),
	llm_url = rana.get_llm_url(),
	llm_model = rana.get_llm_model(),
	stt_url = rana.get_stt_url(),
	stt_model = rana.get_stt_model(),
})

-- Filesystem sandbox: scripts may read script sources and write only to their own
-- per-command scratch directory. Enforced by the C bridge.
rana.set_fs_whitelist({
	rana.get_workdir() .. "/scripts/",
	rana.get_workdir() .. "/scripts/*/scratch/",
})

-- NOTE: no command/variant registration here. The registry is [commands.allowed]
-- in TOML plus the scripts/ scan; each script self-describes via its meta table.
