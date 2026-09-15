#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rana {

struct DaemonConfig {
    std::string socket_type = "unix";    // "unix" | "tcp"
    std::string socket_path;             // UDS path
    uint32_t socket_permissions = 0600;  // UDS mode
    std::string bind_address = "127.0.0.1";
    uint16_t port = 0;
    std::vector<std::string> allowed_ips;  // exact IPs or CIDR ranges
};

struct CommandEntry {
    std::string variant;      // FlatBuffers table name, e.g. "VolumeCmd"
    std::string type;         // script | exec | docker_compose | system
    std::string path;         // script path
    std::string binary;
    std::string workdir;
    std::string default_world;
    std::string default_url;
    std::string default_subnet;
    std::string suspend_binary;
    bool requires_confirm = false;
    uint32_t timeout_ms = 5000;
};

struct SttConfig {
    std::string script;                       // STT hop script (spawned by the daemon)
    std::string url = "http://localai:8080";  // LocalAI transcription endpoint
    std::string model = "talk";               // whisper model name (talk.yaml)
    uint32_t timeout_ms = 30000;
};

struct Config {
    DaemonConfig daemon;
    SttConfig stt;
    std::map<std::string, CommandEntry> commands;
    std::string path;   // path this config was loaded from (passed to hops)

    // Strict loader: rejects malformed files and unknown keys at startup.
    static bool load(const std::string& path, Config& out, std::string& err);
};

}  // namespace rana
