#include "tado.h"

#include <ArduinoJson.h>

#include "log.h"
#include "secrets.h"
#include "storage.h"
#include "time_sync.h"

namespace {

constexpr const char* CLIENT_ID = "1bb50063-6b0c-4d11-bd99-387f4a91cc46";
constexpr const char* AUTH_URL = "https://login.tado.com/oauth2/device_authorize";
constexpr const char* TOKEN_URL = "https://login.tado.com/oauth2/token";
constexpr const char* API_BASE = "https://my.tado.com/api/v2";
constexpr const char* FORM = "application/x-www-form-urlencoded";

// Refresh this long before the access token actually expires.
constexpr uint32_t EXPIRY_MARGIN_S = 60;

String accessToken;
uint32_t accessTokenExpiresAtMs = 0;  // millis() deadline, valid for this boot
bool triedStoredAccessToken = false;

enum class TokenResult { Ok, Pending, SlowDown, InvalidGrant, Failed };

String urlEncode(const String& s) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

bool accessTokenValid() {
    if (accessToken.length() == 0) return false;
    return (int32_t)(accessTokenExpiresAtMs - millis()) > (int32_t)(EXPIRY_MARGIN_S * 1000);
}

void setAccessToken(const String& token, uint32_t lifetimeS) {
    accessToken = token;
    accessTokenExpiresAtMs = millis() + lifetimeS * 1000;
}

// Reuse an access token saved before the last reboot/deep sleep. Needs
// wall-clock time, because millis() restarts at 0 on every boot.
bool loadStoredAccessToken() {
    if (!TimeSync::valid()) {
        LOG("tado: no wall-clock time, cannot check stored access token");
        return false;
    }

    String token;
    uint32_t expiresAt;
    if (!Storage::loadAccessToken(token, expiresAt)) {
        LOG("tado: no stored access token");
        return false;
    }

    uint32_t now = TimeSync::now();
    if (expiresAt <= now + EXPIRY_MARGIN_S) {
        LOG("tado: stored access token expired");
        return false;
    }

    setAccessToken(token, expiresAt - now);
    LOG("tado: reusing stored access token (valid for %lu s)", (unsigned long)(expiresAt - now));
    return true;
}

// POST to the token endpoint and store the resulting tokens on success.
TokenResult requestToken(const String& form) {
    HttpResponse res = httpsRequest("POST", TOKEN_URL, form, FORM);
    if (res.status < 0) return TokenResult::Failed;

    JsonDocument doc;
    if (deserializeJson(doc, res.body)) {
        LOG("tado: token response is not JSON (HTTP %d): %s", res.status, res.body.c_str());
        return TokenResult::Failed;
    }

    if (res.status == 200 && doc["access_token"].is<const char*>()) {
        uint32_t lifetimeS = doc["expires_in"] | 600;
        setAccessToken(doc["access_token"].as<const char*>(), lifetimeS);
        if (TimeSync::valid()) {
            Storage::saveAccessToken(accessToken, TimeSync::now() + lifetimeS);
        }
        LOG("tado: got access token (valid for %lu s)", (unsigned long)lifetimeS);

        const char* refresh = doc["refresh_token"];
        if (refresh && *refresh) {
            // tado rotates the refresh token: the old one is now revoked.
            if (!Storage::saveRefreshToken(refresh)) {
                LOG("tado: WARNING: could not persist refresh token");
            }
        }
        return TokenResult::Ok;
    }

    String error = doc["error"] | "";
    if (error == "authorization_pending") return TokenResult::Pending;
    if (error == "slow_down") return TokenResult::SlowDown;
    if (error == "invalid_grant" || error == "expired_token" || error == "access_denied") {
        LOG("tado: %s: %s", error.c_str(), (const char*)(doc["error_description"] | ""));
        return TokenResult::InvalidGrant;
    }

    LOG("tado: token request failed (HTTP %d): %s", res.status, res.body.c_str());
    return TokenResult::Failed;
}

TokenResult refreshWithStoredToken() {
    String refresh = Storage::loadRefreshToken();
    if (refresh.length() == 0) {
        LOG("tado: no stored refresh token");
        return TokenResult::InvalidGrant;
    }

    LOG("tado: refreshing access token");
    String form = String("client_id=") + CLIENT_ID +
                  "&grant_type=refresh_token&refresh_token=" + urlEncode(refresh);
    return requestToken(form);
}

// OAuth2 device code flow (RFC 8628). The user approves the device in a
// browser; meanwhile we poll the token endpoint. No confirmation on the
// device is needed: the poll succeeds as soon as the user has approved.
bool deviceCodeFlow() {
    LOG("tado: starting device code flow");
    String form = String("client_id=") + CLIENT_ID + "&scope=offline_access";
    HttpResponse res = httpsRequest("POST", AUTH_URL, form, FORM);
    if (res.status != 200) {
        LOG("tado: device_authorize failed (HTTP %d): %s", res.status, res.body.c_str());
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, res.body)) {
        LOG("tado: device_authorize response is not JSON");
        return false;
    }

    String deviceCode = doc["device_code"] | "";
    uint32_t intervalMs = (doc["interval"] | 5) * 1000UL;
    uint32_t expiresMs = (doc["expires_in"] | 300) * 1000UL;
    if (deviceCode.length() == 0) {
        LOG("tado: device_authorize response has no device_code");
        return false;
    }

    // Always printed, even with DEBUG_LOG disabled: it is needed for setup.
    consolePrintf("");
    consolePrintf("================ tado login required ================");
    consolePrintf("Open this URL in a browser and approve the device:");
    consolePrintf("");
    consolePrintf("    %s", (const char*)(doc["verification_uri_complete"] | ""));
    consolePrintf("");
    consolePrintf("(or go to %s and enter code %s)", (const char*)(doc["verification_uri"] | ""),
                  (const char*)(doc["user_code"] | ""));
    consolePrintf("The code expires in %lu s.", (unsigned long)(expiresMs / 1000));
    consolePrintf("=====================================================");

    String pollForm = String("client_id=") + CLIENT_ID + "&device_code=" + urlEncode(deviceCode) +
                      "&grant_type=urn:ietf:params:oauth:grant-type:device_code";

    uint32_t start = millis();
    while (millis() - start < expiresMs) {
        delay(intervalMs);
        switch (requestToken(pollForm)) {
            case TokenResult::Ok:
                consolePrintf("tado: device approved, logged in");
                return true;
            case TokenResult::Pending:
                LOG("tado: waiting for approval...");
                break;
            case TokenResult::SlowDown:
                intervalMs += 5000;  // required by RFC 8628
                LOG("tado: slow_down, polling every %lu s", (unsigned long)(intervalMs / 1000));
                break;
            case TokenResult::InvalidGrant:
                return false;  // expired or denied
            case TokenResult::Failed:
                break;  // transient (network) error: keep polling
        }
    }
    consolePrintf("tado: login code expired");
    return false;
}

String overlayPath() {
    return String("/homes/") + TADO_HOME_ID + "/zones/" + TADO_ZONE_ID + "/overlay";
}

}  // namespace

namespace Tado {

bool ensureAuthenticated() {
    if (accessTokenValid()) return true;

    if (!triedStoredAccessToken) {
        triedStoredAccessToken = true;
        if (loadStoredAccessToken()) return true;
    }

    switch (refreshWithStoredToken()) {
        case TokenResult::Ok:
            return true;
        case TokenResult::InvalidGrant:
            // No token stored, or it was revoked/expired (30 days unused).
            Storage::clearRefreshToken();
            return deviceCodeFlow();
        default:
            // Network/server error: keep the stored token and retry later.
            return false;
    }
}

HttpResponse api(const char* method, const String& path, const String& jsonBody) {
    if (!ensureAuthenticated()) return HttpResponse{-1, String()};

    const char* contentType = jsonBody.length() ? "application/json;charset=utf-8" : nullptr;
    HttpResponse res = httpsRequest(method, String(API_BASE) + path, jsonBody, contentType, accessToken);
    if (res.status == 401) {
        // Token rejected early (e.g. revoked): refresh once and retry.
        LOG("tado: access token rejected (401), refreshing and retrying");
        accessToken = "";
        Storage::clearAccessToken();
        if (!ensureAuthenticated()) return res;
        res = httpsRequest(method, String(API_BASE) + path, jsonBody, contentType, accessToken);
    }
    return res;
}

bool setHeatingOff() {
    LOG("tado: setting zone %d heating OFF (manual, no timer)", TADO_ZONE_ID);
    const char* body =
        "{\"setting\":{\"type\":\"HEATING\",\"power\":\"OFF\"},"
        "\"termination\":{\"type\":\"MANUAL\"}}";
    HttpResponse res = api("PUT", overlayPath(), body);
    if (res.status >= 200 && res.status < 300) return true;
    LOG("tado: setting heating off failed (HTTP %d): %s", res.status, res.body.c_str());
    return false;
}

bool resumeSchedule() {
    LOG("tado: resuming schedule in zone %d (removing overlay)", TADO_ZONE_ID);
    HttpResponse res = api("DELETE", overlayPath());
    if (res.status >= 200 && res.status < 300) return true;
    LOG("tado: resuming schedule failed (HTTP %d): %s", res.status, res.body.c_str());
    return false;
}

}  // namespace Tado
