// ════════════════════════════════════════════════════════════════════════════
// EDGE NODE (SINK) - LoRaWAN End Device
// 
// Role: Mesh sink (rank 0) + LoRaWAN uplink to gateway
// 
// CONFIGURATION:
//   - Device 1: Set MY_NODE_ID to 0x01
//   - Device 2: Set MY_NODE_ID to 0x02
//
// HARDWARE:
//   - ESP32 + LoRa module (e.g., TTGO LoRa32, Heltec WiFi LoRa)
//   - Or Arduino with LoRa (LoRaWAN output via Serial for demo)
//
// PIN CONFIGURATION (adjust for your board):
//   - TTGO LoRa32:    SS=18, RST=14, DIO0=26
//   - Heltec WiFi:    SS=18, RST=14, DIO0=26
//   - Arduino + RFM95: SS=10, RST=9,  DIO0=2
// ════════════════════════════════════════════════════════════════════════════

#include <SPI.h>
#include <LoRa.h>
#include "mesh_protocol.h"

// ════════════════════════════════════════════════════════════════════════════
// NODE CONFIGURATION - CHANGE FOR EACH DEVICE!
// ════════════════════════════════════════════════════════════════════════════

#define MY_NODE_ID      0x01    // Device 1 = 0x01, Device 2 = 0x02
#define MY_ROLE         ROLE_SINK
#define MY_RANK         0       // Sink is always rank 0

// ════════════════════════════════════════════════════════════════════════════
// PIN CONFIGURATION - ADJUST FOR YOUR BOARD!
// ════════════════════════════════════════════════════════════════════════════

// For Arduino Uno + RFM95W
#define LORA_SS         10
#define LORA_RST        9
#define LORA_DIO0       2

// For ESP32 (Radio A on VSPI) - Using conflict-free pins (uncomment if using ESP32)
// #define LORA_SS      5
// #define LORA_RST     25
// #define LORA_DIO0    32

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL STATE
// ════════════════════════════════════════════════════════════════════════════

Neighbor neighbors[MAX_NEIGHBORS];
SeenPacket seen_packets[MAX_SEEN_PACKETS];
uint8_t seen_index = 0;

uint8_t seq_counter = 0;
uint32_t last_beacon_time = 0;
uint32_t packets_received = 0;
uint32_t packets_forwarded = 0;

// RX/TX buffers
uint8_t rx_buffer[64];
uint8_t tx_buffer[64];

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);  // Wait for Serial (max 3s)
    
    Serial.println();
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.println(F("       EDGE NODE (SINK) - LoRaWAN End Device"));
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.print(F("Node ID: 0x"));
    Serial.println(MY_NODE_ID, HEX);
    Serial.print(F("Role: SINK (Rank "));
    Serial.print(MY_RANK);
    Serial.println(F(")"));
    Serial.println(F("════════════════════════════════════════════════════"));
    
    // Initialize LoRa
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
    
    if (!LoRa.begin(LORA_FREQUENCY)) {
        Serial.println(F("ERROR: LoRa init failed!"));
        while (1);
    }
    
    // Configure LoRa
    LoRa.setSpreadingFactor(LORA_SPREADING);
    LoRa.setSignalBandwidth(LORA_BANDWIDTH);
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.enableCrc();
    
    Serial.println(F("LoRa initialized successfully"));
    Serial.print(F("Frequency: "));
    Serial.print(LORA_FREQUENCY / 1E6);
    Serial.println(F(" MHz"));
    
    // Initialize neighbor table
    memset(neighbors, 0, sizeof(neighbors));
    memset(seen_packets, 0, sizeof(seen_packets));
    
    // Send initial beacon
    broadcast_beacon();
    
    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.println(F("Listening for mesh packets..."));
    Serial.println(F("────────────────────────────────────────────────────"));
}

// ════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
    // Check for incoming packets
    check_lora_receive();
    
    // Periodic beacon broadcast
    if (millis() - last_beacon_time >= BEACON_INTERVAL) {
        broadcast_beacon();
        last_beacon_time = millis();
    }
    
    // Cleanup stale neighbors (optional, every 60s)
    static uint32_t last_cleanup = 0;
    if (millis() - last_cleanup >= 60000) {
        cleanup_stale_neighbors();
        last_cleanup = millis();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// LORA RECEIVE
// ════════════════════════════════════════════════════════════════════════════

void check_lora_receive() {
    int packet_size = LoRa.parsePacket();
    
    if (packet_size > 0) {
        // Read packet
        uint8_t len = 0;
        while (LoRa.available() && len < sizeof(rx_buffer)) {
            rx_buffer[len++] = LoRa.read();
        }
        
        int rssi = LoRa.packetRssi();
        float snr = LoRa.packetSnr();
        
        packets_received++;
        
        // Process packet
        process_received_packet(rx_buffer, len, rssi, snr);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// PACKET PROCESSING
// ════════════════════════════════════════════════════════════════════════════

void process_received_packet(uint8_t* packet, uint8_t len, int rssi, float snr) {
    // Validate minimum length
    if (len < sizeof(MeshHeader)) {
        Serial.println(F("RX: Packet too short, ignoring"));
        return;
    }
    
    MeshHeader* hdr = (MeshHeader*)packet;
    uint8_t* payload = packet + sizeof(MeshHeader);
    uint8_t pkt_type = get_packet_type(hdr->flags);
    
    // Debug output
    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.print(F("RX: type=0x"));
    Serial.print(pkt_type, HEX);
    Serial.print(F(" src=0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" prev=0x"));
    Serial.print(hdr->prev_hop, HEX);
    Serial.print(F(" seq="));
    Serial.print(hdr->seq_num);
    Serial.print(F(" rank="));
    Serial.print(hdr->rank);
    Serial.print(F(" RSSI="));
    Serial.print(rssi);
    Serial.print(F(" SNR="));
    Serial.println(snr);
    
    // Handle by packet type
    switch (pkt_type) {
        case PKT_TYPE_BEACON:
            handle_beacon(hdr, rssi);
            break;
            
        case PKT_TYPE_DATA:
            handle_data(hdr, payload, rssi);
            break;
            
        case PKT_TYPE_ACK:
            // Sinks don't need to process ACKs (they don't send data upward)
            Serial.println(F("RX: ACK received (ignored by sink)"));
            break;
            
        case PKT_TYPE_RELAY:
            handle_lorawan_relay(hdr, payload);
            break;
            
        default:
            Serial.print(F("RX: Unknown packet type 0x"));
            Serial.println(pkt_type, HEX);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// BEACON HANDLING
// ════════════════════════════════════════════════════════════════════════════

void handle_beacon(MeshHeader* hdr, int rssi) {
    Serial.print(F("RX BEACON from node 0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" rank="));
    Serial.print(hdr->rank);
    Serial.print(F(" RSSI="));
    Serial.println(rssi);
    
    // Update neighbor table
    update_neighbor(hdr->src_id, hdr->rank, rssi);
    
    // As a sink, we don't select parents (we ARE the destination)
    // But we track neighbors for network visibility
}

void broadcast_beacon() {
    MeshHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    
    hdr.flags = PKT_TYPE_BEACON;
    hdr.src_id = MY_NODE_ID;
    hdr.dst_id = 0xFF;          // Broadcast
    hdr.prev_hop = MY_NODE_ID;
    hdr.ttl = 1;                // Beacons don't hop
    hdr.seq_num = seq_counter++;
    hdr.rank = MY_RANK;         // Always 0 for sink
    hdr.payload_len = 0;
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&hdr, sizeof(hdr));
    LoRa.endPacket();
    
    Serial.print(F("TX BEACON: rank="));
    Serial.print(MY_RANK);
    Serial.print(F(" seq="));
    Serial.println(hdr.seq_num);
}

// ════════════════════════════════════════════════════════════════════════════
// DATA HANDLING (Payload-Agnostic!)
// ════════════════════════════════════════════════════════════════════════════

void handle_data(MeshHeader* hdr, uint8_t* payload, int rssi) {
    // Check for duplicates
    if (is_duplicate(hdr->src_id, hdr->seq_num)) {
        Serial.println(F("RX DATA: Duplicate, ignoring"));
        return;
    }
    mark_seen(hdr->src_id, hdr->seq_num);
    
    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.println(F("📦 DATA RECEIVED AT SINK!"));
    Serial.print(F("   From: Node 0x"));
    Serial.println(hdr->src_id, HEX);
    Serial.print(F("   Payload length: "));
    Serial.print(hdr->payload_len);
    Serial.println(F(" bytes"));
    
    // Print payload bytes (PAYLOAD-AGNOSTIC - we don't parse it!)
    Serial.print(F("   Payload (hex): "));
    for (uint8_t i = 0; i < hdr->payload_len; i++) {
        if (payload[i] < 0x10) Serial.print(F("0"));
        Serial.print(payload[i], HEX);
        Serial.print(F(" "));
    }
    Serial.println();
    
    // Send ACK if requested
    if (is_ack_requested(hdr->flags)) {
        send_ack(hdr->prev_hop, hdr->seq_num);
    }
    
    // Forward to LoRaWAN (or Serial for demo)
    forward_to_lorawan(hdr, payload);
    
    packets_forwarded++;
    Serial.print(F("   Total forwarded: "));
    Serial.println(packets_forwarded);
    Serial.println(F("────────────────────────────────────────────────────"));
}

// ════════════════════════════════════════════════════════════════════════════
// LORAWAN RELAY HANDLING (Version-Agnostic!)
// ════════════════════════════════════════════════════════════════════════════

void handle_lorawan_relay(MeshHeader* hdr, uint8_t* payload) {
    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.println(F("📡 LORAWAN RELAY PACKET RECEIVED!"));
    Serial.print(F("   Captured by: Node 0x"));
    Serial.println(hdr->src_id, HEX);
    Serial.print(F("   LoRaWAN packet length: "));
    Serial.print(hdr->payload_len);
    Serial.println(F(" bytes"));
    
    // Print LoRaWAN packet bytes (VERSION-AGNOSTIC - we don't parse it!)
    Serial.print(F("   LoRaWAN PHYPayload (hex): "));
    for (uint8_t i = 0; i < hdr->payload_len; i++) {
        if (payload[i] < 0x10) Serial.print(F("0"));
        Serial.print(payload[i], HEX);
        Serial.print(F(" "));
    }
    Serial.println();
    
    // Send ACK if requested
    if (is_ack_requested(hdr->flags)) {
        send_ack(hdr->prev_hop, hdr->seq_num);
    }
    
    // Re-transmit original LoRaWAN packet to gateway
    // In real implementation, this would be a raw LoRa transmission
    // For demo, we just print it
    Serial.println(F("   ACTION: Would re-transmit to LoRaWAN gateway"));
    Serial.println(F("   (Version-agnostic: packet forwarded unchanged!)"));
    Serial.println(F("────────────────────────────────────────────────────"));
}

// ════════════════════════════════════════════════════════════════════════════
// LORAWAN OUTPUT (Demo: Serial output instead of actual LoRaWAN)
// ════════════════════════════════════════════════════════════════════════════

void forward_to_lorawan(MeshHeader* hdr, uint8_t* payload) {
    Serial.println(F(""));
    Serial.println(F("╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║           LORAWAN UPLINK (SIMULATED)             ║"));
    Serial.println(F("╠══════════════════════════════════════════════════╣"));
    Serial.print(F("║ Edge Node: 0x"));
    if (MY_NODE_ID < 0x10) Serial.print(F("0"));
    Serial.print(MY_NODE_ID, HEX);
    Serial.println(F("                                  ║"));
    Serial.print(F("║ Source Mesh Node: 0x"));
    if (hdr->src_id < 0x10) Serial.print(F("0"));
    Serial.print(hdr->src_id, HEX);
    Serial.println(F("                          ║"));
    Serial.print(F("║ Payload Length: "));
    Serial.print(hdr->payload_len);
    Serial.println(F(" bytes                          ║"));
    Serial.println(F("║                                                  ║"));
    Serial.print(F("║ Data: "));
    
    // Format payload as hex string
    for (uint8_t i = 0; i < hdr->payload_len && i < 16; i++) {
        if (payload[i] < 0x10) Serial.print(F("0"));
        Serial.print(payload[i], HEX);
        Serial.print(F(" "));
    }
    if (hdr->payload_len > 16) Serial.print(F("..."));
    
    // Padding
    uint8_t printed = min(hdr->payload_len, (uint8_t)16) * 3;
    for (uint8_t i = printed; i < 40; i++) Serial.print(F(" "));
    Serial.println(F("║"));
    
    Serial.println(F("╠══════════════════════════════════════════════════╣"));
    Serial.println(F("║ In production: LMIC_setTxData2() would be called ║"));
    Serial.println(F("║ to send this to the LoRaWAN gateway.             ║"));
    Serial.println(F("╚══════════════════════════════════════════════════╝"));
    Serial.println();
    
    // ═══════════════════════════════════════════════════════════════════════
    // PRODUCTION CODE (uncomment when using LMIC on ESP32):
    // ═══════════════════════════════════════════════════════════════════════
    // 
    // uint8_t lorawan_payload[64];
    // lorawan_payload[0] = hdr->src_id;  // Include mesh source ID
    // memcpy(&lorawan_payload[1], payload, hdr->payload_len);
    // 
    // LMIC_setTxData2(1, lorawan_payload, hdr->payload_len + 1, 0);
    // ═══════════════════════════════════════════════════════════════════════
}

// ════════════════════════════════════════════════════════════════════════════
// ACK HANDLING
// ════════════════════════════════════════════════════════════════════════════

void send_ack(uint8_t to_node, uint8_t seq) {
    MeshHeader ack;
    memset(&ack, 0, sizeof(ack));
    
    ack.flags = PKT_TYPE_ACK;
    ack.src_id = MY_NODE_ID;
    ack.dst_id = to_node;
    ack.prev_hop = MY_NODE_ID;
    ack.ttl = 1;
    ack.seq_num = seq;          // Echo the sequence number
    ack.rank = MY_RANK;
    ack.payload_len = 0;
    
    // Small delay to avoid collision
    delay(random(10, 50));
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&ack, sizeof(ack));
    LoRa.endPacket();
    
    Serial.print(F("TX ACK to 0x"));
    Serial.print(to_node, HEX);
    Serial.print(F(" for seq="));
    Serial.println(seq);
}

// ════════════════════════════════════════════════════════════════════════════
// NEIGHBOR TABLE MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

void update_neighbor(uint8_t node_id, uint8_t rank, int8_t rssi) {
    // Find existing or empty slot
    int8_t slot = -1;
    int8_t empty_slot = -1;
    
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id == node_id) {
            slot = i;
            break;
        }
        if (neighbors[i].node_id == 0 && empty_slot < 0) {
            empty_slot = i;
        }
    }
    
    // Use existing slot or empty slot
    if (slot < 0) {
        if (empty_slot < 0) {
            Serial.println(F("WARNING: Neighbor table full!"));
            return;
        }
        slot = empty_slot;
    }
    
    // Update entry
    neighbors[slot].node_id = node_id;
    neighbors[slot].rank = rank;
    neighbors[slot].rssi = rssi;
    neighbors[slot].last_seen = millis();
    neighbors[slot].fail_count = 0;  // Reset on successful reception
}

void cleanup_stale_neighbors() {
    uint32_t now = millis();
    uint8_t removed = 0;
    
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id != 0) {
            if (now - neighbors[i].last_seen > PARENT_TIMEOUT * 2) {
                Serial.print(F("Removing stale neighbor 0x"));
                Serial.println(neighbors[i].node_id, HEX);
                neighbors[i].node_id = 0;
                removed++;
            }
        }
    }
    
    if (removed > 0) {
        print_neighbor_table();
    }
}

void print_neighbor_table() {
    Serial.println(F("┌──────────┬──────┬───────┬──────────┐"));
    Serial.println(F("│ Neighbor │ Rank │ RSSI  │ Age (s)  │"));
    Serial.println(F("├──────────┼──────┼───────┼──────────┤"));
    
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id != 0) {
            Serial.print(F("│   0x"));
            if (neighbors[i].node_id < 0x10) Serial.print(F("0"));
            Serial.print(neighbors[i].node_id, HEX);
            Serial.print(F("   │  "));
            Serial.print(neighbors[i].rank);
            Serial.print(F("   │  "));
            Serial.print(neighbors[i].rssi);
            Serial.print(F("  │   "));
            Serial.print((millis() - neighbors[i].last_seen) / 1000);
            Serial.println(F("      │"));
            count++;
        }
    }
    
    if (count == 0) {
        Serial.println(F("│        (empty)                    │"));
    }
    Serial.println(F("└──────────┴──────┴───────┴──────────┘"));
}

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION
// ════════════════════════════════════════════════════════════════════════════

bool is_duplicate(uint8_t src_id, uint8_t seq_num) {
    for (uint8_t i = 0; i < MAX_SEEN_PACKETS; i++) {
        if (seen_packets[i].src_id == src_id && 
            seen_packets[i].seq_num == seq_num) {
            // Check if recent (within 30 seconds)
            if (millis() - seen_packets[i].timestamp < 30000) {
                return true;
            }
        }
    }
    return false;
}

void mark_seen(uint8_t src_id, uint8_t seq_num) {
    seen_packets[seen_index].src_id = src_id;
    seen_packets[seen_index].seq_num = seq_num;
    seen_packets[seen_index].timestamp = millis();
    
    seen_index = (seen_index + 1) % MAX_SEEN_PACKETS;
}
