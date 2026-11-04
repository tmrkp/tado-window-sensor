#pragma once

#include <Arduino.h>

#include "https.h"

namespace Tado {

// Ensure a valid access token is available: reuse the cached/stored one,
// refresh it with the stored refresh token, or fall back to the OAuth2 device
// code flow (which requires the user to approve the device in a browser).
bool ensureAuthenticated();

// Authenticated request against https://my.tado.com/api/v2<path>.
// On 401 the access token is refreshed once and the request retried.
HttpResponse api(const char* method, const String& path, const String& jsonBody = String());

// Turn heating in the configured zone off, until changed manually (no timer).
bool setHeatingOff();

// Remove any manual overlay, so the zone follows its schedule again.
bool resumeSchedule();

}  // namespace Tado
