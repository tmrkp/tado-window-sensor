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
//   3. If the reed changed meanwhile, handle the new state; otherwise sleep
//      until it changes (or the keepalive timer fires).

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

void errorBlink() {
    LOG("led: signalling error for %d s", ERROR_BLINK_MS / 1000);
    uint32_t start = millis();
    while (millis() - start < ERROR_BLINK_MS) {
        ledOn();
        delay(ERROR_BLINK_PERIOD_MS / 2);
        ledOff();
        delay(ERROR_BLINK_PERIOD_MS / 2);
    }
}

bool connectWifi(bool allowCache = true) {
    if (!Wifi::connect(allowCache)) return false;

    // Wall-clock time lets us reuse a still-valid access token across sleeps.
    // Not fatal if it fails: the token is then simply refreshed.
    if (!TimeSync::valid()) TimeSync::begin(5000);
    return true;
}

bool sendState(WindowState state) {
    return state == WindowState::Closed ? Tado::resumeSchedule() : Tado::setHeatingOff();
}

bool applyState(WindowState state) {
    uint32_t start = millis();
    LOG("apply: sending %s to tado", stateName(state));
    bool ok = connectWifi() && sendState(state);
    if (!ok && Wifi::connectedFromCache()) {
        // The cached AP/IP may be stale (e.g. the router gave the IP to
        // another device). Retry once with a fresh scan + DHCP.
        LOG("apply: failed over cached connection, retrying with DHCP");
        Wifi::invalidateCache();
        ok = connectWifi(false) && sendState(state);
    }
    LOG("apply: %s after %lu ms", ok ? "SUCCESS" : "FAILED", (unsigned long)(millis() - start));
    return ok;
}

// Keep the refresh token alive (it expires after 30 days unused). Uses DHCP,
// which also refreshes the cached IP configuration once a day.
void keepalive() {
    LOG("keepalive: refreshing tado login and WiFi cache");
    bool ok = connectWifi(false) && Tado::ensureAuthenticated();
    LOG("keepalive: %s", ok ? "ok" : "FAILED");
}

void sleepUntilReedChange() {
    // The SDK can only wake on one edge per pin, so arm the opposite of the
    // current level. Read as late as possible to keep the race window small.
    WindowState state = readReed();
    bool wakeOnRising = state == WindowState::Closed;  // closed (LOW) -> wait for HIGH

    LOG("sleep: reed %s, waking on %s edge or in %lu h", stateName(state),
        wakeOnRising ? "rising" : "falling", (unsigned long)(KEEPALIVE_INTERVAL_MS / 3600000UL));
    Serial.flush();
    delay(20);

    // The SDK would apply a pull-down for a rising edge; keep the pull-up.
    deep_sleep_pullup_gpio = PIN_REED;
    lt_deep_sleep_config_gpio(1UL << PIN_REED, wakeOnRising);
    lt_deep_sleep_config_timer(KEEPALIVE_INTERVAL_MS);
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

    // Only send when the state differs from what tado last accepted. (Opening
    // the serial monitor resets the chip, so don't send on every power-on.)
    WindowState sent = Storage::loadSentState();
    LOG("state: last applied in tado: %s", stateName(sent));

    bool usedNetwork = false;
    while (true) {
        WindowState state = waitForStableReed();

        if (state != sent) {
            usedNetwork = true;
            blinkOnce();  // change detected; WiFi + request take a few seconds
            if (applyState(state)) {
                Storage::saveSentState(state);
                sent = state;
                confirmBlink();
            } else {
                // Not saved: the next wake (reed change or keepalive) retries.
                errorBlink();
            }
        } else {
            LOG("state: %s already applied, nothing to send", stateName(state));
        }

        if (readReed() == state) break;
        LOG("reed: changed while busy, handling again");
    }

    if (wokeFromTimer && !usedNetwork) keepalive();

    sleepUntilReedChange();
}

void loop() {}

#elif APP_MODE == APP_MODE_HW_TEST

// ---- Hardware test: button toggles the LED, reed switch changes are logged.

constexpr uint32_t DEBOUNCE_MS = 30;

bool ledState = false;
int buttonStable = HIGH;  // debounced level (HIGH = released)
int buttonLastRead = HIGH;
uint32_t buttonChangedAt = 0;
int reedLast = -1;

void setup() {
    Serial.begin(115200);
    delay(500);
    consolePrintf("Hardware test: press the button to toggle the LED,");
    consolePrintf("move a magnet near the reed switch to see its state.");

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BUTTON, INPUT);       // external pull-up
    pinMode(PIN_REED, INPUT_PULLUP);  // reed switch to GND
}

void loop() {
    // Button: toggle on each debounced press (HIGH -> LOW).
    int reading = digitalRead(PIN_BUTTON);
    if (reading != buttonLastRead) {
        buttonLastRead = reading;
        buttonChangedAt = millis();
    }
    if (reading != buttonStable && millis() - buttonChangedAt >= DEBOUNCE_MS) {
        buttonStable = reading;
        if (buttonStable == LOW) {
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? HIGH : LOW);
            consolePrintf("Button pressed -> LED %s", ledState ? "ON" : "OFF");
        }
    }

    // Reed switch: LOW = contact closed (magnet near).
    int reed = digitalRead(PIN_REED);
    if (reed != reedLast) {
        reedLast = reed;
        consolePrintf("Reed switch: %s", reed == LOW ? "closed (magnet near)" : "open (no magnet)");
    }

    delay(5);
}

#endif  // APP_MODE
