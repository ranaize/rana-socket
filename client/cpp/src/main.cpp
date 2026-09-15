// rana-socket-client — CLI front-end. Parses the address + command, delegates
// command construction to commands.cpp, and hands the framed buffer to the
// transport in transport.cpp. The wire protocol and FlatBuffer layouts are the
// concern of those modules.

#include "commands.hpp"
#include "transport.hpp"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: rana-socket-client <addr> <cmd> [args...]\n"
            "addr:  /path/to/rana.sock | tcp:127.0.0.1:port\n"
            "cmds:  speak | light | volume | power | browser | movie | media | machine | mc-server | ask | talk\n"
            "try 'rana-socket-client <addr> <cmd> --help' for per-cmd args.\n");
        return 2;
    }

    const std::string addr = argv[1];
    const std::string cmd = argv[2];

    flatbuffers::FlatBufferBuilder fbb(512);
    // Monotonic request id; process-local sequence for log correlation.
    static uint64_t g_request_seq = 0;
    const uint64_t request_id = ++g_request_seq;

    std::string err;
    if (!build_command(cmd, argc, argv, fbb, request_id, err)) {
        if (!err.empty()) std::fprintf(stderr, "rana-voice-agent: %s\n", err.c_str());
        return 2;
    }

    return run_client(addr, fbb.GetBufferPointer(), fbb.GetSize());
}
