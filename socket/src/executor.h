#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "command_generated.h"
#include "config.h"
#include "luafunctions.h"

namespace rana {

// Generic dispatcher. The schema is no longer the firewall — the config
// allowlist ([commands.allowed]) + the Pluto sandbox are. run() only reads the
// single Command{key,action,target,params,data}, checks the key, and hands the
// rest to the script layer (LuaRuntime). No union switch, no per-command C++.
class Executor {
public:
    Executor(const Config& cfg, LuaRuntime& rt) : cfg_(cfg), rt_(rt) {}

    // Generic dispatch on the flattened Command envelope.
    RanaResult run(const Command* cmd);

    // Serialize a Response{request_id, status, message}. A forward is just the
    // status FORWARD_TO_CLIENT with the base64 inner Command in `message` — no
    // extra wrapper or path.
    std::vector<uint8_t> build_response(uint64_t request_id, const RanaResult& r) const;

private:
    const Config& cfg_;
    LuaRuntime& rt_;
};

// Reads framed requests until EOF, verifies each buffer, and answers with a
// framed Response. Never trusts the client-supplied payload shape.
void handle_connection(int fd, Executor& exec);

}  // namespace rana
