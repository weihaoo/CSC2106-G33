// ════════════════════════════════════════════════════════════════════════════
// EDGE NODE CONFIGURATION
// CSC2106 Group 33 - Edge Node (Person 3)
// ════════════════════════════════════════════════════════════════════════════

#ifndef CONFIG_H
#define CONFIG_H

// ════════════════════════════════════════════════════════════════════════════
// NODE IDENTITY - CHANGE THIS FOR EACH EDGE NODE!
// ════════════════════════════════════════════════════════════════════════════

// For Edge 1 (primary): use 0x01
// For Edge 2 (standby): use 0x06
#define NODE_ID 0x01

// ════════════════════════════════════════════════════════════════════════════
// LORAWAN OTAA CREDENTIALS (from Person 4 / TTN Console)
// ════════════════════════════════════════════════════════════════════════════

// IMPORTANT: Get these from Person 4 after they set up TTN!
//
// DevEUI: LSB byte order (reversed from TTN console)
// Example: TTN shows 70B3D57ED0012345 → enter as {0x45, 0x23, 0x01, ...}
//
// JoinEUI: Usually all zeros for TTN
//
// AppKey: MSB byte order (same as TTN console shows)
// Example: TTN shows 0123456789ABCDEF... → enter as {0x01, 0x23, 0x45, ...}

// Edge 1 credentials (replace with actual values from TTN)
#if NODE_ID == 0x01
    #define DEV_EUI  0x0000000000000000ULL  // TODO: Replace with actual DevEUI (LSB)
    #define JOIN_EUI 0x0000000000000000ULL  // Usually all zeros for TTN
    uint8_t APP_KEY[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };  // TODO: Replace with actual AppKey from TTN (MSB order)

// Edge 2 credentials (replace with actual values from TTN)
#elif NODE_ID == 0x06
    #define DEV_EUI  0x0000000000000000ULL  // TODO: Replace with actual DevEUI (LSB)
    #define JOIN_EUI 0x0000000000000000ULL  // Usually all zeros for TTN
    uint8_t APP_KEY[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };  // TODO: Replace with actual AppKey from TTN (MSB order)
#else
    #error "Invalid NODE_ID! Must be 0x01 (Edge 1) or 0x06 (Edge 2)"
#endif

// ════════════════════════════════════════════════════════════════════════════
// T-BEAM HARDWARE PIN DEFINITIONS (SX1262 Radio)
// ════════════════════════════════════════════════════════════════════════════

#define RADIO_NSS   18
#define RADIO_RST   23
#define RADIO_DIO1  33
#define RADIO_BUSY  32
#define RADIO_SCK   5
#define RADIO_MISO  19
#define RADIO_MOSI  27

// PMU (AXP2101) I2C pins
#define PMU_SDA     21
#define PMU_SCL     22

// ════════════════════════════════════════════════════════════════════════════
// LORA MESH PARAMETERS (AS923 for Singapore)
// ════════════════════════════════════════════════════════════════════════════

#define LORA_FREQUENCY      923.0       // MHz (AS923)
#define LORA_SPREADING      7           // SF7 (fastest, good for lab)
#define LORA_BANDWIDTH      125.0       // kHz
#define LORA_CODING_RATE    5           // 4/5
#define LORA_TX_POWER       17          // dBm
#define LORA_SYNC_WORD      0x12        // Private network

// ════════════════════════════════════════════════════════════════════════════
// MESH PROTOCOL PARAMETERS
// ════════════════════════════════════════════════════════════════════════════

#define MY_RANK             0           // Edge nodes are always rank 0
#define BEACON_INTERVAL     10000       // 10 seconds
#define ACK_TIMEOUT         600         // 600ms to wait for ACK
#define DEDUP_WINDOW        30000       // 30s deduplication window
#define DEDUP_TABLE_SIZE    16          // Max recent packets to track

// ════════════════════════════════════════════════════════════════════════════
// AGGREGATION & LORAWAN UPLINK PARAMETERS
// ════════════════════════════════════════════════════════════════════════════

#define MAX_AGG_RECORDS     7           // Max 7 sensor readings per uplink
#define AGG_FLUSH_TIMEOUT   240000      // 240s = 4 minutes
#define LORAWAN_FPORT       1           // TTN uplink port

// ════════════════════════════════════════════════════════════════════════════
// BEACON PHASE OFFSET (prevents all nodes from beaconing simultaneously)
// ════════════════════════════════════════════════════════════════════════════

#define BEACON_PHASE_OFFSET ((NODE_ID % 5) * 2000)  // Spread across 10s

#endif // CONFIG_H
