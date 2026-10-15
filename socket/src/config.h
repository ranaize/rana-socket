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

// Resolve the directory holding init.pluto + the *.pluto scripts. Resolution order:
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

// One entry in a hop's ordered provider chain ([[llm.provider]] / [[stt.provider]]).
// The hop tries providers in `order`; a provider with no explicit order defaults
// to local-before-cloud (local = 100, huggingface = 200), then keeps file order.
// A provider answers when it returns 2xx (or a live 4xx/5xx — the upstream's own
// answer); only a TRANSPORT failure/timeout advances to the next entry.
struct ProviderConfig {
    // ACL tier. "local" => http, host must already be in the scripts/init.pluto
    // allow_endpoints list. "huggingface" => https, usable only from a
    // RANA_ENABLE_TLS build and only when [security].external_egress is true.
    // (Routing an external answer back into a script is a separate, inbound gate
    // — see ScriptFlag::ExternalEgress and scripts/lib/route.pluto.) `type` is
    // validated against the URL scheme so an ACL label can never contradict the
    // wire scheme (which would let a token-bearing call slip past the gates).
    std::string type;
    std::string url;                  // http:// (local) or https:// (huggingface)
    std::string model;                // request model name
    std::string schema = "openai";    // wire dialect; only "openai" supported now
    std::string api_key;              // bearer token; huggingface only (secret)
    uint32_t timeout_ms = 15000;      // bounds this provider's single attempt
    int order = 0;                    // 0 => unset (use the tier default)
    std::string name;                 // optional log label
};

struct LlmConfig {
    std::vector<ProviderConfig> providers;  // ordered ask-hop chain
};

struct SttConfig {
    std::vector<ProviderConfig> providers;  // ordered STT-hop chain
};

// [security] — fleet/incident policy. external_egress=false hard-disables ALL
// cloud fallback even when an endpoint is configured (the local endpoint
// allowlist in init.pluto is unaffected).
struct SecurityConfig {
    bool external_egress = true;
};

struct Config {
    DaemonConfig daemon;
    LlmConfig llm;
    SttConfig stt;
    SecurityConfig security;
    std::set<std::string> allowed;  // [commands.allowed]: enabled script keys
    std::string path;               // path this config was loaded from
    std::string workdir;            // daemon working directory (scripts resolved here)

    // Strict loader: rejects malformed files and unknown keys at startup.
    static bool load(const std::string& path, Config& out, std::string& err);
};

}  // namespace rana
