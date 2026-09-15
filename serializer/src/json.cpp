#include "json.hpp"
#include "ask_reply_generated.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace rana {
namespace serializer {
namespace json {
namespace {

// Minimal, trust-domain JSON string extractor for the two top-level fields.
// Handles the escapes that can appear in natural-language payloads.
bool extract(const std::string& json, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
                                json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;  // opening quote
    std::string val;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') { out = val; return true; }
        if (c == '\\' && pos < json.size()) {
            const char e = json[pos++];
            switch (e) {
                case 'n': val += '\n'; break;
                case 't': val += '\t'; break;
                case 'r': val += '\r'; break;
                case 'b': val += '\b'; break;
                case 'f': val += '\f'; break;
                case '/': val += '/'; break;
                case '\\': val += '\\'; break;
                case '"': val += '"'; break;
                case 'u':
                    if (pos + 4 <= json.size()) { val += "\\u"; val.append(json, pos, 4); pos += 4; }
                    break;
                default: val += e; break;
            }
        } else {
            val += c;
        }
    }
    return false;
}

}  // namespace

bool ask_reply_from_json(const std::string& json, std::string& out_flatbuffer) {
    std::string action, payload;
    if (!extract(json, "action", action) || action.empty()) {
        return false;
    }
    extract(json, "payload", payload);  // optional

    flatbuffers::FlatBufferBuilder fbb(256);
    const auto action_off = fbb.CreateString(action);
    const auto payload_off = fbb.CreateString(payload);
    const auto reply = rana::CreateAskReply(fbb, action_off, payload_off);
    fbb.Finish(reply);

    const uint8_t* p = fbb.GetBufferPointer();
    const size_t n = fbb.GetSize();
    out_flatbuffer.assign(reinterpret_cast<const char*>(p), n);
    return true;
}

}  // namespace json
}  // namespace serializer
}  // namespace rana
