#include "log.h"

#include <stdarg.h>
#include <stdio.h>

namespace {

void vprint(bool timestamp, const char* fmt, va_list args) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    if (timestamp) {
        uint32_t ms = millis();
        char ts[16];
        snprintf(ts, sizeof(ts), "[%4lu.%03lu] ", (unsigned long)(ms / 1000),
                 (unsigned long)(ms % 1000));
        Serial.print(ts);
    }
    Serial.println(buf);
}

}  // namespace

#if DEBUG_LOG
void logPrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(true, fmt, args);
    va_end(args);
}
#endif

void consolePrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(false, fmt, args);
    va_end(args);
}
