#include <Arduino.h>

#include "config.h"
#include "log.h"

#if APP_MODE == APP_MODE_SENSOR

// ---- Window sensor
//
// Waking from deep sleep is a full reboot, so everything happens in setup():
//   1. Debounce: wait until the reed switch is unchanged for REED_STABLE_MS.
//   2. If that state differs from what was last applied in tado, send it:
//        magnet near (window closed) -> remove overlay, follow the schedule
//        magnet gone (window open)   -> heating OFF, manual, no timer
//      One blink when sending starts; then one blink on success, or blinking
//      for ERROR_BLINK_MS on failure.
//   3. If the reed no longer matches the state just handled, handle it again.
//      Otherwise sleep until it changes, or until a timer: a short, growing
//      retry delay if the request failed, else the daily keepalive.

#include "secrets.h"
#include "storage.h"
#include "tado.h"
#include "time_sync.h"
#include "wifi_link.h"

extern "C" volatile int deep_sleep_pullup_gpio;  // deep_sleep_fixup.c

using Storage::WindowState;

const char* stateName(WindowState s) {
    switch (s) {
        case WindowState::Closed:
            return "CLOSED (magnet near)";
        case WindowState::Open:
            return "OPEN (magnet gone)";
        default:
            return "UNKNOWN";
    }
}

WindowState readReed() {
    return digitalRead(PIN_REED) == LOW ? WindowState::Closed : WindowState::Open;
}

// Block until the reed switch has been unchanged for REED_STABLE_MS.
WindowState waitForStableReed() {
    WindowState state = readReed();
    uint32_t since = millis();
    LOG("reed: %s, waiting %d ms for it to be stable", stateName(state), REED_STABLE_MS);
    while (millis() - since < REED_STABLE_MS) {
        delay(10);
        WindowState now = readReed();
        if (now != state) {
            LOG("reed: changed to %s, restarting debounce", stateName(now));
            state = now;
            since = millis();
        }
    }
    LOG("reed: stable %s", stateName(state));
    return state;
}

void ledOn() { digitalWrite(PIN_LED, HIGH); }
void ledOff() { digitalWrite(PIN_LED, LOW); }

void blinkOnce() {
    ledOn();
    delay(CONFIRM_BLINK_MS);
    ledOff();
}

void confirmBlink() {
    delay(CONFIRM_DELAY_MS);
    blinkOnce();
}

// Evenly for a failed request; a double flash per second if a tado login is
// needed (which retrying will not fix).
void errorBlink(bool loginNeeded) {
    LOG("led: signalling %s for %d s", loginNeeded ? "login required" : "error",
        ERROR_BLINK_MS / 1000);
    uint32_t start = millis();
    while (millis() - start < ERROR_BLINK_MS) {
        if (loginNeeded) {
            ledOn(); delay(100); ledOff(); delay(100);
            ledOn(); delay(100); ledOff(); delay(700);
        } else {
            ledOn(); delay(ERROR_BLINK_PERIOD_MS / 2);
            ledOff(); delay(ERROR_BLINK_PERIOD_MS / 2);
        }
    }
}

bool connectWifi(bool allowCache = true) {
    if (!Wifi::connect(allowCache)) return false;

    // Wall-clock time lets us reuse a still-valid access token across sleeps.
    // Not fatal if it fails: the token is then simply refreshed.
    if (!TimeSync::valid()) TimeSync::begin(5000);
    return true;
}

// Returns the HTTP status, or a negative network/authentication error.
int sendState(WindowState state) {
    if (!connectWifi()) return -1;
    return state == WindowState::Closed ? Tado::resumeSchedule() : Tado::setHeatingOff();
}

bool applyState(WindowState state) {
    uint32_t start = millis();
    LOG("apply: sending %s to tado", stateName(state));

    if (!Tado::loginPossible()) {
        // Nothing stored and no device code flow on this wake: don't even
        // power up WiFi.
        LOG("apply: no tado login stored, skipping");
        return false;
    }

    int status = sendState(state);
    if (status < 0 && Wifi::connectedFromCache() && !Tado::loginRequired()) {
        // A network-level failure over the cached AP/IP: it may be stale (e.g.
        // the router gave the IP to another device). Retry once with a fresh
        // scan + DHCP. HTTP errors from tado are not retried: they would fail
        // the same way.
        LOG("apply: network error over cached connection, retrying with DHCP");
        Wifi::invalidateCache();
        status = sendState(state);
    }

    bool ok = Tado::isSuccess(status);
    LOG("apply: %s (status %d) after %lu ms", ok ? "SUCCESS" : "FAILED", status,
        (unsigned long)(millis() - start));
    return ok;
}

// Keep the refresh token alive (it expires after 30 days unused). Uses DHCP,
// which also refreshes the cached IP configuration once a day.
void keepalive() {
    if (!Tado::loginPossible()) {
        LOG("keepalive: no tado login stored, skipping");
        return;
    }
    LOG("keepalive: refreshing tado login and WiFi cache");
    bool ok = connectWifi(false) && Tado::ensureAuthenticated();
    LOG("keepalive: %s", ok ? "ok" : "FAILED");
}

// Delay before retrying a failed request: RETRY_MIN_MS, doubling per
// consecutive failure, capped at RETRY_MAX_MS.
uint32_t retryDelayMs(uint8_t failCount) {
    uint32_t ms = RETRY_MIN_MS;
    for (uint8_t i = 1; i < failCount && ms < RETRY_MAX_MS; i++) ms *= 2;
    return ms < RETRY_MAX_MS ? ms : RETRY_MAX_MS;
}

// Sleep until the reed switch leaves `handled` (the state just dealt with),
// or until the timer fires. Returns (without sleeping) if the reed already
// differs from `handled`, so the caller can handle the change first.
void sleepUntilReedChange(WindowState handled, uint32_t timerMs) {
    // The SDK can only wake on one edge per pin: arm the edge leading away
    // from the handled state, and make sure we are still in that state.
    if (readReed() != handled) return;
    bool wakeOnRising = handled == WindowState::Closed;  // closed (LOW) -> wait for HIGH

    LOG("sleep: reed %s, waking on %s edge or in %lu min", stateName(handled),
        wakeOnRising ? "rising" : "falling", (unsigned long)(timerMs / 60000UL));
    Serial.flush();
    delay(20);
    if (readReed() != handled) return;  // changed while flushing the log

    // The SDK would apply a pull-down for a rising edge; keep the pull-up.
    deep_sleep_pullup_gpio = PIN_REED;
    lt_deep_sleep_config_gpio(1UL << PIN_REED, wakeOnRising);
    lt_deep_sleep_config_timer(timerMs);
    lt_deep_sleep_enter();
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    ledOff();
    pinMode(PIN_REED, INPUT_PULLUP);

    lt_reboot_reason_t reason = lt_get_reboot_reason();
    LOG("boot: reason %s, free heap %u bytes", lt_get_reboot_reason_name(reason),
        (unsigned)lt_heap_get_free());

    bool wokeFromTimer = reason == REBOOT_REASON_SLEEP_RTC;
    bool wokeFromSleep = wokeFromTimer || reason == REBOOT_REASON_SLEEP_GPIO;
    // The 5 min tado device code flow only makes sense after a power-on or
    // reset, when someone can be watching the serial port.
    Tado::setInteractiveLogin(!wokeFromSleep);

    // Only send when the state differs from what tado last accepted. (Opening
    // the serial monitor resets the chip, so don't send on every power-on.)
    WindowState sent = Storage::loadSentState();
    uint8_t failCount = Storage::loadFailCount();
    LOG("state: last applied in tado: %s, failed attempts: %u", stateName(sent), failCount);

    bool usedNetwork = false;
    bool keepaliveDone = false;
    while (true) {
        WindowState handled = waitForStableReed();

        if (handled != sent) {
            usedNetwork = true;
            blinkOnce();  // change detected; WiFi + request take a few seconds
            if (applyState(handled)) {
                Storage::saveSentState(handled);
                sent = handled;
                failCount = 0;
                confirmBlink();
            } else {
                // Not saved: retried after a short delay (see below).
                if (failCount < 255) failCount++;
                Wifi::off();  // don't keep the radio on during the long blink
                errorBlink(Tado::loginRequired());
            }
        } else {
            LOG("state: %s already applied, nothing to send", stateName(handled));
        }

        if (wokeFromTimer && !usedNetwork && !keepaliveDone) {
            keepaliveDone = true;
            keepalive();
        }

        // Sleep only if the reed is still in the state just handled; anything
        // that changed meanwhile (blinking, keepalive) is handled first.
        if (readReed() == handled) {
            bool pending = handled != sent;
            uint32_t timerMs = KEEPALIVE_INTERVAL_MS;
            if (pending && !Tado::loginRequired()) {
                timerMs = retryDelayMs(failCount);
                LOG("state: %s not applied yet, retrying in %lu min", stateName(handled),
                    (unsigned long)(timerMs / 60000UL));
            }
            Storage::saveFailCount(pending ? failCount : 0);
            sleepUntilReedChange(handled, timerMs);
        }
        LOG("reed: changed while busy, handling again");
    }
}

void loop() {}

#elif APP_MODE == APP_MODE_HW_TEST

// ---- Hardware test: the LED is lit while the magnet is near the reed switch.
// Reed and button changes are logged.

constexpr uint32_t DEBOUNCE_MS = 30;

int buttonStable = HIGH;  // debounced level (HIGH = released)
int buttonLastRead = HIGH;
uint32_t buttonChangedAt = 0;
int reedLast = -1;

void setup() {
    Serial.begin(115200);
    delay(500);
    consolePrintf("Hardware test: the LED lights while the magnet is near the reed switch;");
    consolePrintf("reed and button changes are logged.");

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BUTTON, INPUT);       // external pull-up
    pinMode(PIN_REED, INPUT_PULLUP);  // reed switch to GND
}

void loop() {
    // Reed switch: LOW = contact closed (magnet near) -> LED on.
    int reed = digitalRead(PIN_REED);
    if (reed != reedLast) {
        reedLast = reed;
        digitalWrite(PIN_LED, reed == LOW ? HIGH : LOW);
        consolePrintf("Reed switch: %s", reed == LOW ? "closed (magnet near), LED on"
                                                     : "open (no magnet), LED off");
    }

    // Button: log debounced presses and releases.
    int reading = digitalRead(PIN_BUTTON);
    if (reading != buttonLastRead) {
        buttonLastRead = reading;
        buttonChangedAt = millis();
    }
    if (reading != buttonStable && millis() - buttonChangedAt >= DEBOUNCE_MS) {
        buttonStable = reading;
        consolePrintf("Button %s", buttonStable == LOW ? "pressed" : "released");
    }

    delay(5);
}

#endif  // APP_MODE
