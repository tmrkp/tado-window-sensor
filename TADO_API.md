# tado° API: Turning Heating On/Off for a Zone

Summary of what the window sensor needs to switch heating on or off in a specific zone. Sources: tado°'s official help center article on REST API authentication (March 2025), plus the community-maintained v2 API spec and Terence Eden's 2019 API guide. Endpoints and payloads should be verified against a live account before relying on them.

## Overview

1. Authenticate once with the **OAuth2 device code flow** (a person has to approve it in a browser).
2. Store the **refresh token** and keep it up to date. It rotates on every use.
3. Look up the **home ID** and the **zone ID** of the room.
4. Send a **PUT** to `.../zones/{zoneId}/overlay` to turn heating on or off, or a **DELETE** to return the zone to its schedule.

## 1. Authentication

tado° removed the older password grant. Use the device code flow with the public client ID below.

| Item | Value |
|---|---|
| Client ID | `1bb50063-6b0c-4d11-bd99-387f4a91cc46` |
| Scope | `offline_access` (required to receive a refresh token) |

### 1a. Start device authorization (one time)

```
POST https://login.tado.com/oauth2/device_authorize?client_id=1bb50063-6b0c-4d11-bd99-387f4a91cc46&scope=offline_access
```

Response:

```json
{
  "device_code": "XXX_code_XXX",
  "expires_in": 300,
  "interval": 5,
  "user_code": "7BQ5ZQ",
  "verification_uri": "https://login.tado.com/oauth2/device",
  "verification_uri_complete": "https://login.tado.com/oauth2/device?user_code=7BQ5ZQ"
}
```

The user opens `verification_uri_complete` in any browser, logs in to their tado° account, and confirms. The device then polls the token endpoint at `interval` seconds until the request succeeds or `expires_in` runs out.

### 1b. Poll for tokens

```
POST https://login.tado.com/oauth2/token?client_id=1bb50063-6b0c-4d11-bd99-387f4a91cc46&device_code=XXX_code_XXX&grant_type=urn:ietf:params:oauth:grant-type:device_code
```

Response:

```json
{
  "access_token": "...",
  "expires_in": 3599,
  "refresh_token": "...",
  "scope": "offline_access",
  "token_type": "Bearer",
  "userId": "..."
}
```

### 1c. Refresh the access token

```
POST https://login.tado.com/oauth2/token?client_id=1bb50063-6b0c-4d11-bd99-387f4a91cc46&grant_type=refresh_token&refresh_token=...
```

Important points:

- **Refresh token rotation:** each refresh returns a new refresh token, and the old one is revoked immediately. The device has to write the new refresh token to persistent storage (flash) every time, or it will lose access.
- A refresh token expires after **30 days** if it isn't used. A device that refreshes regularly stays logged in indefinitely. A device that has been offline for more than 30 days needs the device flow again.
- The access token lasts about 10 minutes. tado° advises reusing it for several requests, not requesting a new one for each call, to stay under rate limits.

All API calls then use:

```
Authorization: Bearer <access_token>
```

## 2. Finding the home ID and zone ID

### Home ID

```
GET https://my.tado.com/api/v2/me
```

The response contains the home ID (`homes[0].id`, or `homeId` in older responses). Confirm the exact field name against a real response.

### Zones

```
GET https://my.tado.com/api/v2/homes/{homeId}/zones
```

Returns one entry per zone with `id`, `name`, and `type`. Heating zones have `"type": "HEATING"`. Zone `0` is usually Hot Water, and the room zones use other IDs. Zone IDs are not sequential, so look them up by `name` and do not assume a value. Store the zone ID in the config and do not look it up at runtime.

### Zone state (for checking the current status)

```
GET https://my.tado.com/api/v2/homes/{homeId}/zones/{zoneId}/state
```

Useful fields:

- `setting.power`: `"ON"` or `"OFF"`, the current setting
- `overlay`: `null` when the zone follows the schedule, otherwise the active manual override
- `openWindow`: present when tado° has detected an open window
- `sensorDataPoints.insideTemperature.celsius`: current room temperature

## 3. Turning heating on or off

### Turn heating OFF in a zone

```
PUT https://my.tado.com/api/v2/homes/{homeId}/zones/{zoneId}/overlay
Authorization: Bearer <access_token>
Content-Type: application/json;charset=utf-8
```

```json
{
  "setting": {
    "type": "HEATING",
    "power": "OFF"
  },
  "termination": {
    "type": "MANUAL"
  }
}
```

### Turn heating ON in a zone

```json
{
  "setting": {
    "type": "HEATING",
    "power": "ON",
    "temperature": {
      "celsius": 21
    }
  },
  "termination": {
    "type": "MANUAL"
  }
}
```

A temperature is required when turning heating on. A typical value is the target for the room. Send `"fahrenheit"` as well if you need it. The API accepts either, but the examples in the references send both.

### Termination types

| Termination | Meaning |
|---|---|
| `MANUAL` | The override lasts until it is removed with DELETE. |
| `TIMER` | Add `"durationInSeconds": <n>`. The override ends after that many seconds. |
| `NEXT_TIME_BLOCK` | Ends at the next schedule change. Reported by the community but not confirmed here, so verify before using it. |

For the window sensor, `MANUAL` plus a later DELETE is the simplest approach. `TIMER` is a safer choice if the device might lose connection and never send the reset.

### Return a zone to its schedule

```
DELETE https://my.tado.com/api/v2/homes/{homeId}/zones/{zoneId}/overlay
Authorization: Bearer <access_token>
```

This removes the manual override. The zone then follows its normal schedule, which may have heating on or off at that time.

### Response

A successful PUT returns the active overlay, for example:

```json
{
  "type": "MANUAL",
  "setting": {
    "type": "HEATING",
    "power": "OFF"
  },
  "termination": {
    "type": "MANUAL",
    "projectedExpiry": null
  }
}
```

Check the HTTP status code. A `2xx` status means success. Errors come back as JSON with an `errors` array containing `code` and `title`.

## 4. Errors and limits

- **`accessDenied`** ("current user is not allowed to access this resource"): reported by some users when calling PUT endpoints even with a valid token. Probably a permission or account-level restriction. Contact tado° support if this happens.
- **Expired or rejected token (401):** refresh the access token using the refresh token, store the new refresh token, then retry the call once.
- **Rate limits:** the exact limits are not documented. Community reports mention a limit of around 100 requests per day on some accounts or plans. Keep polling light, and send commands only when the window state actually changes.
- **Subscription:** some features (for example automatic away mode and some skills) are tied to a paid subscription. Core heating control via overlays has not been reported as blocked, but this may change.

## 5. Implications for the window sensor

- **HTTPS is required.** tado° endpoints only work over TLS. The current firmware uses plain HTTP, so it has to be upgraded to `WiFiClientSecure` (or the LibreTiny equivalent). Check memory use on the BK7231N (256 KB RAM) because TLS takes a significant share.
- **Refresh token storage:** the rotating refresh token must be stored in non-volatile memory (flash) and updated after every refresh. Losing it means someone has to repeat the device flow in a browser.
- **Config:** home ID, zone ID, and the initial refresh token belong in `include/secrets.h` (or a similar gitignored file), not in source code.
- **Logic:** when an open window is detected, send the OFF overlay. When the window closes, send either DELETE (return to schedule) or ON with a target temperature. Avoid sending the same command repeatedly. Track the last state and only send a change.
- **Open window detection:** tado° also has its own open window detection, reported in `zones/{zoneId}/state`. The window sensor could rely on that instead of, or alongside, its own sensor. Only the state reading is documented here, not the endpoint for activating it.

## References

- tado° help center: How do I authenticate to access the REST API? (March 2025, device code flow migration)
- Community OpenAPI spec for tado° API v2: https://kritsel.github.io/tado-openapispec-v2/swagger.html
- Terence Eden, "Tado API Guide – updated for 2019": https://shkspr.mobi/blog/2019/02/tado-api-guide-updated-for-2019/
- Home Assistant tado integration discussion on the OAuth change: https://github.com/home-assistant/core/issues/143148
