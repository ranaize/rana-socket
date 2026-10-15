#include "../luafunctions.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rana {

namespace {

Status status_from_string(const std::string& s) {
    if (s == "ok") return Status_OK;
    if (s == "unknown" || s == "unknown_command") return Status_UNKNOWN_COMMAND;
    if (s == "forbidden") return Status_FORBIDDEN;
    if (s == "invalid" || s == "invalid_payload") return Status_INVALID_PAYLOAD;
    if (s == "safety" || s == "safety_gate_failed") return Status_SAFETY_GATE_FAILED;
    if (s == "forward" || s == "forward_to_client") return Status_FORWARD_TO_CLIENT;
    return Status_EXECUTION_FAILED;  // "error" / default
}

std::string status_to_string(Status s) {
    switch (s) {
        case Status_OK: return "ok";
        case Status_UNKNOWN_COMMAND: return "unknown_command";
        case Status_FORBIDDEN: return "forbidden";
        case Status_INVALID_PAYLOAD: return "invalid_payload";
        case Status_SAFETY_GATE_FAILED: return "safety_gate_failed";
        case Status_FORWARD_TO_CLIENT: return "forward_to_client";
        case Status_EXECUTION_FAILED:
        default: return "execution_failed";
    }
}

}  // namespace

}  // namespace rana
