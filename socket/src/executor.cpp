#include "executor.h"

#include "logger.h"
#include "protocol.h"

#include <map>
#include <string>
#include <vector>

namespace rana {
namespace {

}  // namespace

RanaResult Executor::run(const Command* cmd) {
    if (cmd == nullptr) { return {Status_INVALID_PAYLOAD, "empty payload"}; }

    const std::string key = cmd->key() ? cmd->key()->str() : "";
    if (key.empty()) { return {Status_INVALID_PAYLOAD, "missing key"}; }

    const std::string action = cmd->action() ? cmd->action()->str() : "";
    const std::string target = cmd->target() ? cmd->target()->str() : "";

    std::map<std::string, std::string> params;
    if (const auto* ps = cmd->params()) {
        for (const StrPair* sp : *ps) {
            if (sp->k() && sp->v()) params[sp->k()->str()] = sp->v()->str();
        }
    }

    std::vector<uint8_t> data;
    if (const auto* d = cmd->data()) data.assign(d->data(), d->data() + d->size());

    if (!rt_.is_allowed(key)) {
        return RanaResult{Status_FORBIDDEN,
                          "command '" + key + "' not in [commands.allowed]"};
    }

    // Every command (ask, talk, speak, lights, ...) is a scripts/*.pluto file.
    // No per-command C++: the script owns its chain (e.g. scripts/ask.pluto and
    // scripts/talk.pluto use rana.dispatch to glue hops together).
    return rt_.run_command(key, action, target, params, data);
}

std::vector<uint8_t> Executor::build_response(uint64_t request_id, const RanaResult& r) const {
    flatbuffers::FlatBufferBuilder b(256);
    const auto msg = b.CreateString(r.message);
    const auto resp = CreateResponse(b, request_id, r.status, msg);
    b.Finish(resp);
    return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}

void handle_connection(int fd, Executor& exec) {
    std::string frame;
    while (read_frame(fd, frame)) {
        RanaResult out = {Status_INVALID_PAYLOAD, "malformed flatbuffer"};
        uint64_t request_id = 0;

        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
        if (VerifyCommandBuffer(v)) {
            const Command* cmd = GetCommand(frame.data());
            request_id = cmd->request_id();
            rana::log_msg("conn", "recv request_id=%llu key=%s action=%s",
                          static_cast<unsigned long long>(request_id),
                          cmd->key() ? cmd->key()->c_str() : "",
                          cmd->action() ? cmd->action()->c_str() : "");
            out = exec.run(cmd);
        }

        // A forward needs no special path: it is status FORWARD_TO_CLIENT with
        // the base64 inner Command already in `message`.
        std::vector<uint8_t> resp = exec.build_response(request_id, out);
        if (!write_frame(fd, resp.data(), static_cast<uint32_t>(resp.size()))) break;
    }
}

}  // namespace rana
