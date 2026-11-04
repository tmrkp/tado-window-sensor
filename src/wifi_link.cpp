#include "wifi_link.h"

#include <WiFi.h>

#include "config.h"
#include "log.h"
#include "secrets.h"
#include "storage.h"

namespace {

bool fromCache = false;

bool waitConnected(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) return false;
        delay(50);
    }
    return true;
}

String ipStr(IPAddress ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return String(buf);
}

void logConnected(const char* how, uint32_t start) {
    const uint8_t* b = WiFi.BSSID();
    LOG("wifi: connected (%s) in %lu ms, IP %s, AP %02X:%02X:%02X:%02X:%02X:%02X ch %d, RSSI %d dBm",
        how, (unsigned long)(millis() - start), ipStr(WiFi.localIP()).c_str(), b[0], b[1], b[2], b[3],
        b[4], b[5], (int)WiFi.channel(), (int)WiFi.RSSI());
}

bool connectFromCache(const Storage::WifiCache& c) {
    LOG("wifi: fast connect to %02X:%02X:%02X:%02X:%02X:%02X ch %d, static IP %s", c.bssid[0],
        c.bssid[1], c.bssid[2], c.bssid[3], c.bssid[4], c.bssid[5], (int)c.channel,
        ipStr(IPAddress(c.ip)).c_str());
    uint32_t start = millis();
    WiFi.config(IPAddress(c.ip), IPAddress(c.gateway), IPAddress(c.subnet), IPAddress(c.dns));
    WiFi.begin(WIFI_SSID, WIFI_PASS, c.channel, c.bssid);
    if (!waitConnected(WIFI_FAST_TIMEOUT_MS)) {
        LOG("wifi: fast connect timed out after %d ms", WIFI_FAST_TIMEOUT_MS);
        return false;
    }
    logConnected("cached", start);
    return true;
}

bool connectWithDhcp() {
    LOG("wifi: connecting to %s (scan + DHCP)", WIFI_SSID);
    uint32_t start = millis();
    WiFi.config((uint32_t)0, (uint32_t)0, (uint32_t)0);  // back to DHCP
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    if (!waitConnected(WIFI_TIMEOUT_MS)) {
        LOG("wifi: connection timed out after %d ms", WIFI_TIMEOUT_MS);
        return false;
    }
    logConnected("DHCP", start);
    return true;
}

void saveCache() {
    Storage::WifiCache c = {};
    strncpy(c.ssid, WIFI_SSID, sizeof(c.ssid) - 1);
    memcpy(c.bssid, WiFi.BSSID(), sizeof(c.bssid));
    c.channel = WiFi.channel();
    c.ip = (uint32_t)WiFi.localIP();
    c.gateway = (uint32_t)WiFi.gatewayIP();
    c.subnet = (uint32_t)WiFi.subnetMask();
    c.dns = (uint32_t)WiFi.dnsIP(0);
    if (c.dns == 0) c.dns = c.gateway;

    // Avoid flash writes if nothing changed.
    Storage::WifiCache old;
    if (Storage::loadWifiCache(old) && memcmp(&old, &c, sizeof(c)) == 0) {
        LOG("wifi: cache unchanged");
        return;
    }
    LOG("wifi: caching AP ch %d, IP %s, gw %s, mask %s, dns %s", (int)c.channel,
        ipStr(IPAddress(c.ip)).c_str(), ipStr(IPAddress(c.gateway)).c_str(),
        ipStr(IPAddress(c.subnet)).c_str(), ipStr(IPAddress(c.dns)).c_str());
    Storage::saveWifiCache(c);
}

}  // namespace

namespace Wifi {

bool connect(bool allowCache) {
    if (WiFi.status() == WL_CONNECTED) return true;
    fromCache = false;

    if (allowCache) {
        Storage::WifiCache c;
        if (!Storage::loadWifiCache(c)) {
            LOG("wifi: no cached connection");
        } else if (strcmp(c.ssid, WIFI_SSID) != 0) {
            LOG("wifi: cached connection is for another SSID, ignoring");
        } else if (connectFromCache(c)) {
            fromCache = true;
            return true;
        } else {
            invalidateCache();
        }
    }

    if (!connectWithDhcp()) return false;
    saveCache();
    return true;
}

bool connectedFromCache() {
    return fromCache;
}

void invalidateCache() {
    LOG("wifi: dropping cached connection");
    Storage::clearWifiCache();
    fromCache = false;

    // disconnect() is asynchronous: wait until the link is really down, or the
    // next connect() would see WL_CONNECTED and skip reconnecting.
    WiFi.disconnect();
    uint32_t start = millis();
    while (WiFi.status() == WL_CONNECTED && millis() - start < 3000) delay(20);
    LOG("wifi: disconnected after %lu ms", (unsigned long)(millis() - start));
}

void off() {
    if (WiFi.status() == WL_CONNECTED) LOG("wifi: switching off");
    WiFi.disconnect(true);  // also disables the station interface
    fromCache = false;
}

}  // namespace Wifi
