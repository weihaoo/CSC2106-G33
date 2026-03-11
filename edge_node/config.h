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
#define NODE_ID 0x06

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

// Edge 1 credentials (TODO: register Edge 1 on TTN and fill in)
#if NODE_ID == 0x01
    #define DEV_EUI  0x0000000000000000ULL  // TODO: Replace with Edge 1 DevEUI (LSB order)
    #define JOIN_EUI 0x0000000000000000ULL
    uint8_t APP_KEY[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };  // TODO: Replace with Edge 1 AppKey from TTN (MSB order)

// Edge 2 credentials — TTN DevEUI: 70B3D57ED007639E (AS923, au1 cluster)
#elif NODE_ID == 0x06
    // DevEUI reversed to LSB for RadioLib (TTN shows MSB: 70B3D57ED007639E)
    #define DEV_EUI  0x70B3D57ED007639EULL
    #define JOIN_EUI 0x0000000000000000ULL
    uint8_t APP_KEY[] = {
        0xE5, 0xEA, 0x56, 0x94, 0x39, 0x88, 0x4F, 0x65,
        0x68, 0xF4, 0xFA, 0xB3, 0x88, 0x97, 0x21, 0x66
    };  // AppKey MSB — copied exactly as shown in TTN console
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
// LORA MESH PARAMETERS
// Radio constants (LORA_FREQUENCY, LORA_SPREADING, etc.) are defined in
// mesh_protocol.h which is shared across all nodes.
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
// MESH PROTOCOL PARAMETERS
// Timing constants (BEACON_INTERVAL_MS, DEDUP_WINDOW_MS, etc.) are defined
// in mesh_protocol.h which is shared across all nodes.
// ════════════════════════════════════════════════════════════════════════════

#define MY_RANK             0           // Edge nodes are always rank 0 (RANK_EDGE in mesh_protocol.h)

// ════════════════════════════════════════════════════════════════════════════
// LORAWAN UPLINK PARAMETERS (edge-node specific)
// MAX_AGG_RECORDS and AGG_FLUSH_TIMEOUT_MS are in mesh_protocol.h
// ════════════════════════════════════════════════════════════════════════════

#define LORAWAN_FPORT       1           // TTN uplink port

// ════════════════════════════════════════════════════════════════════════════
// BEACON PHASE OFFSET (prevents all nodes from beaconing simultaneously)
// ════════════════════════════════════════════════════════════════════════════

#define BEACON_PHASE_OFFSET ((NODE_ID % 5) * 2000)  // Spread across 10s

#endif // CONFIG_H
