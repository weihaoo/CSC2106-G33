// ════════════════════════════════════════════════════════════════════════════
// MESH PROTOCOL HEADER
// Common definitions for all nodes in the LoRa mesh network
// ════════════════════════════════════════════════════════════════════════════

#ifndef MESH_PROTOCOL_H
#define MESH_PROTOCOL_H

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

#define LORA_FREQUENCY      915E6   // 915 MHz (change to 868E6 for EU)
#define LORA_SPREADING      7       // SF7
#define LORA_BANDWIDTH      125E3   // 125 kHz
#define LORA_TX_POWER       17      // dBm

#define MAX_NEIGHBORS       8
#define MAX_PENDING_ACKS    4
#define MAX_SEEN_PACKETS    16

#define BEACON_INTERVAL     10000   // 10 seconds
#define SENSOR_INTERVAL     15000   // 15 seconds
#define ACK_TIMEOUT         1000    // 1 second
#define PARENT_TIMEOUT      35000   // 35 seconds (miss 3 beacons)
#define MAX_RETRIES         3
#define DEFAULT_TTL         10

// Parent selection weights (total = 100)
#define RANK_WEIGHT         60
#define RSSI_WEIGHT         40
#define RSSI_HYSTERESIS     10      // dB improvement needed to switch

// ════════════════════════════════════════════════════════════════════════════
// PACKET TYPES
// ════════════════════════════════════════════════════════════════════════════

#define PKT_TYPE_DATA       0x00    // Sensor data
#define PKT_TYPE_ACK        0x20    // Acknowledgment
#define PKT_TYPE_BEACON     0x40    // Neighbor discovery
#define PKT_TYPE_RELAY      0x60    // LoRaWAN relay (version-agnostic)

#define PKT_FLAG_ACK_REQ    0x10    // Request ACK
#define PKT_FLAG_IS_FWD     0x08    // Is forwarded (not original)

// ════════════════════════════════════════════════════════════════════════════
// NODE ROLES
// ════════════════════════════════════════════════════════════════════════════

#define ROLE_SENSOR         0       // Mesh sensor node
#define ROLE_RELAY          1       // Mesh relay node
#define ROLE_SINK           2       // Edge node / sink

// ════════════════════════════════════════════════════════════════════════════
// NODE STATES
// ════════════════════════════════════════════════════════════════════════════

#define STATE_ORPHAN        0       // No parent, rank = 255
#define STATE_DISCOVERING   1       // Heard beacon, selecting parent
#define STATE_CONNECTED     2       // Has parent, can route
#define STATE_SWITCHING     3       // Parent failed, finding new

// ════════════════════════════════════════════════════════════════════════════
// MESH HEADER (8 bytes)
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t flags;          // [Type:3][ACK_REQ:1][IS_FWD:1][Reserved:3]
    uint8_t src_id;         // Original source node
    uint8_t dst_id;         // Destination (0x00 = any sink)
    uint8_t prev_hop;       // Previous hop node
    uint8_t ttl;            // Time to live
    uint8_t seq_num;        // Sequence number
    uint8_t rank;           // Sender's rank
    uint8_t payload_len;    // Payload length
} MeshHeader;

// ════════════════════════════════════════════════════════════════════════════
// NEIGHBOR TABLE ENTRY (8 bytes - optimized for Arduino)
// ════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t node_id;        // Neighbor node ID (0 = empty slot)
    uint8_t rank;           // Neighbor's rank
    int8_t rssi;            // Last RSSI
    uint8_t fail_count;     // Consecutive failures
    uint32_t last_seen;     // Timestamp of last beacon (millis)
} Neighbor;

// ════════════════════════════════════════════════════════════════════════════
// PENDING ACK ENTRY
// ════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t seq_num;        // Sequence number waiting for ACK
    uint8_t dest_id;        // Who should ACK
    uint8_t retries;        // Retry count
    uint32_t sent_time;     // When packet was sent
    uint8_t payload[32];    // Payload backup for retry
    uint8_t payload_len;    // Payload length
    bool active;            // Slot in use
} PendingAck;

// ════════════════════════════════════════════════════════════════════════════
// SEEN PACKET (for deduplication)
// ════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t src_id;
    uint8_t seq_num;
    uint32_t timestamp;
} SeenPacket;

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

inline uint8_t get_packet_type(uint8_t flags) {
    return flags & 0xE0;
}

inline bool is_ack_requested(uint8_t flags) {
    return (flags & PKT_FLAG_ACK_REQ) != 0;
}

inline bool is_forwarded(uint8_t flags) {
    return (flags & PKT_FLAG_IS_FWD) != 0;
}

#endif // MESH_PROTOCOL_H
