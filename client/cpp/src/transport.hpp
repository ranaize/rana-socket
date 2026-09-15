// Wire transport for the rana-socket client: open a connected unix/TCP socket
// and exchange length-prefixed frames using the daemon's framing
// ([4-byte big-endian length][payload]). No command knowledge lives here.

#pragma once

#include <cstdint>
#include <string>

#include "addr.hpp"

constexpr uint32_t kMaxFrameSize = 16u * 1024 * 1024;

// Opens the connected socket for either scheme. Returns -1 and sets `err` on
// failure; the caller owns the returned descriptor.
int open_socket(const Addr& addr, std::string& err);

bool send_exact(int fd, const void* data, size_t n);
bool recv_exact(int fd, void* buf, size_t n);

// Frame the payload with the 4-byte big-endian length header.
bool send_frame(int fd, const void* data, uint32_t n);
// Reads one [4-byte BE length][payload] frame into `out`.
bool recv_frame(int fd, std::string& out);

// Sends the built command flatbuffer, reads the daemon's framed `Response`,
// prints it, and returns the process exit code (0 = OK, 1 = rejected).
int run_client(const std::string& addr_raw, const uint8_t* cmd_data, size_t cmd_len);
