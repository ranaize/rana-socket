// FlatBuffer command construction. See commands.hpp.
//
// The daemon speaks ONE generic envelope: Command{key, action, target, params,
// data}. Each CLI verb maps onto that envelope (key == script registry name).

#include "commands.hpp"

#include <command_generated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ParamList = std::vector<std::pair<std::string, std::string>>;

// Build a generic Command envelope from its parts.
flatbuffers::Offset<rana::Command> make_command(
    flatbuffers::FlatBufferBuilder& fbb, uint64_t request_id,
    const std::string& key, const std::string& action, const std::string& target,
    const ParamList& params, const std::string& data) {
    const auto k = fbb.CreateString(key);
    const auto a = fbb.CreateString(action);
    const auto t = fbb.CreateString(target);

    std::vector<flatbuffers::Offset<rana::StrPair>> param_offs;
    param_offs.reserve(params.size());
    for (const auto& kv : params) {
        param_offs.push_back(
            rana::CreateStrPair(fbb, fbb.CreateString(kv.first), fbb.CreateString(kv.second)));
    }
    const auto params_off =
        param_offs.empty() ? 0 : fbb.CreateVector(param_offs);

    const auto data_off =
        data.empty() ? 0
                     : fbb.CreateVector(reinterpret_cast<const uint8_t*>(data.data()),
                                        static_cast<uint32_t>(data.size()));

    return rana::CreateCommand(fbb, request_id, k, a, t, params_off, data_off);
}

// Reports a missing-argument usage error and returns false.
bool usage_err(const char* usage) {
    std::fprintf(stderr, "%s\n", usage);
    return false;
}

}  // namespace

bool build_command(const std::string& cmd, int argc, char** argv,
                   flatbuffers::FlatBufferBuilder& fbb, uint64_t request_id,
                   std::string& /*err*/) {
    // Helper lambdas captured below per-verb.
    auto finish = [&](const std::string& key, const std::string& action,
                      const std::string& target, const ParamList& params,
                      const std::string& data = "") {
        const auto off = make_command(fbb, request_id, key, action, target, params, data);
        fbb.Finish(off);
        return true;
    };

    if (cmd == "speak") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> speak <text>");
        return finish("speak", "speak", argv[3], {});
    }

    if (cmd == "ask") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> ask <text>");
        return finish("ask", "", argv[3], {});
    }

    if (cmd == "light") {
        if (argc < 5) return usage_err("usage: rana-socket-client <addr> light <room> <on|off|toggle>");
        const std::string_view a = argv[4];
        if (a != "on" && a != "off" && a != "toggle")
            return usage_err("light: action must be on|off|toggle");
        return finish("lights", std::string(a), argv[3], {});
    }

    if (cmd == "volume") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> volume <0-100> [mute]");
        char* end = nullptr;
        const long lvl = std::strtol(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || lvl < 0 || lvl > 100)
            return usage_err("volume: level must be 0-100");
        const bool mute = argc >= 5 && std::string_view(argv[4]) == "mute";
        ParamList params;
        if (mute) params.emplace_back("mute", "true");
        return finish("volume", "set", std::to_string(lvl), params);
    }

    if (cmd == "power") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> power <shutdown|standby|reboot> [--confirm]");
        const std::string_view a = argv[3];
        if (a != "shutdown" && a != "standby" && a != "reboot")
            return usage_err("power: action must be shutdown|standby|reboot");
        const bool confirm = argc >= 5 && std::string_view(argv[4]) == "--confirm";
        ParamList params;
        if (confirm) params.emplace_back("confirm", "true");
        return finish("power", std::string(a), "", params);
    }

    if (cmd == "browser") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> browser <url>");
        return finish("browser", "open", argv[3], {});
    }

    if (cmd == "movie") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> movie <path> [start_seconds]");
        uint64_t start = 0;
        if (argc >= 5) {
            char* end = nullptr;
            start = std::strtoull(argv[4], &end, 10);
            if (end == argv[4]) start = 0;
        }
        ParamList params;
        if (start) params.emplace_back("start", std::to_string(start));
        return finish("play_movie", "play", argv[3], params);
    }

    if (cmd == "media") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> media <play|pause|stop|playpause> [target]");
        const std::string_view a = argv[3];
        if (a != "play" && a != "pause" && a != "playpause" && a != "stop")
            return usage_err("media: action must be play|pause|stop|playpause");
        const std::string target = argc >= 5 ? argv[4] : "";
        return finish("media", std::string(a), target, {});
    }

    if (cmd == "machine") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> machine <subnet> [timeout_ms]");
        uint32_t timeout = 3000;
        if (argc >= 5) {
            char* end = nullptr;
            const long t = std::strtol(argv[4], &end, 10);
            if (end != argv[4] && *end == '\0' && t >= 0)
                timeout = static_cast<uint32_t>(t);
        }
        ParamList params;
        params.emplace_back("timeout_ms", std::to_string(timeout));
        return finish("lookup_machine", "lookup", argv[3], params);
    }

    if (cmd == "mc-server") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> mc-server <start|stop|restart> [world]");
        const std::string_view a = argv[3];
        if (a != "start" && a != "stop" && a != "restart")
            return usage_err("mc-server: action must be start|stop|restart");
        const std::string world = argc >= 5 ? argv[4] : "";
        return finish("mc_server", std::string(a), world, {});
    }

    if (cmd == "talk") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> talk <audio-file> [encoding=wav|pcm16] [sample_rate] [channels]");
        std::FILE* fp = std::fopen(argv[3], "rb");
        if (!fp) { std::perror("talk: open"); return false; }
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (size <= 0) { std::fclose(fp); return usage_err("talk: empty audio file"); }
        std::string raw(static_cast<size_t>(size), '\0');
        if (std::fread(&raw[0], 1, raw.size(), fp) != raw.size()) {
            std::fclose(fp); std::perror("talk: read"); return false;
        }
        std::fclose(fp);

        const std::string_view enc = argc >= 5 ? argv[4] : "wav";
        uint32_t sample_rate = 0;
        uint8_t channels = 1;
        if (argc >= 6) {
            char* end = nullptr;
            const long sr = std::strtol(argv[5], &end, 10);
            if (end != argv[5] && *end == '\0' && sr >= 0) sample_rate = static_cast<uint32_t>(sr);
        }
        if (argc >= 7) {
            char* end = nullptr;
            const long ch = std::strtol(argv[6], &end, 10);
            if (end != argv[6] && *end == '\0' && ch > 0 && ch <= 255)
                channels = static_cast<uint8_t>(ch);
        }
        ParamList params;
        params.emplace_back("encoding", std::string(enc));
        params.emplace_back("sample_rate", std::to_string(sample_rate));
        params.emplace_back("channels", std::to_string(channels));
        return finish("talk", std::string(enc), "", params, raw);
    }

    std::fprintf(stderr, "rana-voice-agent: unknown command '%s'\n", cmd.c_str());
    return false;
}
