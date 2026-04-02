// ════════════════════════════════════════════════════════════════════════════
// NTP TIME SYNC HELPER — SHARED
// CSC2106 Group 33 — LoRa Mesh Network
//
// Provides Wi-Fi connection + NTP synchronisation for ALL nodes.
// Used by sensor nodes (to timestamp outgoing packets) and by the edge node
// (to compute one-way latency when a packet arrives).
//
// Usage:
//   1. Define WIFI_SSID and WIFI_PASSWORD before including this file.
//   2. Call init_wifi_ntp() once in setup().
//   3. Call get_ntp_epoch_ms() to get a uint64_t Unix timestamp in ms,
//      or get_ntp_epoch_s() for a uint32_t Unix timestamp in seconds.
//
// NTP accuracy on ESP32: ±10–50 ms — adequate for LoRa latency demo
// (LoRa air-time at SF7/125kHz >> NTP error).
//
// NOTE: This file uses the Arduino WiFi + configTime() APIs, which are
// built into the ESP32 Arduino core — no extra library needed.
// ════════════════════════════════════════════════════════════════════════════

#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

// ── NTP servers ──────────────────────────────────────────────────────────────
// Uses pool.ntp.org with Singapore timezone (UTC+8, no DST).
#define NTP_SERVER1   "pool.ntp.org"
#define NTP_SERVER2   "time.google.com"
#define NTP_GMT_OFFSET_SEC    28800   // UTC+8 (Singapore)
#define NTP_DAYLIGHT_OFFSET   0       // No DST in Singapore

// ── State ────────────────────────────────────────────────────────────────────
static bool  _ntp_synced    = false;
static uint32_t _sync_millis = 0;   // millis() at the moment NTP was confirmed
static time_t   _sync_epoch  = 0;   // Unix epoch (seconds) at that millis()

// ─────────────────────────────────────────────────────────────────────────────
// init_wifi_ntp()
//
// Connects to Wi-Fi and waits for NTP sync. Call once in setup().
// Blocks until Wi-Fi + NTP are both ready (max ~15s total).
// Returns true if NTP was synced successfully, false on timeout.
// ─────────────────────────────────────────────────────────────────────────────
inline bool init_wifi_ntp(const char *ssid, const char *password) {
    Serial.println(F("[NTP] Connecting to Wi-Fi..."));

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    // Wait up to 10s for Wi-Fi connection
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 10000UL) {
            Serial.println(F("[NTP] Wi-Fi timeout! Latency timestamps unavailable."));
            return false;
        }
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    Serial.print(F("[NTP] Wi-Fi connected | IP: "));
    Serial.println(WiFi.localIP());

    // Start NTP sync (non-blocking — configTime registers SNTP callbacks)
    configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET, NTP_SERVER1, NTP_SERVER2);

    // Wait up to 8s for the first valid NTP response
    Serial.print(F("[NTP] Waiting for NTP sync"));
    t0 = millis();
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo)) {
        if (millis() - t0 > 8000UL) {
            Serial.println(F("\n[NTP] NTP timeout! Latency timestamps unavailable."));
            return false;
        }
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    // Record the sync anchor: wall-clock epoch at this exact millis() value.
    // All subsequent calls use this anchor + millis() drift.
    _sync_epoch  = mktime(&timeinfo);  // seconds since Unix epoch
    _sync_millis = millis();
    _ntp_synced  = true;

    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.print(F("[NTP] Synced! Time: "));
    Serial.println(tbuf);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ntp_is_synced()
// Returns true if NTP was successfully synchronised.
// ─────────────────────────────────────────────────────────────────────────────
inline bool ntp_is_synced() {
    return _ntp_synced;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_ntp_epoch_s()
// Returns current Unix time in whole SECONDS (uint32_t).
// Valid until year 2106. Returns 0 if NTP not synced.
// ─────────────────────────────────────────────────────────────────────────────
inline uint32_t get_ntp_epoch_s() {
    if (!_ntp_synced) return 0;
    uint32_t elapsed_s = (millis() - _sync_millis) / 1000UL;
    return (uint32_t)(_sync_epoch + elapsed_s);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_ntp_epoch_ms()
// Returns current Unix time in MILLISECONDS (uint64_t).
// Returns 0 if NTP not synced.
// ─────────────────────────────────────────────────────────────────────────────
inline uint64_t get_ntp_epoch_ms() {
    if (!_ntp_synced) return 0;
    uint32_t elapsed_ms = millis() - _sync_millis;
    return ((uint64_t)_sync_epoch * 1000ULL) + elapsed_ms;
}

#endif // NTP_TIME_H
