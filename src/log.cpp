#include "log.hpp"
#include "settings.hpp"
#include <coreinit/debug.h>
#include <cstdarg>
#include <cstdio>

namespace {
void Emit(int required, const char *prefix, const char *format, va_list args) {
    if (Settings::logLevel.load() < required) return;
    char body[512];
    std::vsnprintf(body, sizeof(body), format, args);
    OSReport("[WiiUWebStream][%s] %s\n", prefix, body);
}
}
namespace Log {
void Error(const char *format, ...) { va_list args; va_start(args, format); Emit(1, "ERR", format, args); va_end(args); }
void Info(const char *format, ...) { va_list args; va_start(args, format); Emit(2, "INF", format, args); va_end(args); }
void Verbose(const char *format, ...) { va_list args; va_start(args, format); Emit(3, "DBG", format, args); va_end(args); }
}
