#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace rana {

// Minimal timestamped stderr logger. Always-on for now; a future debug flag can
// gate verbose categories. Usage: rana::log_msg("daemon", "listening on %s", path);
void log_msg(const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace rana
