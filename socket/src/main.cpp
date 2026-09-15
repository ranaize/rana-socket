#include <unistd.h>

#include <cstdio>
#include <string>

#include "config.h"
#include "executor.h"
#include "socket_server.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: rana-socketd <config.toml>\n");
        return 2;
    }

    rana::Config cfg;
    std::string err;
    if (!rana::Config::load(argv[1], cfg, err)) {
        std::fprintf(stderr, "config error: %s\n", err.c_str());
        return 1;
    }

    rana::SocketServer srv(cfg.daemon);
    if (!srv.start(err)) {
        std::fprintf(stderr, "socket error: %s\n", err.c_str());
        return 1;
    }

    // Privilege containment is external: run the daemon unprivileged
    // (dinit `run-as`); root is reachable only via the scoped sudoers
    // policy enforced by the executor.

    rana::Executor exec(cfg);
    for (;;) {
        std::string peer;
        const int cfd = srv.accept_client(peer);
        if (cfd < 0) continue;
        rana::handle_connection(cfd, exec);
        ::close(cfd);
    }
}
