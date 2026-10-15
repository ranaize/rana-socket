#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // execvpe
#endif

#include "luafunctions.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rana {

namespace {

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

}  // namespace

SpawnResult spawn_process(const std::string& workdir,
                         const std::vector<std::string>& argv,
                         const std::vector<std::string>& extra_env,
                         uint32_t timeout_ms,
                         bool capture_stdout) {
    SpawnResult res;
    if (argv.empty()) { res.spawn_failed = true; return res; }

    int err_pipe[2];
    if (pipe(err_pipe) != 0) { res.spawn_failed = true; return res; }

    int out_pipe[2] = {-1, -1};
    if (capture_stdout) {
        if (pipe(out_pipe) != 0) {
            ::close(err_pipe[0]); ::close(err_pipe[1]);
            res.spawn_failed = true; return res;
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
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        if (capture_stdout) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
        res.spawn_failed = true; return res;
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
            if (r > 0) { dst.append(buf, static_cast<size_t>(r)); continue; }
            break;
        }
    };

    std::string err_out, out_out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    for (;;) {
        drain(err_pipe[0], err_out);
        if (capture_stdout) drain(out_fd, out_out);
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) { if (errno == EINTR) continue; res.spawn_failed = true; break; }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            res.timed_out = true; break;
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

}  // namespace rana
