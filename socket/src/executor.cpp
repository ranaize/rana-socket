#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // execvpe
#endif

#include "executor.h"

#include "ask_reply_generated.h"
#include "audio.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>

#include "protocol.h"

namespace rana {
namespace {

struct SpawnResult {
    int exit_code = -1;
    bool timed_out = false;
    bool spawn_failed = false;
    std::string stderr_text;
    std::string stdout_text;
};

// The child inherits the daemon environment; extras override by key.
std::vector<std::string> merged_env(const std::vector<std::string>& extras) {
    std::map<std::string, std::string> kv;
    for (char** e = environ; e && *e; ++e) {
        const std::string s(*e);
        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace(s.substr(0, eq), s.substr(eq + 1));
    }
    for (const auto& e : extras) {
        const auto eq = e.find('=');
        if (eq == std::string::npos) continue;
        kv[e.substr(0, eq)] = e.substr(eq + 1);
    }
    std::vector<std::string> out;
    out.reserve(kv.size());
    for (const auto& [k, v] : kv) out.push_back(k + "=" + v);
    return out;
}

// argv[0] is the executable; no shell involved anywhere.
// When capture_stdout is true the child's stdout is collected into the result
// (used by the ask hop, which returns its routed AskReply FlatBuffer on
// stdout); otherwise
// stdout is inherited by the daemon (container log), preserving prior behavior.
SpawnResult spawn_process(const std::string& workdir,
                          const std::vector<std::string>& argv,
                          const std::vector<std::string>& extra_env,
                          uint32_t timeout_ms,
                          bool capture_stdout = false) {
    SpawnResult res;
    if (argv.empty()) {
        res.spawn_failed = true;
        return res;
    }

    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        res.spawn_failed = true;
        return res;
    }

    int out_pipe[2] = {-1, -1};
    if (capture_stdout) {
        if (pipe(out_pipe) != 0) {
            ::close(err_pipe[0]);
            ::close(err_pipe[1]);
            res.spawn_failed = true;
            return res;
        }
    }

    const std::vector<std::string> env_store = merged_env(extra_env);

    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (const auto& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
    c_argv.push_back(nullptr);

    std::vector<char*> c_env;
    c_env.reserve(env_store.size() + 1);
    for (const auto& e : env_store) c_env.push_back(const_cast<char*>(e.c_str()));
    c_env.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        if (capture_stdout) {
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
        }
        res.spawn_failed = true;
        return res;
    }
    if (pid == 0) {
        ::close(err_pipe[0]);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(err_pipe[1]);
        if (capture_stdout) {
            ::close(out_pipe[0]);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::close(out_pipe[1]);
        }
        if (!workdir.empty()) ::chdir(workdir.c_str());
        ::execvpe(argv[0].c_str(), c_argv.data(), c_env.data());
        ::_exit(127);
    }

    ::close(err_pipe[1]);
    ::fcntl(err_pipe[0], F_SETFL, ::fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);
    int out_fd = -1;
    if (capture_stdout) {
        ::close(out_pipe[1]);
        out_fd = out_pipe[0];
        ::fcntl(out_fd, F_SETFL, ::fcntl(out_fd, F_GETFL) | O_NONBLOCK);
    }

    auto drain = [&](int fd, std::string& dst) {
        if (fd < 0) return;
        char buf[4096];
        for (;;) {
            const ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r > 0) {
                dst.append(buf, static_cast<size_t>(r));
                continue;
            }
            break;  // EAGAIN, EOF, or error
        }
    };

    std::string err_out, out_out;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    for (;;) {
        drain(err_pipe[0], err_out);
        if (capture_stdout) drain(out_fd, out_out);
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) {
            if (errno == EINTR) continue;
            res.spawn_failed = true;
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            res.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drain(err_pipe[0], err_out);
    if (capture_stdout) drain(out_fd, out_out);
    ::close(err_pipe[0]);
    if (capture_stdout) ::close(out_fd);

    if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.exit_code = 128 + WTERMSIG(status);

    res.stderr_text = std::move(err_out);
    if (res.stderr_text.size() > 4096) res.stderr_text.resize(4096);
    if (capture_stdout) {
        res.stdout_text = std::move(out_out);
        if (res.stdout_text.size() > 16384) res.stdout_text.resize(16384);
    }
    return res;
}

Result to_result(const SpawnResult& s) {
    if (s.spawn_failed) return {Status_EXECUTION_FAILED, "spawn failed"};
    if (s.timed_out) return {Status_EXECUTION_FAILED, "timed out; process killed"};
    if (s.exit_code != 0) {
        std::string msg = "exit code " + std::to_string(s.exit_code);
        if (!s.stderr_text.empty()) msg += ": " + s.stderr_text;
        return {Status_EXECUTION_FAILED, msg};
    }
    return {Status_OK, "ok"};
}

// The ask hop's result arrives as a binary rana::AskReply FlatBuffer (produced
// by rana-ask.sh -> rana-serializer). The daemon never parses JSON: it verifies
// the buffer and reads the two string fields. Returns false on a malformed or
// unverifiable buffer.
static bool read_ask_reply(const std::string& buf, std::string& action, std::string& payload) {
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    if (!VerifyAskReplyBuffer(v)) return false;
    const rana::AskReply* r = rana::GetAskReply(buf.data());
    if (r == nullptr || r->action() == nullptr) return false;
    action = r->action()->str();
    payload = r->payload() ? r->payload()->str() : std::string();
    return true;
}

}  // namespace

Result Executor::run(const Command* cmd) {
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "empty payload"};

    switch (cmd->payload_type()) {
        case CommandPayload_McServerCmd:
            return run_mc_server(cmd->payload_as<McServerCmd>());

        case CommandPayload_VolumeCmd:
            return run_volume(cmd->payload_as<VolumeCmd>());

        case CommandPayload_PowerCmd:
            return run_power(cmd->payload_as<PowerCmd>());

        case CommandPayload_PlayMovieCmd:
            return run_play_movie(cmd->payload_as<PlayMovieCmd>());

        case CommandPayload_LaunchBrowserCmd:
            return run_browser(cmd->payload_as<LaunchBrowserCmd>());

        case CommandPayload_MediaCmd:
            return run_media(cmd->payload_as<MediaCmd>());

        case CommandPayload_LookupMachineCmd:
            return run_lookup(cmd->payload_as<LookupMachineCmd>());

        case CommandPayload_SpeakCmd:
            return run_speak(cmd->payload_as<SpeakCmd>());

        case CommandPayload_LightCmd:
            return run_light(cmd->payload_as<LightCmd>());

        case CommandPayload_AskCmd:
            return run_ask(cmd->payload_as<AskCmd>());

        case CommandPayload_TalkCmd:
            return run_talk(cmd->payload_as<TalkCmd>());

        case CommandPayload_NONE:
            return {Status_INVALID_PAYLOAD, "empty payload"};

        default:
            return {Status_UNKNOWN_COMMAND, "unsupported union variant"};
    }
}

std::vector<uint8_t> Executor::build_response(uint64_t request_id, const Result& r) const {
    flatbuffers::FlatBufferBuilder b(256);
    const auto msg = b.CreateString(r.message);
    const auto resp = CreateResponse(b, request_id, r.status, msg);
    b.Finish(resp);
    return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}

const CommandEntry* Executor::lookup(const std::string& key, const std::string& variant,
                                     Result& fail) const {
    auto it = cfg_.commands.find(key);
    if (it == cfg_.commands.end()) {
        fail = {Status_UNKNOWN_COMMAND, key + " not configured"};
        return nullptr;
    }
    if (it->second.variant != variant) {
        fail = {Status_TYPE_MISMATCH,
                "union variant does not match configured entry for " + key};
        return nullptr;
    }
    return &it->second;
}

Result Executor::run_mc_server(const McServerCmd* cmd) {
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "bad payload"};
    Result fail;
    const CommandEntry* entry = lookup("mc_server", "McServerCmd", fail);
    if (entry == nullptr) return fail;

    std::vector<std::string> argv = {"docker", "compose"};
    switch (cmd->action()) {
        case McAction_Start:   argv.push_back("up");      break;
        case McAction_Stop:    argv.push_back("down");    break;
        case McAction_Restart: argv.push_back("restart"); break;
        default: return {Status_INVALID_PAYLOAD, "unknown mc action"};
    }

    std::string world;
    if (cmd->world_name()) world = cmd->world_name()->str();
    else if (!entry->default_world.empty()) world = entry->default_world;

    std::vector<std::string> env;
    if (!world.empty()) env.push_back("WORLD=" + world);

    return to_result(spawn_process(entry->workdir, argv, env, entry->timeout_ms));
}

Result Executor::run_volume(const VolumeCmd* cmd) {
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "bad payload"};
    Result fail;
    const CommandEntry* entry = lookup("volume", "VolumeCmd", fail);
    if (entry == nullptr) return fail;

    const std::vector<std::string> env = {
        "LEVEL=" + std::to_string(cmd->level()),
        std::string("MUTE=") + (cmd->mute() ? "1" : "0"),
    };
    return to_result(spawn_process("", {entry->path}, env, entry->timeout_ms));
}

Result Executor::run_power(const PowerCmd* cmd) {
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "bad payload"};

    // Safety gate: hard-coded requirement, never deferred to config.
    if (!cmd->confirm()) return {Status_SAFETY_GATE_FAILED, "confirm flag not set"};

    Result fail;
    const CommandEntry* entry = lookup("power", "PowerCmd", fail);
    if (entry == nullptr) return fail;

    const std::string* binary = nullptr;
    std::vector<std::string> args;
    switch (cmd->action()) {
        case PowerAction_Shutdown: binary = &entry->binary;         args = {"poweroff"}; break;
        case PowerAction_Standby:  binary = &entry->suspend_binary; args = {};           break;
        case PowerAction_Reboot:   binary = &entry->binary;         args = {"reboot"};   break;
        default: return {Status_INVALID_PAYLOAD, "unknown power action"};
    }
    if (binary->empty()) return {Status_EXECUTION_FAILED, "action not configured"};

    // Root is reachable only through the scoped sudoers policy; never a shell.
    std::vector<std::string> argv;
    if (::geteuid() != 0) argv.push_back("/usr/bin/sudo");
    argv.push_back(*binary);
    argv.insert(argv.end(), args.begin(), args.end());

    return to_result(spawn_process("", argv, {}, entry->timeout_ms));
}

Result Executor::run_play_movie(const PlayMovieCmd* cmd) {
    if (cmd == nullptr || cmd->path() == nullptr) {
        return {Status_INVALID_PAYLOAD, "bad payload"};
    }
    Result fail;
    const CommandEntry* entry = lookup("play_movie", "PlayMovieCmd", fail);
    if (entry == nullptr) return fail;

    std::vector<std::string> argv = {entry->binary};
    if (cmd->start_seconds() > 0) argv.push_back("--start=" + std::to_string(cmd->start_seconds()));
    argv.push_back(cmd->path()->str());

    return to_result(spawn_process("", argv, {}, entry->timeout_ms));
}

Result Executor::run_browser(const LaunchBrowserCmd* cmd) {
    Result fail;
    const CommandEntry* entry = lookup("browser", "LaunchBrowserCmd", fail);
    if (entry == nullptr) return fail;

    const std::string url = (cmd != nullptr && cmd->url()) ? cmd->url()->str() : entry->default_url;
    if (url.empty()) {
        return {Status_INVALID_PAYLOAD, "no url supplied and no default configured"};
    }

    return to_result(spawn_process("", {entry->binary, url}, {}, entry->timeout_ms));
}

Result Executor::run_media(const MediaCmd* cmd) {
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "bad payload"};
    Result fail;
    const CommandEntry* entry = lookup("media", "MediaCmd", fail);
    if (entry == nullptr) return fail;

    const char* action = nullptr;
    switch (cmd->action()) {
        case MediaAction_Play:      action = "play";      break;
        case MediaAction_Pause:     action = "pause";     break;
        case MediaAction_PlayPause: action = "playpause"; break;
        case MediaAction_Stop:      action = "stop";      break;
        default: return {Status_INVALID_PAYLOAD, "unknown media action"};
    }

    const std::string target = cmd->target() ? cmd->target()->str() : "";
    const std::vector<std::string> env = {std::string("ACTION=") + action, "TARGET=" + target};
    return to_result(spawn_process("", {entry->path}, env, entry->timeout_ms));
}

Result Executor::run_lookup(const LookupMachineCmd* cmd) {
    Result fail;
    const CommandEntry* entry = lookup("lookup_machine", "LookupMachineCmd", fail);
    if (entry == nullptr) return fail;

    const std::string subnet =
        (cmd != nullptr && cmd->subnet()) ? cmd->subnet()->str() : entry->default_subnet;
    if (subnet.empty()) {
        return {Status_INVALID_PAYLOAD, "no subnet supplied and no default configured"};
    }

    const uint32_t scan_timeout = (cmd != nullptr) ? cmd->timeout_ms() : 0;
    const std::vector<std::string> env = {
        "SUBNET=" + subnet,
        "TIMEOUT_MS=" + std::to_string(scan_timeout),
    };
    return to_result(spawn_process("", {entry->path}, env, entry->timeout_ms));
}

Result Executor::run_speak(const SpeakCmd* cmd) {
    if (cmd == nullptr || cmd->text() == nullptr) {
        return {Status_INVALID_PAYLOAD, "bad payload"};
    }
    Result fail;
    const CommandEntry* entry = lookup("speak", "SpeakCmd", fail);
    if (entry == nullptr) return fail;

    // argv = {binary, text}; execvp-style spawn, never a shell.
    return to_result(spawn_process("", {entry->binary, cmd->text()->str()}, {}, entry->timeout_ms));
}

Result Executor::run_light(const LightCmd* cmd) {
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "bad payload"};
    Result fail;
    const CommandEntry* entry = lookup("lights", "LightCmd", fail);
    if (entry == nullptr) return fail;

    const char* action = nullptr;
    switch (cmd->action()) {
        case LightAction_Toggle: action = "toggle"; break;
        case LightAction_On:     action = "on";     break;
        case LightAction_Off:    action = "off";    break;
        default: return {Status_INVALID_PAYLOAD, "unknown light action"};
    }

    const std::string room = cmd->room() ? cmd->room()->str() : "";
    return to_result(spawn_process("", {entry->binary, action, room}, {}, entry->timeout_ms));
}

Result Executor::run_ask(const AskCmd* cmd) {
    // The daemon is the ONLY caller that may reach the LLM hop: it holds the
    // previously-unreachable ("asked") authority and never forwards it outward.
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "bad payload"};
    if (cmd->text() == nullptr) return {Status_INVALID_PAYLOAD, "no text"};
    return run_ask_text(cmd->text()->str());
}

Result Executor::run_ask_text(const std::string& text) {
    Result fail;
    const CommandEntry* entry = lookup("ask", "AskCmd", fail);
    if (entry == nullptr) return fail;

    // script-type config fills `path`; fall back to `binary` if unset.
    // The question rides as argv[1] — never via a shell. The config path is
    // forwarded so the ask hop can build its system prompt from [commands].
    const std::string& exe = entry->path.empty() ? entry->binary : entry->path;
    if (exe.empty()) return {Status_EXECUTION_FAILED, "ask not configured with a binary/path"};

    const std::vector<std::string> env = {"RANA_CONFIG=" + cfg_.path};
    const std::vector<std::string> argv = {exe, text};
    SpawnResult sr = spawn_process("", argv, env, entry->timeout_ms, /*capture_stdout=*/true);
    if (sr.spawn_failed) return {Status_EXECUTION_FAILED, "ask hop spawn failed"};
    if (sr.timed_out)    return {Status_EXECUTION_FAILED, "ask hop timed out"};
    if (sr.exit_code != 0) {
        std::string msg = "ask hop exit " + std::to_string(sr.exit_code);
        if (!sr.stderr_text.empty()) msg += ": " + sr.stderr_text;
        return {Status_EXECUTION_FAILED, msg};
    }

    std::string action, payload;
    if (!read_ask_reply(sr.stdout_text, action, payload)) {
        return {Status_EXECUTION_FAILED, "ask hop returned an invalid AskReply buffer"};
    }
    return dispatch_inner(action, payload);
}

Result Executor::run_talk(const TalkCmd* cmd) {
    // Capture → on-server STT → ask hop → re-dispatch. The untrusted client
    // sends only raw audio; decoding/transport is owned by rana-serializer.
    if (cmd == nullptr) return {Status_INVALID_PAYLOAD, "bad payload"};
    const uint8_t* p = cmd->audio() ? cmd->audio()->data() : nullptr;
    const uint32_t n = cmd->audio() ? cmd->audio()->size() : 0;
    if (p == nullptr || n == 0) return {Status_INVALID_PAYLOAD, "no audio"};

    const std::string encoding = cmd->encoding() ? cmd->encoding()->str() : std::string("pcm16");
    const uint32_t sample_rate = cmd->sample_rate();
    const uint8_t channels = cmd->channels();

    std::string wav;
    if (!rana::serializer::audio::decode_to_wav(std::string(reinterpret_cast<const char*>(p), n), encoding,
                                    sample_rate, channels, wav)) {
        return {Status_INVALID_PAYLOAD, "unsupported audio encoding: " + encoding};
    }

    // Hand the decoded WAV to the STT backend via a temp file (the daemon has
    // no in-process HTTP client; it spawns a hop script, like the ask path).
    char tmpl[] = "/tmp/rana-talk-XXXXXX.wav";
    const int fd = ::mkstemps(tmpl, 4);
    if (fd < 0) return {Status_EXECUTION_FAILED, "cannot create temp wav"};
    size_t off = 0;
    while (off < wav.size()) {
        const ssize_t w = ::write(fd, wav.data() + off, wav.size() - off);
        if (w < 0) { ::close(fd); return {Status_EXECUTION_FAILED, "temp wav write failed"}; }
        off += static_cast<size_t>(w);
    }
    ::close(fd);
    const std::string wav_path(tmpl);

    const std::string stt_script = cfg_.stt.script;
    if (stt_script.empty()) { ::unlink(wav_path.c_str()); return {Status_EXECUTION_FAILED, "stt not configured"}; }
    const std::vector<std::string> env = {
        "STT_URL=" + cfg_.stt.url,
        "STT_MODEL=" + cfg_.stt.model,
    };
    SpawnResult sr = spawn_process("", {stt_script, wav_path}, env, cfg_.stt.timeout_ms,
                                  /*capture_stdout=*/true);
    ::unlink(wav_path.c_str());
    if (sr.spawn_failed) return {Status_EXECUTION_FAILED, "stt spawn failed"};
    if (sr.timed_out)    return {Status_EXECUTION_FAILED, "stt timed out"};
    if (sr.exit_code != 0) {
        std::string msg = "stt exit " + std::to_string(sr.exit_code);
        if (!sr.stderr_text.empty()) msg += ": " + sr.stderr_text;
        return {Status_EXECUTION_FAILED, msg};
    }

    std::string transcript = sr.stdout_text;
    while (!transcript.empty() && std::isspace(static_cast<unsigned char>(transcript.back())))
        transcript.pop_back();
    return run_ask_text(transcript);
}

// Re-dispatch the LLM-routed action internally. The reply text from the hop is
// whatever LocalAI classified; we map it back onto a strongly-typed Command so
// the SAME executor path (and SAME config gate) handles it. No LLM output ever
// leaves the daemon.
Result Executor::dispatch_inner(const std::string& action, const std::string& payload) {
    flatbuffers::FlatBufferBuilder b(256);
    CommandPayload type = CommandPayload_NONE;
    flatbuffers::Offset<void> up;

    if (action == "reply") {
        up = CreateSpeakCmd(b, b.CreateString(payload)).Union();
        type = CommandPayload_SpeakCmd;
    } else if (action == "open_browser") {
        up = CreateLaunchBrowserCmd(b, b.CreateString(payload)).Union();
        type = CommandPayload_LaunchBrowserCmd;
    } else if (action == "search_web") {
        std::string q = payload;
        for (char& c : q) if (c == ' ') c = '+';
        up = CreateLaunchBrowserCmd(b, b.CreateString("https://duckduckgo.com/?q=" + q)).Union();
        type = CommandPayload_LaunchBrowserCmd;
    } else if (action == "toggle_lights") {
        up = CreateLightCmd(b, b.CreateString(payload), LightAction_Toggle).Union();
        type = CommandPayload_LightCmd;
    } else if (action == "shutdown") {
        up = CreatePowerCmd(b, PowerAction_Shutdown, /*confirm=*/true).Union();
        type = CommandPayload_PowerCmd;
    } else {
        return {Status_UNKNOWN_COMMAND, "ask returned unknown action: " + action};
    }

    const auto cmd_off = CreateCommand(b, /*request_id=*/0, type, up);
    b.Finish(cmd_off);
    const Command* inner = GetCommand(b.GetBufferPointer());
    return run(inner);
}

void handle_connection(int fd, Executor& exec) {
    std::string frame;
    while (read_frame(fd, frame)) {
        Result r;
        uint64_t request_id = 0;

        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
        if (VerifyCommandBuffer(v)) {
            const Command* cmd = GetCommand(frame.data());
            request_id = cmd->request_id();
            r = exec.run(cmd);
        } else {
            r = {Status_INVALID_PAYLOAD, "malformed flatbuffer"};
        }

        const auto resp = exec.build_response(request_id, r);
        if (!write_frame(fd, resp.data(), static_cast<uint32_t>(resp.size()))) break;
    }
}

}  // namespace rana
