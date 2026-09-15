// rana-serializer — serialization CLI.
//
// Default mode: reads the assistant content JSON ({"action":"...","payload":"..."})
// from stdin and emits a binary rana::AskReply FlatBuffer on stdout, replacing the
// old rana-ask-encode tool. rana-socketd verifies and reads that buffer, so no
// JSON is parsed inside the daemon.

#include "json.hpp"

#include <cstdio>
#include <string>

namespace {

std::string read_all_stdin() {
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) out.append(buf, n);
    return out;
}

}  // namespace

int main() {
    const std::string in = read_all_stdin();
    std::string out;
    if (!rana::serializer::json::ask_reply_from_json(in, out)) {
        std::fprintf(stderr, "rana-serializer: missing \"action\" in LLM reply\n");
        return 1;
    }
    if (std::fwrite(out.data(), 1, out.size(), stdout) != out.size()) {
        std::fprintf(stderr, "rana-serializer: write failed\n");
        return 1;
    }
    std::fflush(stdout);
    return 0;
}
