#pragma once

#include <Arduino.h>

// Persistent key-value storage in the flash "kvs" partition (FlashDB).
// Survives deep sleep, reboots and reflashing.
namespace Storage {

bool begin();

// tado refresh token. It rotates on every use, so it must be saved each time.
String loadRefreshToken();
bool saveRefreshToken(const String& token);
bool clearRefreshToken();

// tado access token with its absolute expiry (Unix time), so it can be reused
// across reboots/deep sleep instead of refreshing every time.
bool loadAccessToken(String& token, uint32_t& expiresAt);
bool saveAccessToken(const String& token, uint32_t expiresAt);
bool clearAccessToken();

// Window state that was last applied successfully in tado.
enum class WindowState : uint8_t { Unknown = 0, Closed = 1, Open = 2 };
WindowState loadSentState();
bool saveSentState(WindowState state);

// Consecutive failed attempts to apply the state (for retry backoff).
uint8_t loadFailCount();
bool saveFailCount(uint8_t count);

// Access point and IP configuration from the last successful DHCP connect,
// used to reconnect quickly (no scan, no DHCP).
struct WifiCache {
    char ssid[33];  // the cache is only used for this SSID
    uint8_t bssid[6];
    int32_t channel;
    uint32_t ip, gateway, subnet, dns;  // IPAddress raw values
};
bool loadWifiCache(WifiCache& cache);
bool saveWifiCache(const WifiCache& cache);
bool clearWifiCache();

}  // namespace Storage
