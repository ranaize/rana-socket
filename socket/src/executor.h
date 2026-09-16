#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "command_generated.h"
#include "config.h"

namespace rana {

struct Result {
    Status status = Status_OK;
    std::string message;
};

// Outcome of running a Command. Either a normal Result, or a signal that the
// command is not mapped on this daemon and must be forwarded back to the client
// (socket 1) for execution there — in which case `forward_cmd` holds the raw
// inner Command bytes to relay.
struct RunOutcome {
    bool forward_to_client = false;
    std::vector<uint8_t> forward_cmd;
    Result result;
};

class Executor {
public:
    explicit Executor(const Config& cfg) : cfg_(cfg) {}

    // Exhaustive dispatch on the FlatBuffers union — the schema is the firewall.
    RunOutcome run(const Command* cmd);

    std::vector<uint8_t> build_response(uint64_t request_id, const Result& r) const;
    // Response that tells the client to forward `cmd` (base64 in the message).
    std::vector<uint8_t> build_forward_response(uint64_t request_id,
                                                const std::vector<uint8_t>& cmd) const;

private:
    // Looks up the TOML entry by daemon-side key and cross-checks the
    // configured `variant` against the union type actually received.
    const CommandEntry* lookup(const std::string& key, const std::string& variant,
                               Result& fail) const;

    Result run_mc_server(const McServerCmd* cmd);
    Result run_volume(const VolumeCmd* cmd);
    Result run_power(const PowerCmd* cmd);
    Result run_play_movie(const PlayMovieCmd* cmd);
    Result run_browser(const LaunchBrowserCmd* cmd);
    Result run_media(const MediaCmd* cmd);
    Result run_lookup(const LookupMachineCmd* cmd);
    Result run_speak(const SpeakCmd* cmd);
    Result run_light(const LightCmd* cmd);
    RunOutcome run_ask(const AskCmd* cmd);
    RunOutcome run_talk(const TalkCmd* cmd);

    // Shared LLM-hop path: run the ask script with `text` and re-dispatch the
    // routed action. Used by both AskCmd (text already in hand) and TalkCmd
    // (after on-server STT produces the transcript).
    RunOutcome run_ask_text(const std::string& text);

    // Re-dispatches an LLM-routed action (from the ask hop) as a fresh,
    // strongly-typed Command through run(). Forwards to the client when the
    // command is not mapped on this daemon.
    RunOutcome dispatch_inner(const std::string& action, const std::string& payload);

    const Config& cfg_;
};

// Reads framed requests until EOF, verifies each buffer, and answers with a
// framed Response. Never trusts the client-supplied payload shape.
void handle_connection(int fd, Executor& exec);

}  // namespace rana
