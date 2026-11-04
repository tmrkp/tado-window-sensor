#pragma once

#include "config.h"

// LOG("fmt", ...) prints "[  12.345] message" to the serial port when
// DEBUG_LOG is enabled, and compiles to nothing otherwise.
#if DEBUG_LOG
void logPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logPrintf(__VA_ARGS__)
#else
#define LOG(...) \
    do {         \
    } while (0)
#endif

// Always printed, regardless of DEBUG_LOG (e.g. the tado login URL).
void consolePrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
