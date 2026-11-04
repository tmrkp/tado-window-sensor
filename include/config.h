#pragma once

#include <Arduino.h>

// ---- Hardware pins (BK7231N GPIO numbers; LibreTiny uses them as Arduino pins)

// Reed switch to GND. Must be deep-sleep wake capable (all CBU GPIOs are).
// LOW = contact closed = magnet near = window closed.
#define PIN_REED PIN_P22  // CBU pin 4

// Status LED, active high (GPIO -> resistor -> LED -> GND). PWM4 capable.
#define PIN_LED PIN_P24  // CBU pin 12

// Push button to GND, with external pull-up: reads LOW while pressed.
#define PIN_BUTTON PIN_P16  // CBU pin 2

// ---- Build mode

#define APP_MODE_SENSOR 0   // window sensor application
#define APP_MODE_HW_TEST 1  // button toggles LED, reed changes logged

#define APP_MODE APP_MODE_SENSOR

// 1: log diagnostics to the serial port. 0 for final deployment.
// (The tado login URL is always printed, since it is needed for setup.)
#define DEBUG_LOG 1

// ---- Behaviour

// The reed state must be unchanged this long before a request is sent.
#define REED_STABLE_MS 2000

// LED feedback for a state change:
//   1. one blink as soon as the debounced change is detected (before WiFi)
//   2. one blink, CONFIRM_DELAY_MS after the request succeeded
#define CONFIRM_DELAY_MS 300
#define CONFIRM_BLINK_MS 200

// Failure: blink for this long.
#define ERROR_BLINK_MS 60000
#define ERROR_BLINK_PERIOD_MS 500

// Timer wake to keep the tado refresh token alive (it expires after 30 days
// unused) and to retry a failed request. The BK7231N RTC timer allows at most
// ~36 h per sleep.
#define KEEPALIVE_INTERVAL_MS (24UL * 60 * 60 * 1000)

// Give up on WiFi after this long (scan + DHCP).
#define WIFI_TIMEOUT_MS 20000
// Give up on the fast connect (cached AP + static IP) after this long and
// fall back to scan + DHCP.
#define WIFI_FAST_TIMEOUT_MS 5000
