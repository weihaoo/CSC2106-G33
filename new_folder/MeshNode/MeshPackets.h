#ifndef MESH_PACKETS_H
#define MESH_PACKETS_H

#include <Arduino.h>

// ============================================
// PACKET TYPES
// ============================================
#define PKT_BEACON      0x01  // Neighbor discovery & gradient updates
#define PKT_DATA        0x02  // Sensor data
#define PKT_ACK         0x03  // Acknowledgment (optional for now)

// ============================================
// NODE TYPES
// ============================================
#define NODE_TYPE_REGULAR  0x01
#define NODE_TYPE_SINK     0x02

// ============================================
// NETWORK CONSTANTS
// ============================================
#define MAX_HOPS           8
#define INVALID_DISTANCE   255
#define BROADCAST_ADDR     0xFF

// ============================================
// PACKET STRUCTURES
// ============================================

// Common Header - ALL packets start with this
struct PacketHeader {
  uint8_t packetType;    // PKT_BEACON, PKT_DATA, PKT_ACK
  uint8_t srcId;         // Source node ID
  uint8_t destId;        // Destination (0xFF = broadcast)
  uint8_t hopCount;      // Number of hops so far
  uint16_t packetId;     // Unique packet identifier
  uint8_t checksum;      // Simple XOR checksum
} __attribute__((packed));

// Beacon Packet - For gradient routing
struct BeaconPacket {
  PacketHeader header;
  uint8_t nodeType;      // NODE_TYPE_REGULAR or NODE_TYPE_SINK
  uint8_t distanceToSink; // Hops to nearest sink (0 for sink nodes)
  int16_t rssi;          // Signal strength (filled by receiver)
  uint32_t timestamp;    // Millis when sent
} __attribute__((packed));

// Data Packet - Sensor readings
struct DataPacket {
  PacketHeader header;
  uint8_t sinkId;        // Which sink to route toward (0xFF = any sink)
  float sensorValue;     // Temperature reading
  uint32_t timestamp;    // When reading was taken
  uint8_t batteryLevel;  // Optional: battery percentage
} __attribute__((packed));

// ACK Packet - Optional reliability
struct AckPacket {
  PacketHeader header;
  uint16_t ackedPacketId; // Which packet we're acknowledging
} __attribute__((packed));

// ============================================
// HELPER FUNCTIONS
// ============================================

// Calculate simple XOR checksum
uint8_t calculateChecksum(uint8_t* data, uint8_t len) {
  uint8_t checksum = 0;
  // XOR all bytes except the last one (which will hold the checksum)
  for (uint8_t i = 0; i < len - 1; i++) {
    checksum ^= data[i];
  }
  return checksum;
}

// Verify checksum
bool verifyChecksum(uint8_t* data, uint8_t len) {
  uint8_t calculated = calculateChecksum(data, len);
  uint8_t received = data[len - 1];  // Checksum is last byte
  return (calculated == received);
}

// Print packet header for debugging
void printPacketHeader(PacketHeader* header) {
  Serial.print("Type:");
  switch(header->packetType) {
    case PKT_BEACON: Serial.print("BEACON"); break;
    case PKT_DATA:   Serial.print("DATA"); break;
    case PKT_ACK:    Serial.print("ACK"); break;
    default:         Serial.print("UNKNOWN"); break;
  }
  Serial.print(" Src:");
  Serial.print(header->srcId);
  Serial.print(" Dst:");
  Serial.print(header->destId);
  Serial.print(" Hops:");
  Serial.print(header->hopCount);
  Serial.print(" PktID:");
  Serial.print(header->packetId);
}

#endif