// ════════════════════════════════════════════════════════════════════════════
// LOGGING MODULE - CSC2106 Group 33
// Clear, team-friendly logging for LoRa Mesh Network
// ════════════════════════════════════════════════════════════════════════════
//
// Usage:
//   1. Define NODE_NAME before including this file:
//      #define NODE_NAME "RELAY-02"
//   2. Include this file:
//      #include "../shared/logging.h"
//   3. Use logging macros:
//      LOG_INFO("Initializing...");
//      LOG_OK("Radio ready");
//
// ════════════════════════════════════════════════════════════════════════════

#ifndef LOGGING_H
#define LOGGING_H

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════════════
// LOG LEVELS
// ════════════════════════════════════════════════════════════════════════════

#define LOG_LEVEL_DEBUG  0
#define LOG_LEVEL_INFO   1
#define LOG_LEVEL_WARN   2
#define LOG_LEVEL_ERROR  3

// Set minimum log level (change for production vs debug)
#ifndef MIN_LOG_LEVEL
  #define MIN_LOG_LEVEL LOG_LEVEL_INFO
#endif

// ════════════════════════════════════════════════════════════════════════════
// CORE LOGGING FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

inline void log_timestamp() {
    float ts = millis() / 1000.0;
    Serial.print("[");
    if (ts < 100) Serial.print("0");
    if (ts < 10) Serial.print("0");
    Serial.print(ts, 1);
    Serial.print("s]");
}

inline void log_prefix(const char* level) {
    log_timestamp();
    Serial.print(" [");
    Serial.print(NODE_NAME);
    Serial.print("] [");
    Serial.print(level);
    Serial.print("] ");
}

// ════════════════════════════════════════════════════════════════════════════
// LOG MACROS
// ════════════════════════════════════════════════════════════════════════════

#define LOG_INFO(msg)   do { log_prefix("INFO  "); Serial.println(msg); } while(0)
#define LOG_OK(msg)     do { log_prefix("OK    "); Serial.println(msg); } while(0)
#define LOG_WARN(msg)   do { log_prefix("WARN  "); Serial.println(msg); } while(0)
#define LOG_ERROR(msg)  do { log_prefix("ERROR "); Serial.println(msg); } while(0)
#define LOG_DEBUG(msg)  do { if (MIN_LOG_LEVEL <= LOG_LEVEL_DEBUG) { log_prefix("DEBUG "); Serial.println(msg); } } while(0)

// Action-specific logs
#define LOG_TX(msg)     do { log_prefix("TX    "); Serial.println(msg); } while(0)
#define LOG_RX(msg)     do { log_prefix("RX    "); Serial.println(msg); } while(0)
#define LOG_ACK(msg)    do { log_prefix("ACK   "); Serial.println(msg); } while(0)
#define LOG_FWD(msg)    do { log_prefix("FWD   "); Serial.println(msg); } while(0)
#define LOG_DROP(msg)   do { log_prefix("DROP  "); Serial.println(msg); } while(0)
#define LOG_BEACON(msg) do { log_prefix("BEACON"); Serial.println(msg); } while(0)
#define LOG_PARENT(msg) do { log_prefix("PARENT"); Serial.println(msg); } while(0)
#define LOG_DAG(msg)    do { log_prefix("DAG   "); Serial.println(msg); } while(0)
#define LOG_AGG(msg)    do { log_prefix("AGG   "); Serial.println(msg); } while(0)
#define LOG_UPLINK(msg) do { log_prefix("UPLINK"); Serial.println(msg); } while(0)

// ════════════════════════════════════════════════════════════════════════════
// VISUAL SEPARATORS
// ════════════════════════════════════════════════════════════════════════════

inline void log_separator_major() {
    Serial.println("════════════════════════════════════════════════════════════════════════");
}

inline void log_separator_minor() {
    Serial.println("────────────────────────────────────────────────────────────────────────");
}

inline void log_separator_packet() {
    Serial.println("┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄");
}

// Indented detail line (for multi-line logs)
inline void log_detail(const char* msg) {
    Serial.print("                                    ");  // Align with message start
    Serial.println(msg);
}

// ════════════════════════════════════════════════════════════════════════════
// NODE NAME HELPER
// Converts node ID to human-readable name
// ════════════════════════════════════════════════════════════════════════════

inline const char* node_name(uint8_t id) {
    switch(id) {
        case 0x01: return "EDGE-01";
        case 0x02: return "RELAY-02";
        case 0x03: return "SENSOR-03";
        case 0x04: return "SENSOR-04";
        case 0x06: return "EDGE-06";
        case 0xFF: return "BROADCAST";
        default:   return "UNKNOWN";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// BOOT BANNER
// ════════════════════════════════════════════════════════════════════════════

inline void log_boot_banner(const char* role) {
    Serial.println();
    log_separator_major();
    Serial.print("  ");
    Serial.print(NODE_NAME);
    Serial.print(" | ");
    Serial.print(role);
    Serial.println(" | CSC2106 G33 LoRa Mesh");
    log_separator_major();
    Serial.println();
}

#endif // LOGGING_H
