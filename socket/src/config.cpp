#include "config.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

#include <sys/stat.h>
#include <unistd.h>

#include <toml.hpp>

namespace rana {
namespace {

const std::set<std::string> kDaemonKeys = {
    "socket_type", "socket_path", "socket_permissions",
    "bind_address", "port", "allowed_ips", "scripts_dir",
};

std::set<std::string> allowed_llm_keys() {
    return {"url", "model"};
}

std::set<std::string> allowed_stt_keys() {
    return {"url", "model", "timeout_ms"};
}

std::string unknown_key(const toml::table& table, const std::set<std::string>& allowed,
                        const std::string& context) {
    for (const auto& [key, value] : table) {
        if (allowed.count(std::string(key)) == 0) {
            return context + ": unknown key '" + std::string(key) + "'";
        }
    }
    return {};
}

// Directory of the running daemon binary (Linux: /proc/self/exe).
std::string exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        const std::string p(buf, static_cast<size_t>(n));
        const auto pos = p.find_last_of('/');
        return (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
    }
    return ".";
}

bool has_init_lua(const std::string& dir) {
    if (dir.empty()) return false;
    struct stat st {};
    return ::stat((dir + "/init.lua").c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string dirname_of(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

// Walk up from `start` (a directory) looking for a `scripts/` dir with init.lua.
std::string find_scripts_upwards(const std::string& start) {
    std::string dir = start;
    for (;;) {
        const std::string cand = dir + "/scripts";
        if (has_init_lua(cand)) return cand;
        const std::string parent = dirname_of(dir);
        if (parent == dir) break;  // reached filesystem root
        dir = parent;
    }
    return {};
}

}  // namespace

std::string resolve_scripts_dir(const Config& cfg) {
    // 1) Explicit env override (host launch / Makefile sets RANA_SCRIPTS_DIR).
    if (const char* e = ::getenv("RANA_SCRIPTS_DIR")) {
        if (std::string s(e); !s.empty()) return s;
    }
    // 2) Config value (empty => fall through to autodiscover).
    std::string cfg_dir = cfg.daemon.scripts_dir;
    if (!cfg_dir.empty()) {
        if (cfg_dir[0] != '/') cfg_dir = cfg.workdir + "/" + cfg_dir;
        return cfg_dir;
    }
    // 3) Autodiscover: walk up from the binary, then fixed install locations.
    if (auto f = find_scripts_upwards(exe_dir()); !f.empty()) return f;
    if (has_init_lua("/opt/rana/scripts")) return "/opt/rana/scripts";
    if (has_init_lua(cfg.workdir + "/scripts")) return cfg.workdir + "/scripts";
    // Nothing found — return a best-guess path so the error names what we wanted.
    return cfg.workdir + "/scripts";
}

std::string normalize_key(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool Config::load(const std::string& path, Config& out, std::string& err) {
    try {
        out.path = path;

        // workdir defaults to the config file's directory so scripts/ resolves
        // relative to wherever the daemon's config lives.
        {
            std::string p = path;
            const auto slash = p.find_last_of("/\\");
            out.workdir = (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
            if (out.workdir.empty()) out.workdir = ".";
        }

        const toml::parse_result tbl = toml::parse_file(path);

        for (const auto& [key, value] : tbl) {
            const std::string k(key);
            if (k != "daemon" && k != "llm" && k != "stt" && k != "commands") {
                err = "unknown top-level key '" + k + "'";
                return false;
            }
        }

        if (const auto* daemon = tbl["daemon"].as_table()) {
            if (const std::string e = unknown_key(*daemon, kDaemonKeys, "[daemon]"); !e.empty()) {
                err = e;
                return false;
            }
            if (auto v = daemon->at_path("socket_type").value<std::string>()) out.daemon.socket_type = *v;
            if (auto v = daemon->at_path("socket_path").value<std::string>()) out.daemon.socket_path = *v;
            if (auto v = daemon->at_path("socket_permissions").value<int64_t>()) {
                out.daemon.socket_permissions = static_cast<uint32_t>(*v);
            }
            if (auto v = daemon->at_path("bind_address").value<std::string>()) out.daemon.bind_address = *v;
            if (auto v = daemon->at_path("port").value<int64_t>()) out.daemon.port = static_cast<uint16_t>(*v);
            if (auto v = daemon->at_path("scripts_dir").value<std::string>()) out.daemon.scripts_dir = *v;
            if (const auto* arr = daemon->at_path("allowed_ips").as_array()) {
                for (const auto& el : *arr) {
                    if (auto s = el.value<std::string>()) out.daemon.allowed_ips.push_back(*s);
                }
            }
        }

        if (const auto* llm = tbl["llm"].as_table()) {
            if (const std::string e = unknown_key(*llm, allowed_llm_keys(), "[llm]"); !e.empty()) {
                err = e;
                return false;
            }
            if (auto v = llm->at_path("url").value<std::string>()) out.llm.url = *v;
            if (auto v = llm->at_path("model").value<std::string>()) out.llm.model = *v;
        }

        if (const auto* stt = tbl["stt"].as_table()) {
            if (const std::string e = unknown_key(*stt, allowed_stt_keys(), "[stt]"); !e.empty()) {
                err = e;
                return false;
            }
            if (auto v = stt->at_path("url").value<std::string>()) out.stt.url = *v;
            if (auto v = stt->at_path("model").value<std::string>()) out.stt.model = *v;
            if (auto v = stt->at_path("timeout_ms").value<int64_t>()) {
                out.stt.timeout_ms = static_cast<uint32_t>(*v);
            }
        }

        // [commands] now holds only [commands.allowed] — a set of enabled script
        // keys. Any other [commands.*] table is rejected; per-command tuning lives
        // in the script's own meta table.
        if (const auto* commands = tbl["commands"].as_table()) {
            if (const auto* allowed = commands->at_path("allowed").as_table()) {
                for (const auto& [name, value] : *allowed) {
                    const std::string key(name);
                    const bool enabled = value.value_or(false);
                    if (enabled) out.allowed.insert(normalize_key(key));
                }
            } else if (commands->contains("allowed")) {
                err = "[commands.allowed] must be a table of key = bool";
                return false;
            } else if (!commands->empty()) {
                err = "[commands] may only contain [commands.allowed]; "
                      "per-command tables (variant/type/path) are no longer supported";
                return false;
            }
        }

        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
}

}  // namespace rana
