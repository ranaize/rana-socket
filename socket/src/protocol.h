#pragma once

#include <cstdint>
#include <string>

namespace rana {

// Length-prefixed framing: [4-byte big-endian length][payload].

bool read_exact(int fd, void* buf, size_t n);

// Reads one frame; false on EOF, error, or oversized payload.
bool read_frame(int fd, std::string& out);

// Writes one frame; MSG_NOSIGNAL prevents SIGPIPE on closed TCP peers.
bool write_frame(int fd, const void* data, uint32_t len);

}  // namespace rana
