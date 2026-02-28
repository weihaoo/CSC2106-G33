// ════════════════════════════════════════════════════════════════════════════
// MESH PROTOCOL DEFINITIONS
// CSC2106 Group 33 - Common header for all nodes
// Based on IMPLEMENTATION_PLAN_SIMPLE.md
// ════════════════════════════════════════════════════════════════════════════

#ifndef MESH_PROTOCOL_H
#define MESH_PROTOCOL_H

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════════════
// PACKET TYPE FLAGS (stored in flags byte, bits 7-5)
// ════════════════════════════════════════════════════════════════════════════

#define PKT_TYPE_DATA       0x00    // Sensor data packet
#define PKT_TYPE_BEACON     0x20    // Beacon packet (neighbor discovery)
#define PKT_TYPE_ACK        0x40    // Acknowledgment packet
#define PKT_TYPE_RESERVED   0x60    // Reserved for future use

#define PKT_FLAG_ACK_REQ    0x10    // Bit 4: ACK requested
#define PKT_FLAG_FWD        0x08    // Bit 3: Packet has been forwarded

// Helper macros
#define GET_PKT_TYPE(flags)    ((flags) & 0xE0)
#define IS_ACK_REQUESTED(flags) (((flags) & PKT_FLAG_ACK_REQ) != 0)
#define IS_FORWARDED(flags)     (((flags) & PKT_FLAG_FWD) != 0)

// ════════════════════════════════════════════════════════════════════════════
// MESH HEADER - 10 BYTES (as per IMPLEMENTATION_PLAN_SIMPLE.md lines 178-190)
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t  flags;         // Byte 0: Packet type + flags
    uint8_t  src_id;        // Byte 1: Original sender
    uint8_t  dst_id;        // Byte 2: Destination (0xFF = broadcast)
    uint8_t  prev_hop;      // Byte 3: Last hop that touched this packet
    uint8_t  ttl;           // Byte 4: Time to live (decremented each hop)
    uint8_t  seq_num;       // Byte 5: Sequence number from sender
    uint8_t  rank;          // Byte 6: Sender's rank (edge=0, relay=1, sensor=2)
    uint8_t  payload_len;   // Byte 7: Payload length (bytes after header)
    uint16_t crc16;         // Bytes 8-9: CRC16 checksum of bytes 0-7
} MeshHeader;

#define MESH_HEADER_SIZE 10

// ════════════════════════════════════════════════════════════════════════════
// SENSOR PAYLOAD - 7 BYTES (DHT22 reading)
// As per plan lines 194-205
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t  schema_version;  // 0x01
    uint8_t  sensor_type;     // 0x03 = DHT22
    int16_t  temp_c_x10;      // Temperature × 10 (e.g., 253 = 25.3°C)
    uint16_t humidity_x10;    // Humidity × 10 (e.g., 655 = 65.5%)
    uint8_t  status;          // 0x00 = OK, 0x01 = read error
} SensorPayload;

#define SENSOR_PAYLOAD_SIZE 7
#define SENSOR_TYPE_DHT22   0x03

// ════════════════════════════════════════════════════════════════════════════
// BEACON PAYLOAD - 4 BYTES (after MeshHeader)
// As per plan lines 433-441
// ════════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    uint8_t schema_version;   // 0x01
    uint8_t queue_pct;        // TX queue occupancy 0-100%
    uint8_t link_quality;     // RSSI-based quality 0-100
    uint8_t parent_health;    // ACK success rate 0-100%
} BeaconPayload;

#define BEACON_PAYLOAD_SIZE 4

// ════════════════════════════════════════════════════════════════════════════
// BRIDGE AGGREGATION V1 FORMAT (sent to TTN via LoRaWAN)
// As per plan lines 208-231
// ════════════════════════════════════════════════════════════════════════════

// Header: 3 bytes
typedef struct __attribute__((packed)) {
    uint8_t schema_version;   // 0x02 = BridgeAggV1
    uint8_t bridge_id;        // Edge node ID (0x01 or 0x06)
    uint8_t record_count;     // Number of records following (max 7)
} BridgeAggHeader;

// Each record: 14 bytes
typedef struct __attribute__((packed)) {
    uint8_t  mesh_src_id;       // Which sensor sent this
    uint8_t  mesh_seq;          // Packet sequence number
    uint8_t  sensor_type_hint;  // 0x03 = DHT22
    uint8_t  hop_estimate;      // How many hops it took
    uint16_t edge_uptime_s;     // Edge uptime when received (seconds)
    uint8_t  opaque_len;        // Payload length (always 7 for DHT22)
    uint8_t  opaque_payload[7]; // Raw sensor payload, copied as-is
} BridgeRecord;

#define BRIDGE_RECORD_SIZE 14
#define BRIDGE_AGG_SCHEMA_V1 0x02

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION STRUCTURE
// ════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t  src_id;
    uint8_t  seq_num;
    uint32_t timestamp;    // millis() when seen
} DedupEntry;

// ════════════════════════════════════════════════════════════════════════════
// CRC16 CALCULATION (CCITT-FALSE)
// Polynomial: 0x1021, Initial: 0xFFFF
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

// Helper to compute CRC for MeshHeader (bytes 0-7)
inline uint16_t compute_mesh_crc(const MeshHeader* hdr) {
    return crc16_ccitt((const uint8_t*)hdr, 8);
}

// Helper to validate MeshHeader CRC
inline bool validate_mesh_crc(const MeshHeader* hdr) {
    uint16_t computed = compute_mesh_crc(hdr);
    return (computed == hdr->crc16);
}

#endif // MESH_PROTOCOL_H
