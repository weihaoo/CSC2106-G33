// ════════════════════════════════════════════════════════════════════════════
// MESH PROTOCOL DEFINITIONS — CANONICAL (all nodes share this file)
// CSC2106 Group 33 — LoRa Mesh Network
//
// Copy this file verbatim into sensor_node/, relay_node/, and edge_node/.
// Do NOT add node-specific constants here — those belong in each .ino or
// the edge node's config.h.
// ════════════════════════════════════════════════════════════════════════════

#ifndef MESH_PROTOCOL_H
#define MESH_PROTOCOL_H

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════════════
// LORA RADIO PARAMETERS (AS923, Singapore — all nodes must match exactly)
// ════════════════════════════════════════════════════════════════════════════

#define LORA_FREQUENCY      923.0   // MHz (AS923 for Singapore)
#define LORA_SPREADING      7       // SF7
#define LORA_BANDWIDTH      125.0   // kHz
#define LORA_CODING_RATE    5       // 4/5
#define LORA_SYNC_WORD      0x12    // Private network sync word
#define LORA_TX_POWER       17      // dBm

// ════════════════════════════════════════════════════════════════════════════
// PROTOCOL TIMING CONSTANTS (shared across all nodes)
// ════════════════════════════════════════════════════════════════════════════

#define BEACON_INTERVAL_MS       10000UL   // How often nodes broadcast beacons
#define ACK_TIMEOUT_MS           600       // How long to wait for a hop ACK
#define PARENT_TIMEOUT_MS        35000UL   // Declare parent dead after this (3+ missed beacons)
#define MAX_RETRIES              3         // Max TX attempts per packet
#define DEFAULT_TTL              10        // Starting TTL (max hops)
#define DEDUP_WINDOW_MS          30000UL   // Ignore duplicate packets within this window
#define DEDUP_TABLE_SIZE         16        // Max recent packets tracked for dedup
#define MAX_CANDIDATES           4         // Max parent candidates tracked by sensor/relay
#define PARENT_SWITCH_HYSTERESIS 8         // Min score improvement needed to switch parent

// Edge node aggregation
#define MAX_AGG_RECORDS          7         // Max sensor readings per LoRaWAN uplink
#define AGG_FLUSH_TIMEOUT_MS     240000UL  // Flush to TTN after 4 minutes regardless

// ════════════════════════════════════════════════════════════════════════════
// NODE RANK VALUES
// Rank 0 = closest to TTN (edge), higher = further away
// ════════════════════════════════════════════════════════════════════════════

#define RANK_EDGE       0   // Edge node (sink, LoRaWAN gateway)
#define RANK_RELAY      1   // Relay node (payload-agnostic forwarder)
#define RANK_SENSOR     2   // Sensor node (data originator)

// ════════════════════════════════════════════════════════════════════════════
// PACKET TYPE FLAGS (stored in flags byte, bits 7–5)
// ════════════════════════════════════════════════════════════════════════════

#define PKT_TYPE_DATA       0x00    // Sensor data packet
#define PKT_TYPE_BEACON     0x20    // Beacon packet (parent discovery)
#define PKT_TYPE_ACK        0x40    // Acknowledgment packet
#define PKT_TYPE_RESERVED   0x60    // Reserved

#define PKT_FLAG_ACK_REQ    0x10    // Bit 4: ACK requested by sender
#define PKT_FLAG_FWD        0x08    // Bit 3: Packet has been forwarded (relay set this)

// Helper macros
#define GET_PKT_TYPE(flags)      ((flags) & 0xE0)
#define IS_ACK_REQUESTED(flags)  (((flags) & PKT_FLAG_ACK_REQ) != 0)
#define IS_FORWARDED(flags)      (((flags) & PKT_FLAG_FWD) != 0)

// ════════════════════════════════════════════════════════════════════════════
// MESH HEADER — 10 BYTES
//
// Byte layout:
//   [0] flags       — packet type (bits 7–5) + flags (bits 4–0)
//   [1] src_id      — original sender node ID
//   [2] dst_id      — destination node ID (0xFF = broadcast)
//   [3] prev_hop    — node ID of the last hop that touched this packet
//   [4] ttl         — time to live, decremented each hop, drop at 0
//   [5] seq_num     — sequence number from original sender (for dedup)
//   [6] rank        — sender's rank (0=edge, 1=relay, 2=sensor)
//   [7] payload_len — bytes following this header
//   [8] crc16[0]    — CRC16 high byte (big-endian, covers bytes 0–7)
//   [9] crc16[1]    — CRC16 low byte
//
// IMPORTANT: crc16 is stored as uint8_t[2] in big-endian order to ensure
// identical byte layout on all platforms regardless of CPU endianness.
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t flags;
    uint8_t src_id;
    uint8_t dst_id;
    uint8_t prev_hop;
    uint8_t ttl;
    uint8_t seq_num;
    uint8_t rank;
    uint8_t payload_len;
    uint8_t crc16[2];   // [0]=high byte, [1]=low byte (explicit big-endian)
} MeshHeader;

#define MESH_HEADER_SIZE 10

// ════════════════════════════════════════════════════════════════════════════
// SENSOR PAYLOAD — 7 BYTES (DHT22 temperature + humidity reading)
//
// Byte layout:
//   [0] schema_version — 0x01
//   [1] sensor_type    — 0x03 = DHT22
//   [2–3] temp_c_x10   — temperature × 10, signed int16, big-endian
//                         (e.g. 253 = 25.3°C, -12 = -1.2°C)
//   [4–5] humidity_x10 — humidity × 10, unsigned int16, big-endian
//                         (e.g. 655 = 65.5%)
//   [6] status         — 0x00 = OK, 0x01 = read error (discard this reading)
//
// NOTE: Use manual byte packing (not struct cast) to guarantee big-endian
// layout on any platform.
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t  schema_version;  // 0x01
    uint8_t  sensor_type;     // 0x03 = DHT22
    int16_t  temp_c_x10;      // Temperature × 10 (signed)
    uint16_t humidity_x10;    // Humidity × 10 (unsigned)
    uint8_t  status;          // 0x00 = OK, 0x01 = read error
} SensorPayload;

#define SENSOR_PAYLOAD_SIZE 7
#define SENSOR_TYPE_DHT22   0x03

// ════════════════════════════════════════════════════════════════════════════
// BEACON PAYLOAD — 4 BYTES
//
// Byte layout:
//   [0] schema_version — 0x01
//   [1] queue_pct      — TX queue occupancy 0–100%
//   [2] link_quality   — RSSI-derived uplink quality 0–100
//   [3] parent_health  — ACK success rate to parent 0–100%
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t schema_version;
    uint8_t queue_pct;
    uint8_t link_quality;
    uint8_t parent_health;
} BeaconPayload;

#define BEACON_PAYLOAD_SIZE 4

// ════════════════════════════════════════════════════════════════════════════
// BRIDGE AGGREGATION V1 FORMAT (edge node → TTN via LoRaWAN)
//
// Header (3 bytes):
//   [0] schema_version — 0x02 = BridgeAggV1
//   [1] bridge_id      — edge node ID (0x01 or 0x06)
//   [2] record_count   — number of records following (max 7)
//
// Each record (14 bytes):
//   [0]   mesh_src_id     — original sensor node ID
//   [1]   mesh_seq        — packet sequence number from sensor
//   [2]   sensor_type_hint— 0x03 = DHT22
//   [3]   hop_estimate    — estimated hops (DEFAULT_TTL - received_ttl)
//   [4–5] edge_uptime_s   — edge node uptime when received (big-endian uint16)
//   [6]   opaque_len      — payload length (always 7 for DHT22)
//   [7–13] opaque_payload — raw 7-byte SensorPayload, copied as-is
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t schema_version;
    uint8_t bridge_id;
    uint8_t record_count;
} BridgeAggHeader;

typedef struct __attribute__((packed)) {
    uint8_t  mesh_src_id;
    uint8_t  mesh_seq;
    uint8_t  sensor_type_hint;
    uint8_t  hop_estimate;
    uint16_t edge_uptime_s;
    uint8_t  opaque_len;
    uint8_t  opaque_payload[7];
} BridgeRecord;

#define BRIDGE_RECORD_SIZE   14
#define BRIDGE_AGG_SCHEMA_V1 0x02

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION ENTRY (used by relay and edge)
// ════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t  src_id;
    uint8_t  seq_num;
    uint32_t timestamp;  // millis() when first seen
    bool     valid;      // slot is in use
} DedupEntry;

// ════════════════════════════════════════════════════════════════════════════
// CRC16-CCITT (polynomial 0x1021, initial value 0xFFFF)
// Computed over the first `len` bytes of `data`.
// ════════════════════════════════════════════════════════════════════════════

inline uint16_t crc16_ccitt(const uint8_t* data, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// Compute CRC16 over bytes 0–7 and write it big-endian into hdr->crc16[0..1].
// Call this after filling all other header fields.
inline void set_mesh_crc(MeshHeader* hdr) {
    uint16_t crc = crc16_ccitt((const uint8_t*)hdr, 8);
    hdr->crc16[0] = (crc >> 8) & 0xFF;  // high byte first
    hdr->crc16[1] = crc & 0xFF;          // low byte second
}

// Validate MeshHeader CRC. Reads the stored big-endian value and compares to
// freshly computed CRC. Returns true if the packet is intact.
inline bool validate_mesh_crc(const MeshHeader* hdr) {
    uint16_t computed = crc16_ccitt((const uint8_t*)hdr, 8);
    uint16_t stored   = ((uint16_t)hdr->crc16[0] << 8) | hdr->crc16[1];
    return (computed == stored);
}

#endif // MESH_PROTOCOL_H
