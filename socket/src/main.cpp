#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "executor.h"
#include "logger.h"
#include "socket_server.h"

namespace {
volatile std::sig_atomic_t g_running = 1;

// Async-signal-safe: only set a flag and write a fixed string to stderr so the
// shutdown is observable in the logs without touching non-signal-safe state.
void on_signal(int sig) {
    g_running = 0;
    const char* msg = (sig == SIGINT)    ? "rana-socketd: received SIGINT, shutting down\n"
                      : (sig == SIGTERM) ? "rana-socketd: received SIGTERM, shutting down\n"
                                         : "rana-socketd: received signal, shutting down\n";
    ::write(STDERR_FILENO, msg, std::strlen(msg));
}
}  // namespace

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

    // Log + break cleanly on shutdown. `make server`/`stop` deliver SIGTERM (via
    // the run script); a foreground run gets SIGINT. Ignoring SIGPIPE keeps us
    // alive when a client disconnects mid-write.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    rana::SocketServer srv(cfg.daemon);
    if (!srv.start(err)) {
        std::fprintf(stderr, "socket error: %s\n", err.c_str());
        return 1;
    }

    if (cfg.daemon.socket_type == "unix") {
        rana::log_msg("main", "rana-socketd listening on unix socket %s (mode %o)",
                      cfg.daemon.socket_path.c_str(), cfg.daemon.socket_permissions);
    } else {
        rana::log_msg("main", "rana-socketd listening on tcp %s:%u",
                      cfg.daemon.bind_address.c_str(), cfg.daemon.port);
    }

    // Privilege containment is external: run the daemon unprivileged; root is
    // reachable only via the scoped sudoers policy enforced by the executor.

    rana::Executor exec(cfg);
    while (g_running) {
        std::string peer;
        const int cfd = srv.accept_client(peer);
        if (cfd < 0) continue;  // EINTR on signal -> loop re-checks g_running
        rana::handle_connection(cfd, exec);
        ::close(cfd);
    }

    rana::log_msg("main", "rana-socketd stopped");
    return 0;
}
