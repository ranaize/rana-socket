#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <toml.hpp>

namespace rana {
namespace {

const std::set<std::string> kDaemonKeys = {
    "socket_type", "socket_path", "socket_permissions",
    "bind_address", "port", "allowed_ips", "scripts_dir",
};

// [llm]/[stt] hold a single [[llm.provider]] / [[stt.provider]] array.
const std::set<std::string> kHopKeys = {"provider"};

std::set<std::string> allowed_security_keys() {
    return {"external_egress"};
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

// One [[llm.provider]] / [[stt.provider]] entry. Strict: an unknown key, a missing
// url, a `type` that disagrees with the URL scheme, a non-openai schema, or an
// api_key on a cleartext (local) provider all reject the whole config.
bool parse_provider(const toml::table& t, const std::string& ctx,
                    ProviderConfig& out, std::string& err) {
    static const std::set<std::string> kKeys = {
        "type", "url", "model", "schema", "api_key", "timeout_ms", "order", "name",
    };
    if (const std::string e = unknown_key(t, kKeys, ctx); !e.empty()) {
        err = e;
        return false;
    }
    if (auto v = t.at_path("url").value<std::string>()) out.url = *v;
    if (auto v = t.at_path("model").value<std::string>()) out.model = *v;
    if (auto v = t.at_path("schema").value<std::string>()) out.schema = *v;
    if (auto v = t.at_path("api_key").value<std::string>()) out.api_key = *v;
    if (auto v = t.at_path("name").value<std::string>()) out.name = *v;
    if (auto v = t.at_path("timeout_ms").value<int64_t>()) {
        if (*v > 0) out.timeout_ms = static_cast<uint32_t>(*v);
    }
    if (auto v = t.at_path("order").value<int64_t>()) {
        if (*v > 0) out.order = static_cast<int>(*v);
    }

    if (out.url.empty()) {
        err = ctx + ": 'url' is required";
        return false;
    }
    const bool https = out.url.rfind("https://", 0) == 0;
    const bool http = out.url.rfind("http://", 0) == 0;
    if (!http && !https) {
        err = ctx + ": url must start with http:// or https://";
        return false;
    }
    // The URL scheme is the source of truth for the ACL tier; an explicit `type`
    // is only allowed to agree with it. That keeps a token-bearing https call from
    // being relabelled "local" to slip past the cloud gates.
    const std::string infer = https ? "huggingface" : "local";
    if (auto v = t.at_path("type").value<std::string>()) out.type = *v;
    if (out.type.empty()) out.type = infer;
    if (out.type != "local" && out.type != "huggingface") {
        err = ctx + ": type must be \"local\" or \"huggingface\"";
        return false;
    }
    if (out.type != infer) {
        err = ctx + ": type \"" + out.type + "\" does not match the " +
              (https ? "https" : "http") + " url";
        return false;
    }
    if (out.type == "local" && !out.api_key.empty()) {
        err = ctx + ": api_key is only allowed on a huggingface provider "
              "(a bearer token must never travel in cleartext)";
        return false;
    }
    if (out.schema != "openai") {
        err = ctx + ": schema \"" + out.schema + "\" is not supported (only \"openai\")";
        return false;
    }
    return true;
}

// Parse [llm]/[stt] into an ordered provider chain.
bool parse_providers(const toml::table& section, const std::string& name,
                     std::vector<ProviderConfig>& out, std::string& err) {
    if (const std::string e = unknown_key(section, kHopKeys, "[" + name + "]"); !e.empty()) {
        err = e;
        return false;
    }
    const auto* arr = section.at_path("provider").as_array();
    if (!arr) {
        if (section.contains("provider")) {
            err = "[" + name + ".provider] must be an array of tables";
            return false;
        }
        return true;  // no providers configured for this hop
    }
    int idx = 0;
    for (const auto& el : *arr) {
        ++idx;
        const auto* t = el.as_table();
        const std::string ctx = "[" + name + ".provider] #" + std::to_string(idx);
        if (!t) {
            err = ctx + " must be a table";
            return false;
        }
        ProviderConfig p;
        if (!parse_provider(*t, ctx, p, err)) return false;
        out.push_back(std::move(p));
    }
    // Stable sort by order (0 => tier default: local before huggingface), then
    // file order. With no `order` at all the chain is therefore local-then-cloud.
    std::stable_sort(out.begin(), out.end(),
                     [](const ProviderConfig& a, const ProviderConfig& b) {
        auto key = [](const ProviderConfig& p) {
            if (p.order > 0) return p.order;
            return p.type == "huggingface" ? 200 : 100;
        };
        return key(a) < key(b);
    });
    return true;
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

bool has_init_pluto(const std::string& dir) {
    if (dir.empty()) return false;
    struct stat st {};
    return ::stat((dir + "/init.pluto").c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string dirname_of(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

// Walk up from `start` (a directory) looking for a `scripts/` dir with init.pluto.
std::string find_scripts_upwards(const std::string& start) {
    std::string dir = start;
    for (;;) {
        const std::string cand = dir + "/scripts";
        if (has_init_pluto(cand)) return cand;
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
    if (has_init_pluto("/opt/rana/scripts")) return "/opt/rana/scripts";
    if (has_init_pluto(cfg.workdir + "/scripts")) return cfg.workdir + "/scripts";
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
            if (k != "daemon" && k != "llm" && k != "stt" && k != "security" && k != "commands") {
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
            if (!parse_providers(*llm, "llm", out.llm.providers, err)) return false;
        }

        if (const auto* stt = tbl["stt"].as_table()) {
            if (!parse_providers(*stt, "stt", out.stt.providers, err)) return false;
        }

        if (const auto* security = tbl["security"].as_table()) {
            if (const std::string e = unknown_key(*security, allowed_security_keys(), "[security]"); !e.empty()) {
                err = e;
                return false;
            }
            if (auto v = security->at_path("external_egress").value<bool>()) {
                out.security.external_egress = *v;
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
