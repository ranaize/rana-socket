#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // execvpe
#endif

#include "executor.h"

#include "ask_reply_generated.h"
#include "audio.hpp"
#include "logger.h"

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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"

namespace rana {
namespace {

const char* payload_name(CommandPayload t) {
    switch (t) {
        case CommandPayload_McServerCmd:      return "McServerCmd";
        case CommandPayload_VolumeCmd:        return "VolumeCmd";
        case CommandPayload_PowerCmd:         return "PowerCmd";
        case CommandPayload_PlayMovieCmd:     return "PlayMovieCmd";
        case CommandPayload_LaunchBrowserCmd: return "LaunchBrowserCmd";
        case CommandPayload_MediaCmd:         return "MediaCmd";
        case CommandPayload_LookupMachineCmd: return "LookupMachineCmd";
        case CommandPayload_SpeakCmd:         return "SpeakCmd";
        case CommandPayload_LightCmd:         return "LightCmd";
        case CommandPayload_AskCmd:           return "AskCmd";
        case CommandPayload_TalkCmd:          return "TalkCmd";
        case CommandPayload_NONE:             return "NONE";
        default:                              return "?";
    }
}

// ─── Fuzzy pre-router ──────────────────────────────────────────────────────
// A spoken query is first matched here (lexically) against the configured
// command descriptions/keywords. Only if nothing confident matches do we fall
// back to the LLM. Danger commands (e.g. machine power) require an EXACT phrase
// match, so a partial sentence such as "shutdown the minecraft server" can never
// collapse onto the power command.

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Flattened, whitespace-joined, lowercased phrase — used for exact matching.
std::string norm_phrase(const std::string& s) {
    std::string out;
    for (const auto& t : tokenize(s)) {
        out += t;
        out += ' ';
    }
    if (!out.empty()) out.pop_back();
    return out;
}

// Function words that carry no routing intent. They appear in nearly every
// command description ("the", "set", "turn"…), so including them in the overlap
// score produces false matches against unrelated queries. Strip them from both
// the transcript and the command corpus before scoring.
const std::set<std::string>& stopwords() {
    static const std::set<std::string> s = {
        "the", "a", "an", "of", "to", "on", "in", "for", "with", "and", "or",
        "is", "are", "be", "am", "my", "your", "our", "their", "please", "can",
        "could", "would", "should", "will", "you", "i", "it", "this", "that",
        "these", "those", "what", "who", "how", "why", "when", "where", "do",
        "does", "did", "have", "has", "some", "any", "into", "from", "at", "by",
        "as", "so", "if", "then", "about", "me", "we", "they", "he", "she",
        "turn", "set", "make", "get", "got", "close", "just", "now",
    };
    return s;
}

std::vector<std::string> drop_stopwords(std::vector<std::string> toks) {
    toks.erase(std::remove_if(toks.begin(), toks.end(),
                   [](const std::string& t) { return stopwords().count(t) > 0; }),
               toks.end());
    return toks;
}

// Meaningful-token phrase used for exact (danger) matching — stopwords removed so
// "turn off" (keyword) doesn't falsely equal "turn off the lights" (query).
std::string norm_meaningful(const std::string& s) {
    std::string out;
    for (const auto& t : drop_stopwords(tokenize(s))) {
        out += t;
        out += ' ';
    }
    if (!out.empty()) out.pop_back();
    return out;
}

size_t overlap_count(const std::vector<std::string>& haystack,
                     const std::vector<std::string>& needles) {
    std::set<std::string> hs(haystack.begin(), haystack.end());
    size_t n = 0;
    for (const auto& t : needles) if (hs.count(t)) ++n;
    return n;
}

bool contains_word(const std::string& s, const char* w) {
    for (const auto& t : tokenize(s)) if (t == w) return true;
    return false;
}

// Config key -> the canonical action keyword the router dispatches on.
std::string canonical_action(const std::string& key) {
    if (key == "speak")     return "reply";
    if (key == "browser")   return "open_browser";
    if (key == "lights")    return "toggle_lights";
    if (key == "power")     return "shutdown";
    if (key == "mc_server") return "open_minecraft";
    if (key == "volume")    return "set_volume";
    if (key == "media")     return "media_control";
    return key;
}

// Returns the [commands.<key>] that best matches `transcript`, or "" if none.
// `speak`/`ask` are intentionally skipped: they need the LLM (answer text or
// the hop itself) and must not be short-circuited by lexical matching.
std::string fuzzy_best_command(const std::string& transcript,
                               const std::map<std::string, CommandEntry>& cmds) {
    const auto qt = drop_stopwords(tokenize(transcript));
    if (qt.empty()) return "";
    const std::string qnorm = norm_meaningful(transcript);

    // 1) Danger commands: only an exact phrase match qualifies.
    for (const auto& [key, e] : cmds) {
        if (key == "ask" || key == "speak" || !e.danger) continue;
        for (const auto& kw : e.keywords) {
            if (norm_meaningful(kw) == qnorm) return key;
        }
    }

    // 2) Non-danger commands: best lexical overlap on meaningful tokens.
    // Require at least one overlapping meaningful token, and roughly half the
    // query's meaningful tokens, so a single common word can't trigger a false
    // route. ("open minecraft server" -> 3 tokens, need 2; "open browser" -> 2,
    // need 1.)
    const size_t qn = qt.size();
    const size_t need = qn <= 2 ? 1 : (qn + 1) / 2;
    std::string best;
    size_t best_score = 0;
    for (const auto& [key, e] : cmds) {
        if (key == "ask" || key == "speak" || e.danger) continue;
        if (e.description.empty() && e.keywords.empty()) continue;

        std::vector<std::string> corpus = drop_stopwords(tokenize(e.description));
        for (const auto& kw : e.keywords) {
            auto kt = drop_stopwords(tokenize(kw));
            corpus.insert(corpus.end(), kt.begin(), kt.end());
        }
        const size_t ov = overlap_count(qt, corpus);
        if (ov >= need && ov > best_score) {
            best = key;
            best_score = ov;
        }
    }
    return best;
}

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

namespace {

// RFC4648 base64 (no newline). Embeds a forwarded Command inside a
// Response.message so the reply stays a single, always-parseable Response.
static const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | uint32_t(in[i + 2]);
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(kBase64[(n >> 6) & 63]);
        out.push_back(kBase64[n & 63]);
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t n = uint32_t(in[i]) << 16;
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(kBase64[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

}  // namespace

RunOutcome Executor::run(const Command* cmd) {
    if (cmd == nullptr) { RunOutcome o; o.result = {Status_INVALID_PAYLOAD, "empty payload"}; return o; }

    switch (cmd->payload_type()) {
        case CommandPayload_McServerCmd: { RunOutcome o; o.result = run_mc_server(cmd->payload_as<McServerCmd>()); return o; }
        case CommandPayload_VolumeCmd:   { RunOutcome o; o.result = run_volume(cmd->payload_as<VolumeCmd>()); return o; }
        case CommandPayload_PowerCmd:    { RunOutcome o; o.result = run_power(cmd->payload_as<PowerCmd>()); return o; }
        case CommandPayload_PlayMovieCmd:{ RunOutcome o; o.result = run_play_movie(cmd->payload_as<PlayMovieCmd>()); return o; }
        case CommandPayload_LaunchBrowserCmd: { RunOutcome o; o.result = run_browser(cmd->payload_as<LaunchBrowserCmd>()); return o; }
        case CommandPayload_MediaCmd:    { RunOutcome o; o.result = run_media(cmd->payload_as<MediaCmd>()); return o; }
        case CommandPayload_LookupMachineCmd: { RunOutcome o; o.result = run_lookup(cmd->payload_as<LookupMachineCmd>()); return o; }
        case CommandPayload_SpeakCmd:    { RunOutcome o; o.result = run_speak(cmd->payload_as<SpeakCmd>()); return o; }
        case CommandPayload_LightCmd:    { RunOutcome o; o.result = run_light(cmd->payload_as<LightCmd>()); return o; }
        case CommandPayload_AskCmd:      return run_ask(cmd->payload_as<AskCmd>());
        case CommandPayload_TalkCmd:     return run_talk(cmd->payload_as<TalkCmd>());
        case CommandPayload_NONE:        { RunOutcome o; o.result = {Status_INVALID_PAYLOAD, "empty payload"}; return o; }
        default:                         { RunOutcome o; o.result = {Status_UNKNOWN_COMMAND, "unsupported union variant"}; return o; }
    }
}

std::vector<uint8_t> Executor::build_response(uint64_t request_id, const Result& r) const {
    flatbuffers::FlatBufferBuilder b(256);
    const auto msg = b.CreateString(r.message);
    const auto resp = CreateResponse(b, request_id, r.status, msg);
    b.Finish(resp);
    return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}

std::vector<uint8_t> Executor::build_forward_response(uint64_t request_id,
                                                     const std::vector<uint8_t>& cmd) const {
    // Tells the client to run `cmd` on another socket. The inner Command is
    // base64-encoded into the message so the reply is still a single Response.
    flatbuffers::FlatBufferBuilder b(256);
    const auto msg = b.CreateString(base64_encode(cmd));
    const auto resp = CreateResponse(b, request_id, Status_FORWARD_TO_CLIENT, msg);
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

    // An explicit url may be empty (e.g. forwarded from a command with no args);
    // treat an empty url the same as "not supplied" and fall back to default_url.
    std::string url;
    if (cmd != nullptr && cmd->url()) url = cmd->url()->str();
    if (url.empty()) url = entry->default_url;
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

RunOutcome Executor::run_ask(const AskCmd* cmd) {
    // The daemon is the ONLY caller that may reach the LLM hop: it holds the
    // previously-unreachable ("asked") authority and never forwards it outward.
    if (cmd == nullptr) { RunOutcome o; o.result = {Status_INVALID_PAYLOAD, "bad payload"}; return o; }
    if (cmd->text() == nullptr) { RunOutcome o; o.result = {Status_INVALID_PAYLOAD, "no text"}; return o; }
    return run_ask_text(cmd->text()->str());
}

RunOutcome Executor::run_ask_text(const std::string& text) {
    Result fail;
    const CommandEntry* entry = lookup("ask", "AskCmd", fail);
    if (entry == nullptr) { RunOutcome o; o.result = fail; return o; }

    // script-type config fills `path`; fall back to `binary` if unset.
    // The question rides as argv[1] — never via a shell. The config path is
    // forwarded so the ask hop can build its system prompt from [commands].
    const std::string& exe = entry->path.empty() ? entry->binary : entry->path;
    if (exe.empty()) { RunOutcome o; o.result = {Status_EXECUTION_FAILED, "ask not configured with a binary/path"}; return o; }

    const std::vector<std::string> env = {"RANA_CONFIG=" + cfg_.path};
    const std::vector<std::string> argv = {exe, text};
    rana::log_msg("ask", "run_ask_text: question=%s", text.c_str());
    SpawnResult sr = spawn_process("", argv, env, entry->timeout_ms, /*capture_stdout=*/true);
    if (sr.spawn_failed) { RunOutcome o; o.result = {Status_EXECUTION_FAILED, "ask hop spawn failed"}; return o; }
    if (sr.timed_out)    { RunOutcome o; o.result = {Status_EXECUTION_FAILED, "ask hop timed out"}; return o; }
    if (sr.exit_code != 0) {
        std::string msg = "ask hop exit " + std::to_string(sr.exit_code);
        if (!sr.stderr_text.empty()) msg += ": " + sr.stderr_text;
        RunOutcome o; o.result = {Status_EXECUTION_FAILED, msg}; return o;
    }

    std::string action, payload;
    if (!read_ask_reply(sr.stdout_text, action, payload)) {
        RunOutcome o; o.result = {Status_EXECUTION_FAILED, "ask hop returned an invalid AskReply buffer"}; return o;
    }
    rana::log_msg("ask", "run_ask_text: LLM action=%s payload=%s", action.c_str(), payload.c_str());
    return dispatch_inner(action, payload);
}

RunOutcome Executor::run_talk(const TalkCmd* cmd) {
    // Capture → on-server STT → ask hop → re-dispatch. The untrusted client
    // sends only raw audio; decoding/transport is owned by rana-serializer.
    if (cmd == nullptr) return RunOutcome{false, {}, {Status_INVALID_PAYLOAD, "bad payload"}};
    const uint8_t* p = cmd->audio() ? cmd->audio()->data() : nullptr;
    const uint32_t n = cmd->audio() ? cmd->audio()->size() : 0;
    if (p == nullptr || n == 0) return RunOutcome{false, {}, {Status_INVALID_PAYLOAD, "no audio"}};

    const std::string encoding = cmd->encoding() ? cmd->encoding()->str() : std::string("pcm16");
    const uint32_t sample_rate = cmd->sample_rate();
    const uint8_t channels = cmd->channels();
    rana::log_msg("talk", "run_talk: %u audio bytes, encoding=%s rate=%u ch=%u", n,
                  encoding.c_str(), sample_rate, channels);

    std::string wav;
    if (!rana::serializer::audio::decode_to_wav(std::string(reinterpret_cast<const char*>(p), n), encoding,
                                    sample_rate, channels, wav)) {
        return RunOutcome{false, {}, {Status_INVALID_PAYLOAD, "unsupported audio encoding: " + encoding}};
    }

    // Hand the decoded WAV to the STT backend via a temp file (the daemon has
    // no in-process HTTP client; it spawns a hop script, like the ask path).
    char tmpl[] = "/tmp/rana-talk-XXXXXX.wav";
    const int fd = ::mkstemps(tmpl, 4);
    if (fd < 0) return RunOutcome{false, {}, {Status_EXECUTION_FAILED, "cannot create temp wav"}};
    size_t off = 0;
    while (off < wav.size()) {
        const ssize_t w = ::write(fd, wav.data() + off, wav.size() - off);
        if (w < 0) { ::close(fd); return RunOutcome{false, {}, {Status_EXECUTION_FAILED, "temp wav write failed"}}; }
        off += static_cast<size_t>(w);
    }
    ::close(fd);
    const std::string wav_path(tmpl);

    const std::string stt_script = cfg_.stt.script;
    if (stt_script.empty()) { ::unlink(wav_path.c_str()); return RunOutcome{false, {}, {Status_EXECUTION_FAILED, "stt not configured"}}; }
    const std::vector<std::string> env = {
        "STT_URL=" + cfg_.stt.url,
        "STT_MODEL=" + cfg_.stt.model,
    };
    rana::log_msg("talk", "run_talk: running STT via %s (url=%s model=%s)", stt_script.c_str(),
                  cfg_.stt.url.c_str(), cfg_.stt.model.c_str());
    SpawnResult sr = spawn_process("", {stt_script, wav_path}, env, cfg_.stt.timeout_ms,
                                  /*capture_stdout=*/true);
    ::unlink(wav_path.c_str());
    if (sr.spawn_failed) return RunOutcome{false, {}, {Status_EXECUTION_FAILED, "stt spawn failed"}};
    if (sr.timed_out)    return RunOutcome{false, {}, {Status_EXECUTION_FAILED, "stt timed out"}};
    if (sr.exit_code != 0) {
        std::string msg = "stt exit " + std::to_string(sr.exit_code);
        if (!sr.stderr_text.empty()) msg += ": " + sr.stderr_text;
        return RunOutcome{false, {}, {Status_EXECUTION_FAILED, msg}};
    }

    std::string transcript = sr.stdout_text;
    while (!transcript.empty() && std::isspace(static_cast<unsigned char>(transcript.back())))
        transcript.pop_back();
    rana::log_msg("talk", "run_talk: transcript=%s", transcript.c_str());

    // Pre-LLM fuzzy route: if the transcript lexically matches a configured
    // command, dispatch it directly (skipping the LLM hop entirely). The LLM
    // remains the general fallback for everything else.
    if (const std::string key = fuzzy_best_command(transcript, cfg_.commands); !key.empty()) {
        rana::log_msg("fuzzy", "transcript routed to [commands.%s] before LLM", key.c_str());
        // For mc_server the transcript carries the start/stop/restart intent;
        // other commands have no args, so an empty payload uses their TOML
        // defaults (default_world / default_url / Toggle / ...).
        const std::string payload = (key == "mc_server") ? transcript : std::string();
        return dispatch_inner(canonical_action(key), payload);
    }
    return run_ask_text(transcript);
}

// Re-dispatch the LLM-routed action internally. The reply text from the hop is
// whatever LocalAI classified; we map it back onto a strongly-typed Command so
// the SAME executor path (and SAME config gate) handles it. No LLM output ever
// leaves the daemon.
RunOutcome Executor::dispatch_inner(const std::string& action, const std::string& payload) {
    // The LLM may emit either the prompted keyword ("open_browser") or the
    // FlatBuffer variant name ("LaunchBrowserCmd"); normalize to the keyword the
    // router below expects so a model that echoes the variant name still routes.
    std::string a = action;
    if (a == "LaunchBrowserCmd" || a == "browser") a = "open_browser";
    else if (a == "SpeakCmd")                       a = "reply";
    else if (a == "PowerCmd")                       a = "shutdown";
    else if (a == "LightCmd")                       a = "toggle_lights";
    else if (a == "McServerCmd" || a == "mc_server")        a = "open_minecraft";
    else if (a == "VolumeCmd"  || a == "volume")            a = "set_volume";
    else if (a == "MediaCmd"   || a == "media")             a = "media_control";

    flatbuffers::FlatBufferBuilder b(256);
    CommandPayload type = CommandPayload_NONE;
    flatbuffers::Offset<void> up;
    // [commands.<key>] used to test whether THIS daemon owns the command.
    std::string cfg_key;

    if (a == "reply") {
        up = CreateSpeakCmd(b, b.CreateString(payload)).Union();
        type = CommandPayload_SpeakCmd;
        cfg_key = "speak";
    } else if (a == "open_browser") {
        up = CreateLaunchBrowserCmd(b, b.CreateString(payload)).Union();
        type = CommandPayload_LaunchBrowserCmd;
        cfg_key = "browser";
    } else if (a == "search_web") {
        std::string q = payload;
        for (char& c : q) if (c == ' ') c = '+';
        up = CreateLaunchBrowserCmd(b, b.CreateString("https://duckduckgo.com/?q=" + q)).Union();
        type = CommandPayload_LaunchBrowserCmd;
        cfg_key = "browser";
    } else if (a == "toggle_lights") {
        up = CreateLightCmd(b, b.CreateString(payload), LightAction_Toggle).Union();
        type = CommandPayload_LightCmd;
        cfg_key = "lights";
    } else if (a == "shutdown") {
        up = CreatePowerCmd(b, PowerAction_Shutdown, /*confirm=*/true).Union();
        type = CommandPayload_PowerCmd;
        cfg_key = "power";
    } else if (a == "open_minecraft") {
        // Action is inferred from wording (start/stop/restart). The world comes
        // from the TOML default_world; the transcript/payload only steers action.
        McAction act = McAction_Start;
        if (contains_word(payload, "restart"))      act = McAction_Restart;
        else if (contains_word(payload, "stop") || contains_word(payload, "halt") ||
                 contains_word(payload, "shutdown"))
            act = McAction_Stop;
        up = CreateMcServerCmd(b, act, 0).Union();
        type = CommandPayload_McServerCmd;
        cfg_key = "mc_server";
    } else if (a == "set_volume") {
        up = CreateVolumeCmd(b, /*level=*/50, /*mute=*/false).Union();
        type = CommandPayload_VolumeCmd;
        cfg_key = "volume";
    } else if (a == "media_control") {
        up = CreateMediaCmd(b, MediaAction_Play, b.CreateString("")).Union();
        type = CommandPayload_MediaCmd;
        cfg_key = "media";
    } else {
        RunOutcome o; o.result = {Status_UNKNOWN_COMMAND, "ask returned unknown action: " + action};
        return o;
    }

    // Serialize the chosen Command so it can be forwarded to the client or run
    // locally through the normal dispatch path.
    const auto cmd = CreateCommand(b, 0, type, up);
    FinishCommandBuffer(b, cmd);
    std::vector<uint8_t> inner(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());

    // Variant name is derived from the command key (matches the toml->schema
    // mapping in config.cpp) so it is never hardcoded in two places.
    std::string variant = derive_variant(cfg_key);
    Result fail;
    const CommandEntry* entry = lookup(cfg_key, variant, fail);
    rana::log_msg("dispatch", "action=%s cfg_key=%s variant=%s -> %s", a.c_str(),
                  cfg_key.c_str(), variant.c_str(),
                  entry ? "execute-locally" : "forward-to-client");
    if (entry == nullptr || entry->forward_only) {
        RunOutcome fwd;
        fwd.forward_to_client = true;
        fwd.forward_cmd = std::move(inner);
        return fwd;
    }
    const Command* inner_cmd = GetCommand(inner.data());
    RunOutcome o;
    o.result = run(inner_cmd).result;
    return o;
}

void handle_connection(int fd, Executor& exec) {
    std::string frame;
    while (read_frame(fd, frame)) {
        RunOutcome outcome;
        uint64_t request_id = 0;

        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
        if (VerifyCommandBuffer(v)) {
            const Command* cmd = GetCommand(frame.data());
            request_id = cmd->request_id();
            rana::log_msg("conn", "recv request_id=%llu payload_type=%s(%d)",
                          static_cast<unsigned long long>(request_id),
                          payload_name(cmd->payload_type()),
                          static_cast<int>(cmd->payload_type()));
            outcome = exec.run(cmd);
        } else {
            rana::log_msg("conn", "recv malformed flatbuffer (%zu bytes)", frame.size());
            outcome.result = {Status_INVALID_PAYLOAD, "malformed flatbuffer"};
        }

        std::vector<uint8_t> resp;
        if (outcome.forward_to_client) {
            resp = exec.build_forward_response(request_id, outcome.forward_cmd);
            rana::log_msg("conn", "send request_id=%llu forward_to_client (status=%d, %zu bytes)",
                          static_cast<unsigned long long>(request_id),
                          static_cast<int>(outcome.result.status), resp.size());
        } else {
            resp = exec.build_response(request_id, outcome.result);
            rana::log_msg("conn", "send request_id=%llu status=%d msg=%s (%zu bytes)",
                          static_cast<unsigned long long>(request_id),
                          static_cast<int>(outcome.result.status),
                          outcome.result.message.c_str(), resp.size());
        }
        if (!write_frame(fd, resp.data(), static_cast<uint32_t>(resp.size()))) break;
    }
}

}  // namespace rana
