#pragma once

#include <string>

struct lua_State;  // opaque; defined by the embedded Pluto/Lua headers in the .cpp

namespace rana {

// Minimal JSON (encode/parse) operating directly on the Lua stack.
// `encode` pushes nothing and appends to `out`; `decode` pushes exactly one
// value and throws std::runtime_error on malformed input. Shared by the rana
// bridge functions (rana.http_post / rana.provider_post / rana.json_decode)
// and by the startup meta scan.
namespace json {

void encode(lua_State* L, int idx, std::string& out);
void decode(lua_State* L, const std::string& s);

}  // namespace json

}  // namespace rana
