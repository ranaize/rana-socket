#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rana {

// Derive the canonical script key for a [commands.allowed] entry. The return
// value is just the trimmed key name; kept for any legacy callers. The new
// registry is a flat set of enabled script keys — no variant/type/path.
std::string normalize_key(const std::string& key);

struct Config;  // forward declaration; full definition below

// Resolve the directory holding init.lua + the *.pluto scripts. Resolution order:
//   1. $RANA_SCRIPTS_DIR            explicit override (set by `make server`/`make client`)
//   2. daemon.scripts_dir from config (absolute, or relative to the config's dir)
//   3. autodiscover: walk up from the daemon binary's directory, then fixed
//      locations such as /opt/rana/scripts (the canonical install path).
// When scripts_dir is omitted from the config, autodiscovery is used by default.
std::string resolve_scripts_dir(const Config& cfg);

struct DaemonConfig {
    std::string socket_type = "unix";    // "unix" | "tcp"
    std::string socket_path;             // UDS path
    uint32_t socket_permissions = 0600;  // UDS mode
    std::string bind_address = "127.0.0.1";
    uint16_t port = 0;
    std::vector<std::string> allowed_ips;  // exact IPs or CIDR ranges
    std::string scripts_dir;               // optional; empty => autodiscover (see resolve_scripts_dir)
};

struct LlmConfig {
    std::string url = "http://localai:8080";  // LocalAI chat endpoint base
    std::string model = "command";            // command-routing model name
};

struct SttConfig {
    std::string url = "http://localai:8080";  // LocalAI transcription endpoint base
    std::string model = "talk";               // whisper model name (talk.yaml)
    uint32_t timeout_ms = 30000;
};

struct Config {
    DaemonConfig daemon;
    LlmConfig llm;
    SttConfig stt;
    std::set<std::string> allowed;  // [commands.allowed]: enabled script keys
    std::string path;               // path this config was loaded from
    std::string workdir;            // daemon working directory (scripts resolved here)

    // Strict loader: rejects malformed files and unknown keys at startup.
    static bool load(const std::string& path, Config& out, std::string& err);
};

}  // namespace rana
