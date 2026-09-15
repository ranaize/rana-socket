// FlatBuffer command construction. See commands.hpp.

#include "commands.hpp"

#include <command_generated.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

void finish_envelope(flatbuffers::FlatBufferBuilder& fbb, uint64_t request_id,
                     rana::CommandPayload payload_type, flatbuffers::Offset<void> payload) {
    const auto off = rana::CreateCommand(fbb, request_id, payload_type, payload);
    fbb.Finish(off);
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
    if (cmd == "speak") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> speak <text>");
        const auto text = fbb.CreateString(argv[3]);
        finish_envelope(fbb, request_id, rana::CommandPayload_SpeakCmd,
                        rana::CreateSpeakCmd(fbb, text).Union());
        return true;
    }

    if (cmd == "ask") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> ask <text>");
        const auto text = fbb.CreateString(argv[3]);
        finish_envelope(fbb, request_id, rana::CommandPayload_AskCmd,
                        rana::CreateAskCmd(fbb, text).Union());
        return true;
    }

    if (cmd == "light") {
        if (argc < 5) return usage_err("usage: rana-socket-client <addr> light <room> <on|off|toggle>");
        rana::LightAction action;
        const std::string_view a = argv[4];
        if (a == "on") action = rana::LightAction_On;
        else if (a == "off") action = rana::LightAction_Off;
        else if (a == "toggle") action = rana::LightAction_Toggle;
        else return usage_err("light: action must be on|off|toggle");
        const auto room = fbb.CreateString(argv[3]);
        finish_envelope(fbb, request_id, rana::CommandPayload_LightCmd,
                        rana::CreateLightCmd(fbb, room, action).Union());
        return true;
    }

    if (cmd == "volume") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> volume <0-100> [mute]");
        char* end = nullptr;
        const long lvl = std::strtol(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || lvl < 0 || lvl > 100)
            return usage_err("volume: level must be 0-100");
        const bool mute = argc >= 5 && std::string_view(argv[4]) == "mute";
        finish_envelope(fbb, request_id, rana::CommandPayload_VolumeCmd,
                        rana::CreateVolumeCmd(fbb, static_cast<uint8_t>(lvl), mute).Union());
        return true;
    }

    if (cmd == "power") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> power <shutdown|standby|reboot> [--confirm]");
        rana::PowerAction action;
        const std::string_view a = argv[3];
        if (a == "shutdown") action = rana::PowerAction_Shutdown;
        else if (a == "standby") action = rana::PowerAction_Standby;
        else if (a == "reboot") action = rana::PowerAction_Reboot;
        else return usage_err("power: action must be shutdown|standby|reboot");
        const bool confirm = argc >= 5 && std::string_view(argv[4]) == "--confirm";
        finish_envelope(fbb, request_id, rana::CommandPayload_PowerCmd,
                        rana::CreatePowerCmd(fbb, action, confirm).Union());
        return true;
    }

    if (cmd == "browser") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> browser <url>");
        const auto url = fbb.CreateString(argv[3]);
        finish_envelope(fbb, request_id, rana::CommandPayload_LaunchBrowserCmd,
                        rana::CreateLaunchBrowserCmd(fbb, url).Union());
        return true;
    }

    if (cmd == "movie") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> movie <path> [start_seconds]");
        uint64_t start = 0;
        if (argc >= 5) {
            char* end = nullptr;
            start = std::strtoull(argv[4], &end, 10);
            if (end == argv[4]) start = 0;
        }
        const auto path = fbb.CreateString(argv[3]);
        finish_envelope(fbb, request_id, rana::CommandPayload_PlayMovieCmd,
                        rana::CreatePlayMovieCmd(fbb, path, start).Union());
        return true;
    }

    if (cmd == "media") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> media <play|pause|stop|playpause> [target]");
        rana::MediaAction action;
        const std::string_view a = argv[3];
        if (a == "play") action = rana::MediaAction_Play;
        else if (a == "pause") action = rana::MediaAction_Pause;
        else if (a == "playpause") action = rana::MediaAction_PlayPause;
        else if (a == "stop") action = rana::MediaAction_Stop;
        else return usage_err("media: action must be play|pause|stop|playpause");
        const auto target = argc >= 5 ? fbb.CreateString(argv[4]) : 0;
        finish_envelope(fbb, request_id, rana::CommandPayload_MediaCmd,
                        rana::CreateMediaCmd(fbb, action, target).Union());
        return true;
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
        const auto subnet = fbb.CreateString(argv[3]);
        finish_envelope(fbb, request_id, rana::CommandPayload_LookupMachineCmd,
                        rana::CreateLookupMachineCmd(fbb, subnet, timeout).Union());
        return true;
    }

    if (cmd == "mc-server") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> mc-server <start|stop|restart> [world]");
        rana::McAction action;
        const std::string_view a = argv[3];
        if (a == "start") action = rana::McAction_Start;
        else if (a == "stop") action = rana::McAction_Stop;
        else if (a == "restart") action = rana::McAction_Restart;
        else return usage_err("mc-server: action must be start|stop|restart");
        const auto world = argc >= 5 ? fbb.CreateString(argv[4]) : 0;
        finish_envelope(fbb, request_id, rana::CommandPayload_McServerCmd,
                        rana::CreateMcServerCmd(fbb, action, world).Union());
        return true;
    }

    if (cmd == "talk") {
        if (argc < 4) return usage_err("usage: rana-socket-client <addr> talk <audio-file> [encoding=wav|pcm16] [sample_rate] [channels]");
        // Read the raw audio file into the buffer and forward it as a TalkCmd;
        // the daemon decodes + transcribes on-server (rana-serializer).
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
        uint32_t sample_rate = 0;  // 0 => daemon/codec decides (used for pcm16)
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
        const auto audio = fbb.CreateVector(
            reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        const auto encoding = fbb.CreateString(enc);
        finish_envelope(fbb, request_id, rana::CommandPayload_TalkCmd,
                        rana::CreateTalkCmd(fbb, audio, encoding, sample_rate, channels).Union());
        return true;
    }

    std::fprintf(stderr, "rana-voice-agent: unknown command '%s'\n", cmd.c_str());
    return false;
}
