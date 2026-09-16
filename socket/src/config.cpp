#include "config.h"

#include <cctype>
#include <map>
#include <set>

#include <toml.hpp>

namespace rana {
namespace {

const std::set<std::string> kDaemonKeys = {
    "socket_type", "socket_path", "socket_permissions",
    "bind_address", "port", "allowed_ips",
};

std::set<std::string> allowed_command_keys(const std::string& type) {
    std::set<std::string> keys = {"type", "timeout_ms", "description", "keywords", "danger", "forward_only"};
    if (type == "script") {
        keys.insert("path");
        keys.insert("default_subnet");
    } else if (type == "exec") {
        keys.insert("binary");
        keys.insert("default_url");
    } else if (type == "docker_compose") {
        keys.insert("workdir");
        keys.insert("default_world");
    } else if (type == "system") {
        keys.insert("binary");
        keys.insert("suspend_binary");
        keys.insert("requires_confirm");
    }
    return keys;
}

std::set<std::string> allowed_stt_keys() {
    return {"script", "url", "model", "timeout_ms"};
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

}  // namespace

// The FlatBuffers union table for a command is conventionally the config key in
// CamelCase + "Cmd" (e.g. "volume" -> "VolumeCmd", "mc_server" -> "McServerCmd").
// A few tables diverge from that naming; listed in kOverride so the toml never
// needs a 'variant' attribute repeating the schema name.
std::string derive_variant(const std::string& key) {
    static const std::map<std::string, std::string> kOverride = {
        {"browser", "LaunchBrowserCmd"},
    };
    auto it = kOverride.find(key);
    if (it != kOverride.end()) return it->second;

    std::string out;
    bool cap = true;
    for (char c : key) {
        if (c == '_') { cap = true; continue; }
        out += cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        cap = false;
    }
    return out + "Cmd";
}

bool Config::load(const std::string& path, Config& out, std::string& err) {
    try {
        out.path = path;
        const toml::parse_result tbl = toml::parse_file(path);

        for (const auto& [key, value] : tbl) {
            const std::string k(key);
            if (k != "daemon" && k != "commands" && k != "stt") {
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
            if (const auto* arr = daemon->at_path("allowed_ips").as_array()) {
                for (const auto& el : *arr) {
                    if (auto s = el.value<std::string>()) out.daemon.allowed_ips.push_back(*s);
                }
            }
        }

        if (const auto* commands = tbl["commands"].as_table()) {
            for (const auto& [name, node] : *commands) {
                const auto* t = node.as_table();
                const std::string key(name);
                if (t == nullptr) {
                    err = "[commands." + key + "]: not a table";
                    return false;
                }
                const std::string type = t->at_path("type").value_or(std::string());
                if (type.empty()) {
                    err = "[commands." + key + "]: missing 'type'";
                    return false;
                }
                if (const std::string e = unknown_key(*t, allowed_command_keys(type), "[commands." + key + "]");
                    !e.empty()) {
                    err = e;
                    return false;
                }

                CommandEntry entry;
                entry.variant = derive_variant(key);
                entry.type = type;
                entry.path = t->at_path("path").value_or(std::string());
                entry.binary = t->at_path("binary").value_or(std::string());
                entry.workdir = t->at_path("workdir").value_or(std::string());
                entry.default_world = t->at_path("default_world").value_or(std::string());
                entry.default_url = t->at_path("default_url").value_or(std::string());
                entry.default_subnet = t->at_path("default_subnet").value_or(std::string());
                entry.suspend_binary = t->at_path("suspend_binary").value_or(std::string());
                entry.requires_confirm = t->at_path("requires_confirm").value_or(false);
                entry.timeout_ms =
                    static_cast<uint32_t>(t->at_path("timeout_ms").value_or(int64_t{5000}));
                entry.description = t->at_path("description").value_or(std::string());
                entry.danger = t->at_path("danger").value_or(false);
                entry.forward_only = t->at_path("forward_only").value_or(false);
                if (const auto* arr = t->at_path("keywords").as_array()) {
                    for (const auto& el : *arr) {
                        if (auto s = el.value<std::string>()) entry.keywords.push_back(*s);
                    }
                }
                out.commands.emplace(key, std::move(entry));
            }
        }

        if (const auto* stt = tbl["stt"].as_table()) {
            if (const std::string e = unknown_key(*stt, allowed_stt_keys(), "[stt]"); !e.empty()) {
                err = e;
                return false;
            }
            out.stt.script    = stt->at_path("script").value_or(std::string());
            out.stt.url       = stt->at_path("url").value_or(std::string("http://localai:8080"));
            out.stt.model     = stt->at_path("model").value_or(std::string("talk"));
            out.stt.timeout_ms =
                static_cast<uint32_t>(stt->at_path("timeout_ms").value_or(int64_t{30000}));
        }

        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
}

}  // namespace rana
