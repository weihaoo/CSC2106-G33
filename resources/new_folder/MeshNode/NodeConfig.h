#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "MeshPackets.h"

// ============================================
// NODE CONFIGURATION - CHANGE FOR EACH NODE!
// ============================================
#define MY_NODE_ID      1                    // CHANGE: 1, 2, 3, 4, 5
#define MY_NODE_TYPE    NODE_TYPE_REGULAR    // or NODE_TYPE_SINK

// For Sink Nodes: Which gateway are you connected to?
#define GATEWAY_ID      1                    // Gateway 1 or 2

// ============================================
// HARDWARE CONFIGURATION (Cytron LoRa-RFM Shield)
// ============================================
#define LORA_SS         10   // NSS pin
#define LORA_RST        9    // Reset pin
#define LORA_DIO0       2    // DIO0 pin

// ============================================
// LORA PARAMETERS - Singapore (AS923)
// ============================================
#define LORA_FREQUENCY  923E6    // 923 MHz for Singapore
#define LORA_SF         7        // Spreading Factor (7 = fast, 12 = long range)
#define LORA_BW         125E3    // Bandwidth: 125 kHz
#define LORA_CR         5        // Coding Rate: 4/5
#define LORA_TX_POWER   17       // TX power in dBm (max 20)
#define SYNC_WORD       0x12     // Custom sync word (not LoRaWAN 0x34)

// ============================================
// TIMING PARAMETERS (milliseconds)
// ============================================
#define BEACON_INTERVAL         10000   // Send beacon every 10 seconds
#define DATA_SEND_INTERVAL      30000   // Send sensor data every 30 seconds
#define NEIGHBOR_TIMEOUT        30000   // Forget neighbor after 30 seconds
#define ROUTE_UPDATE_INTERVAL   5000    // Check/update routes every 5 seconds

// ============================================
// COLLISION AVOIDANCE PARAMETERS
// ============================================
#define CSMA_MAX_BACKOFF        100     // Max random backoff (ms)
#define LISTEN_BEFORE_TALK_MS   10      // Listen duration before transmit
#define RSSI_THRESHOLD          -100    // Channel clear if weaker than this

// ============================================
// SENSOR CONFIGURATION
// ============================================
#define TEMP_SENSOR_PIN         A0      // Analog pin for temperature sensor
#define USE_SIMULATED_SENSOR    true    // Set false when real sensor connected

#endif