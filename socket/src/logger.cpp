#include "logger.h"

#include <cstring>
#include <mutex>

namespace rana {
namespace {

std::mutex g_log_mu;

}  // namespace

void log_msg(const char* tag, const char* fmt, ...) {
    std::time_t now = std::time(nullptr);
    char ts[16];
    if (std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&now)) == 0) {
        ts[0] = '\0';
    }

    va_list ap;
    va_start(ap, fmt);
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        std::fprintf(stderr, "[%s] %s: ", ts, tag);
        std::vfprintf(stderr, fmt, ap);
        std::fprintf(stderr, "\n");
    }
    va_end(ap);
}

}  // namespace rana
