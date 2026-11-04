#pragma once

#include <Arduino.h>

// Wall-clock time via SNTP (the BK7231N has no RTC; the clock restarts at 0
// on every boot).
namespace TimeSync {

// Start SNTP and wait up to `timeoutMs` for the first sync.
bool begin(uint32_t timeoutMs = 10000);

// True once the clock has been set by SNTP.
bool valid();

// Current Unix time in seconds (only meaningful if valid()).
uint32_t now();

}  // namespace TimeSync
