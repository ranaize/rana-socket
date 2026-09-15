#pragma once

#include <string>

namespace rana {
namespace serializer {
namespace json {

// Builds a binary rana::AskReply FlatBuffer from the assistant content JSON
// ({"action":"...","payload":"..."}). Returns false (and leaves out_flatbuffer
// empty) when the required "action" field is missing. Used by the
// rana-serializer CLI so the daemon never parses JSON itself.
bool ask_reply_from_json(const std::string& json, std::string& out_flatbuffer);

}  // namespace json
}  // namespace serializer
}  // namespace rana
