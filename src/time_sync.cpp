#include "time_sync.h"

#include "log.h"

#include <lwip/apps/sntp.h>
#include <lwip/tcpip.h>
#include <sys/time.h>
#include <time.h>

namespace {

// Anything before 2024-01-01 means the clock has not been set yet.
constexpr time_t MIN_VALID_TIME = 1704067200;

bool started = false;

// Use gettimeofday() (provided by LibreTiny) rather than time(): time() drags
// in newlib's _gettimeofday stub, which clashes with LibreTiny's at link time.
time_t currentTime() {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return tv.tv_sec;
}

// lwIP raw API calls must run on the tcpip thread.
void startSntp(void*) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_init();
}

}  // namespace

namespace TimeSync {

bool begin(uint32_t timeoutMs) {
    if (!started) {
        tcpip_callback(startSntp, nullptr);
        started = true;
    }
    uint32_t start = millis();
    while (!valid()) {
        if (millis() - start > timeoutMs) {
            LOG("time: SNTP sync timed out");
            return false;
        }
        delay(100);
    }
    time_t t = currentTime();
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime(&t));
    LOG("time: synced in %lu ms, UTC %s", (unsigned long)(millis() - start), buf);
    return true;
}

bool valid() {
    return currentTime() >= MIN_VALID_TIME;
}

uint32_t now() {
    return (uint32_t)currentTime();
}

}  // namespace TimeSync
