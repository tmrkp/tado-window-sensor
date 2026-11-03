#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "secrets.h"

const char* TEST_URL = "http://example.com/";

WiFiClient wifiClient;

bool connectWifi() {
    Serial.print("Connecting to ");
    Serial.println(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 30000) {
            Serial.println("WiFi connection timed out");
            return false;
        }
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
    return true;
}

void httpGet() {
    HTTPClient http;
    http.begin(wifiClient, TEST_URL);

    int code = http.GET();
    if (code > 0) {
        Serial.print("HTTP ");
        Serial.println(code);
        Serial.println(http.getStream().readString());
    } else {
        Serial.print("HTTP request failed: ");
        Serial.println(http.errorToString(code));
    }

    http.end();
}

void setup() {
    Serial.begin(115200);
    delay(500);

    while (!Serial) {
        delay(10);
    }

    if (connectWifi()) {
        httpGet();
    }
}

void loop() {
    delay(10000);
}
