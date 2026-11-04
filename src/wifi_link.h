#pragma once

#include <Arduino.h>

// WiFi connection with a fast path: after a successful DHCP connect, the
// access point (BSSID, channel) and IP configuration are cached in flash.
// Later connects use them directly (no scan, no DHCP) and fall back to a
// normal DHCP connect if that fails.
namespace Wifi {

// Connect to WIFI_SSID. With allowCache=false, always scan + DHCP (and
// refresh the cache).
bool connect(bool allowCache = true);

// True if the current connection was made from the cache.
bool connectedFromCache();

// Drop the cache (e.g. a request failed after a cached connect) and
// disconnect, so the next connect() does a full DHCP connect.
void invalidateCache();

// Disconnect and switch the radio off (e.g. before a long LED signal).
void off();

}  // namespace Wifi
