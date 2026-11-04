#pragma once

#include <Arduino.h>

#include "https.h"

namespace Tado {

// Allow the OAuth2 device code flow (user approves the device in a browser,
// up to 5 min with WiFi on). Only enable this when someone can be watching
// the serial port, i.e. after power-on/reset, not on every wake from sleep.
void setInteractiveLogin(bool allowed);

// False if there is no stored login and the device code flow is not allowed,
// so a request cannot succeed (no need to even connect to WiFi).
bool loginPossible();

// True if the last authentication failed because a new login is needed
// (no/invalid refresh token and device code flow not allowed or failed).
bool loginRequired();

// Ensure a valid access token is available: reuse the cached/stored one,
// refresh it with the stored refresh token, or (if allowed) fall back to the
// device code flow.
bool ensureAuthenticated();

// Authenticated request against https://my.tado.com/api/v2<path>.
// On 401 the access token is refreshed once and the request retried.
// Returns status -1 if authentication failed.
HttpResponse api(const char* method, const String& path, const String& jsonBody = String());

// Turn heating in the configured zone off, until changed manually (no timer).
// Returns the HTTP status (2xx = success) or a negative network error.
int setHeatingOff();

// Remove any manual overlay, so the zone follows its schedule again.
// Returns the HTTP status (2xx = success) or a negative network error.
int resumeSchedule();

inline bool isSuccess(int status) { return status >= 200 && status < 300; }

}  // namespace Tado
