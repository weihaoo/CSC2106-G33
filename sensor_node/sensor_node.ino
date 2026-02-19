// ════════════════════════════════════════════════════════════════════════════
// MESH NODE (SENSOR) - LoRa Mesh Sensor Node
// 
// Role: Mesh sensor node with gradient routing
// 
// CONFIGURATION:
//   - Device 3: Set MY_NODE_ID to 0x03, SENSOR_TYPE to 0x01 (Gas)
//   - Device 4: Set MY_NODE_ID to 0x04, SENSOR_TYPE to 0x02 (Temp)
//
// HARDWARE:
//   - Arduino Uno + LoRa module (RFM95W, SX1276)
//
// PIN CONFIGURATION (Arduino Uno + RFM95W):
//   - SS:   Pin 10
//   - RST:  Pin 9
//   - DIO0: Pin 2
// ════════════════════════════════════════════════════════════════════════════

#include <SPI.h>
#include <LoRa.h>
#include "mesh_protocol.h"

// ════════════════════════════════════════════════════════════════════════════
// NODE CONFIGURATION - CHANGE FOR EACH DEVICE!
// ════════════════════════════════════════════════════════════════════════════

#define MY_NODE_ID      0x03    // Device 3 = 0x03, Device 4 = 0x04
#define MY_ROLE         ROLE_SENSOR

// Sensor configuration (for demo - use dummy data)
#define SENSOR_TYPE     0x01    // 0x01 = Gas, 0x02 = Temperature

// ════════════════════════════════════════════════════════════════════════════
// PIN CONFIGURATION - Arduino Uno + RFM95W
// ════════════════════════════════════════════════════════════════════════════

#define LORA_SS         10
#define LORA_RST        9
#define LORA_DIO0       2

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL STATE
// ════════════════════════════════════════════════════════════════════════════

// Gradient routing state
uint8_t my_rank = 255;          // Start as orphan (max rank = disconnected)
uint8_t parent_id = 0;          // No parent initially
uint8_t node_state = STATE_ORPHAN;

// Neighbor table
Neighbor neighbors[MAX_NEIGHBORS];

// Pending ACKs
PendingAck pending_acks[MAX_PENDING_ACKS];

// Deduplication
SeenPacket seen_packets[MAX_SEEN_PACKETS];
uint8_t seen_index = 0;

// Counters and timers
uint8_t seq_counter = 0;
uint32_t last_beacon_time = 0;
uint32_t last_sensor_time = 0;
uint32_t last_parent_heard = 0;

// Statistics
uint32_t packets_sent = 0;
uint32_t packets_acked = 0;
uint32_t packets_failed = 0;

// RX/TX buffers
uint8_t rx_buffer[64];
uint8_t tx_buffer[64];

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    
    Serial.println();
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.println(F("         MESH NODE (SENSOR) - Gradient Routing"));
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.print(F("Node ID: 0x"));
    Serial.println(MY_NODE_ID, HEX);
    Serial.print(F("Sensor Type: "));
    if (SENSOR_TYPE == 0x01) {
        Serial.println(F("Gas Sensor"));
    } else if (SENSOR_TYPE == 0x02) {
        Serial.println(F("Temperature Sensor"));
    } else {
        Serial.println(F("Unknown"));
    }
    Serial.println(F("State: ORPHAN (waiting for beacons...)"));
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
    
    // Initialize data structures
    memset(neighbors, 0, sizeof(neighbors));
    memset(pending_acks, 0, sizeof(pending_acks));
    memset(seen_packets, 0, sizeof(seen_packets));
    
    // Randomize initial timing to avoid collisions
    last_beacon_time = millis() - random(0, BEACON_INTERVAL / 2);
    last_sensor_time = millis() - random(0, SENSOR_INTERVAL / 2);
    
    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.println(F("Listening for beacons from sink nodes..."));
    Serial.println(F("────────────────────────────────────────────────────"));
}

// ════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
    // 1. Check for incoming packets
    check_lora_receive();
    
    // 2. Check pending ACKs for timeouts
    check_pending_acks();
    
    // 3. Check parent timeout
    check_parent_timeout();
    
    // 4. Send beacon (if connected)
    if (node_state == STATE_CONNECTED) {
        if (millis() - last_beacon_time >= BEACON_INTERVAL) {
            broadcast_beacon();
            last_beacon_time = millis();
        }
    }
    
    // 5. Send sensor data (if connected)
    if (node_state == STATE_CONNECTED) {
        if (millis() - last_sensor_time >= SENSOR_INTERVAL) {
            send_sensor_data();
            last_sensor_time = millis();
        }
    }
    
    // 6. Print status periodically
    static uint32_t last_status = 0;
    if (millis() - last_status >= 30000) {
        print_status();
        last_status = millis();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// LORA RECEIVE
// ════════════════════════════════════════════════════════════════════════════

void check_lora_receive() {
    int packet_size = LoRa.parsePacket();
    
    if (packet_size > 0) {
        uint8_t len = 0;
        while (LoRa.available() && len < sizeof(rx_buffer)) {
            rx_buffer[len++] = LoRa.read();
        }
        
        int rssi = LoRa.packetRssi();
        
        process_received_packet(rx_buffer, len, rssi);
    }
}

void process_received_packet(uint8_t* packet, uint8_t len, int rssi) {
    if (len < sizeof(MeshHeader)) {
        return;  // Too short
    }
    
    MeshHeader* hdr = (MeshHeader*)packet;
    uint8_t* payload = packet + sizeof(MeshHeader);
    uint8_t pkt_type = get_packet_type(hdr->flags);
    
    // Ignore our own packets
    if (hdr->src_id == MY_NODE_ID) {
        return;
    }
    
    switch (pkt_type) {
        case PKT_TYPE_BEACON:
            handle_beacon(hdr, rssi);
            break;
            
        case PKT_TYPE_ACK:
            handle_ack(hdr);
            break;
            
        case PKT_TYPE_DATA:
            handle_data_forward(hdr, payload, len, rssi);
            break;
            
        default:
            break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// BEACON HANDLING - Gradient Routing Core
// ════════════════════════════════════════════════════════════════════════════

void handle_beacon(MeshHeader* hdr, int rssi) {
    Serial.print(F("RX BEACON from 0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" rank="));
    Serial.print(hdr->rank);
    Serial.print(F(" RSSI="));
    Serial.println(rssi);
    
    // Update neighbor table
    update_neighbor(hdr->src_id, hdr->rank, rssi);
    
    // If from current parent, update last heard time
    if (hdr->src_id == parent_id) {
        last_parent_heard = millis();
    }
    
    // Consider as potential parent if lower rank
    if (hdr->rank < my_rank || node_state == STATE_ORPHAN) {
        consider_parent(hdr->src_id, hdr->rank, rssi);
    }
}

void consider_parent(uint8_t node_id, uint8_t their_rank, int8_t rssi) {
    uint8_t new_rank = their_rank + 1;
    
    // First parent (orphan state)
    if (node_state == STATE_ORPHAN || parent_id == 0) {
        select_parent(node_id, new_rank, rssi);
        return;
    }
    
    // Already have a parent - check if this one is better
    Neighbor* current_parent = find_neighbor(parent_id);
    if (current_parent == NULL) {
        // Current parent not in table anymore, switch!
        select_parent(node_id, new_rank, rssi);
        return;
    }
    
    // Compare: lower rank always wins
    if (their_rank < current_parent->rank) {
        Serial.println(F("Found better parent (lower rank)!"));
        select_parent(node_id, new_rank, rssi);
        return;
    }
    
    // Same rank: need significantly better RSSI to switch (hysteresis)
    if (their_rank == current_parent->rank) {
        if (rssi > current_parent->rssi + RSSI_HYSTERESIS) {
            Serial.println(F("Found better parent (better RSSI)!"));
            select_parent(node_id, new_rank, rssi);
            return;
        }
    }
    
    // Keep current parent, but remember this one as backup
}

void select_parent(uint8_t new_parent, uint8_t new_rank, int8_t rssi) {
    uint8_t old_parent = parent_id;
    uint8_t old_rank = my_rank;
    
    parent_id = new_parent;
    my_rank = new_rank;
    node_state = STATE_CONNECTED;
    last_parent_heard = millis();
    
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.println(F("        PARENT SELECTED - NOW CONNECTED!"));
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.print(F("New Parent: 0x"));
    Serial.println(new_parent, HEX);
    Serial.print(F("My Rank: "));
    Serial.println(my_rank);
    Serial.print(F("Parent RSSI: "));
    Serial.println(rssi);
    
    if (old_parent != 0 && old_parent != new_parent) {
        Serial.print(F("(Switched from 0x"));
        Serial.print(old_parent, HEX);
        Serial.println(F(")"));
    }
    Serial.println(F("════════════════════════════════════════════════════"));
}

void broadcast_beacon() {
    if (my_rank == 255) return;  // Don't beacon if orphan
    
    MeshHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    
    hdr.flags = PKT_TYPE_BEACON;
    hdr.src_id = MY_NODE_ID;
    hdr.dst_id = 0xFF;
    hdr.prev_hop = MY_NODE_ID;
    hdr.ttl = 1;
    hdr.seq_num = seq_counter++;
    hdr.rank = my_rank;
    hdr.payload_len = 0;
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&hdr, sizeof(hdr));
    LoRa.endPacket();
    
    Serial.print(F("TX BEACON: rank="));
    Serial.println(my_rank);
}

// ════════════════════════════════════════════════════════════════════════════
// SENSOR DATA - Payload-Agnostic Design
// ════════════════════════════════════════════════════════════════════════════

void send_sensor_data() {
    if (parent_id == 0) {
        Serial.println(F("Cannot send: no parent!"));
        return;
    }
    
    // ══════════════════════════════════════════════════════════════════════
    // CREATE PAYLOAD - Different formats for different sensors
    // This demonstrates PAYLOAD-AGNOSTIC: relay doesn't care about format!
    // ══════════════════════════════════════════════════════════════════════
    
    uint8_t payload[16];
    uint8_t payload_len = 0;
    
    if (SENSOR_TYPE == 0x01) {
        // Gas sensor payload: [type][gas_ppm_hi][gas_ppm_lo][alert_level]
        uint16_t gas_ppm = random(0, 1000);  // Dummy: 0-1000 ppm
        uint8_t alert = (gas_ppm > 500) ? 1 : 0;
        
        payload[0] = SENSOR_TYPE;
        payload[1] = (gas_ppm >> 8) & 0xFF;
        payload[2] = gas_ppm & 0xFF;
        payload[3] = alert;
        payload_len = 4;
        
        Serial.print(F("SENSOR: Gas = "));
        Serial.print(gas_ppm);
        Serial.print(F(" ppm, Alert = "));
        Serial.println(alert);
        
    } else if (SENSOR_TYPE == 0x02) {
        // Temperature payload: [type][temp_int][temp_frac][humidity]
        int8_t temp_int = random(20, 35);    // Dummy: 20-35°C
        uint8_t temp_frac = random(0, 99);   // Decimal part
        uint8_t humidity = random(40, 90);   // Dummy: 40-90%
        
        payload[0] = SENSOR_TYPE;
        payload[1] = temp_int;
        payload[2] = temp_frac;
        payload[3] = humidity;
        payload_len = 4;
        
        Serial.print(F("SENSOR: Temp = "));
        Serial.print(temp_int);
        Serial.print(F("."));
        Serial.print(temp_frac);
        Serial.print(F("°C, Humidity = "));
        Serial.print(humidity);
        Serial.println(F("%"));
        
    } else {
        // Generic payload
        payload[0] = SENSOR_TYPE;
        payload[1] = random(0, 255);
        payload[2] = random(0, 255);
        payload_len = 3;
    }
    
    // ══════════════════════════════════════════════════════════════════════
    // SEND VIA MESH
    // ══════════════════════════════════════════════════════════════════════
    
    send_to_parent(payload, payload_len);
}

void send_to_parent(uint8_t* payload, uint8_t payload_len) {
    MeshHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    
    hdr.flags = PKT_TYPE_DATA | PKT_FLAG_ACK_REQ;  // Request ACK
    hdr.src_id = MY_NODE_ID;
    hdr.dst_id = 0x00;          // Any sink
    hdr.prev_hop = MY_NODE_ID;
    hdr.ttl = DEFAULT_TTL;
    hdr.seq_num = seq_counter++;
    hdr.rank = my_rank;
    hdr.payload_len = payload_len;
    
    // Transmit
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&hdr, sizeof(hdr));
    LoRa.write(payload, payload_len);
    LoRa.endPacket();
    
    packets_sent++;
    
    Serial.print(F("TX DATA to parent 0x"));
    Serial.print(parent_id, HEX);
    Serial.print(F(" seq="));
    Serial.print(hdr.seq_num);
    Serial.print(F(" len="));
    Serial.println(payload_len);
    
    // Queue for ACK
    queue_pending_ack(hdr.seq_num, parent_id, payload, payload_len);
}

// ════════════════════════════════════════════════════════════════════════════
// DATA FORWARDING (for relay functionality)
// ════════════════════════════════════════════════════════════════════════════

void handle_data_forward(MeshHeader* hdr, uint8_t* payload, uint8_t total_len, int rssi) {
    // Check for duplicates
    if (is_duplicate(hdr->src_id, hdr->seq_num)) {
        return;
    }
    mark_seen(hdr->src_id, hdr->seq_num);
    
    // If destination is us, process locally (shouldn't happen for sensors)
    if (hdr->dst_id == MY_NODE_ID) {
        Serial.println(F("RX DATA: Addressed to me (unexpected)"));
        return;
    }
    
    // Send ACK to previous hop if requested
    if (is_ack_requested(hdr->flags)) {
        send_ack(hdr->prev_hop, hdr->seq_num);
    }
    
    // Forward toward sink (if we have a parent and TTL > 1)
    if (parent_id != 0 && hdr->ttl > 1) {
        Serial.print(F("FWD DATA from 0x"));
        Serial.print(hdr->src_id, HEX);
        Serial.print(F(" via 0x"));
        Serial.println(parent_id, HEX);
        
        // Update header for forwarding
        hdr->ttl--;
        hdr->prev_hop = MY_NODE_ID;
        hdr->flags |= PKT_FLAG_IS_FWD;
        
        // Forward (PAYLOAD-AGNOSTIC - we don't touch payload!)
        delay(random(10, 50));  // Random backoff
        
        LoRa.beginPacket();
        LoRa.write((uint8_t*)hdr, sizeof(MeshHeader));
        LoRa.write(payload, hdr->payload_len);
        LoRa.endPacket();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// ACK HANDLING
// ════════════════════════════════════════════════════════════════════════════

void handle_ack(MeshHeader* hdr) {
    // Check if this ACK is for us
    if (hdr->dst_id != MY_NODE_ID) {
        return;
    }
    
    Serial.print(F("RX ACK from 0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" for seq="));
    Serial.println(hdr->seq_num);
    
    // Find and clear pending ACK
    for (uint8_t i = 0; i < MAX_PENDING_ACKS; i++) {
        if (pending_acks[i].active && 
            pending_acks[i].seq_num == hdr->seq_num &&
            pending_acks[i].dest_id == hdr->src_id) {
            
            pending_acks[i].active = false;
            packets_acked++;
            
            // Reset fail count for this neighbor
            Neighbor* n = find_neighbor(hdr->src_id);
            if (n != NULL) {
                n->fail_count = 0;
            }
            
            Serial.println(F("ACK matched, packet delivered!"));
            return;
        }
    }
}

void send_ack(uint8_t to_node, uint8_t seq) {
    MeshHeader ack;
    memset(&ack, 0, sizeof(ack));
    
    ack.flags = PKT_TYPE_ACK;
    ack.src_id = MY_NODE_ID;
    ack.dst_id = to_node;
    ack.prev_hop = MY_NODE_ID;
    ack.ttl = 1;
    ack.seq_num = seq;
    ack.rank = my_rank;
    ack.payload_len = 0;
    
    delay(random(10, 50));
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&ack, sizeof(ack));
    LoRa.endPacket();
    
    Serial.print(F("TX ACK to 0x"));
    Serial.print(to_node, HEX);
    Serial.print(F(" seq="));
    Serial.println(seq);
}

void queue_pending_ack(uint8_t seq, uint8_t dest, uint8_t* payload, uint8_t len) {
    // Find empty slot
    for (uint8_t i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!pending_acks[i].active) {
            pending_acks[i].active = true;
            pending_acks[i].seq_num = seq;
            pending_acks[i].dest_id = dest;
            pending_acks[i].retries = 0;
            pending_acks[i].sent_time = millis();
            pending_acks[i].payload_len = min(len, (uint8_t)32);
            memcpy(pending_acks[i].payload, payload, pending_acks[i].payload_len);
            return;
        }
    }
    Serial.println(F("WARNING: Pending ACK queue full!"));
}

void check_pending_acks() {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!pending_acks[i].active) continue;
        
        if (now - pending_acks[i].sent_time >= ACK_TIMEOUT) {
            pending_acks[i].retries++;
            
            if (pending_acks[i].retries >= MAX_RETRIES) {
                // Max retries reached - mark failure
                Serial.print(F("FAIL: No ACK for seq="));
                Serial.print(pending_acks[i].seq_num);
                Serial.println(F(" after max retries"));
                
                pending_acks[i].active = false;
                packets_failed++;
                
                // Increment fail count for parent
                Neighbor* n = find_neighbor(pending_acks[i].dest_id);
                if (n != NULL) {
                    n->fail_count++;
                    
                    // If too many failures, trigger parent switch
                    if (n->fail_count >= 3 && pending_acks[i].dest_id == parent_id) {
                        Serial.println(F("Parent unreliable! Switching..."));
                        switch_parent();
                    }
                }
            } else {
                // Retry
                Serial.print(F("RETRY #"));
                Serial.print(pending_acks[i].retries);
                Serial.print(F(" for seq="));
                Serial.println(pending_acks[i].seq_num);
                
                // Retransmit
                MeshHeader hdr;
                hdr.flags = PKT_TYPE_DATA | PKT_FLAG_ACK_REQ;
                hdr.src_id = MY_NODE_ID;
                hdr.dst_id = 0x00;
                hdr.prev_hop = MY_NODE_ID;
                hdr.ttl = DEFAULT_TTL;
                hdr.seq_num = pending_acks[i].seq_num;
                hdr.rank = my_rank;
                hdr.payload_len = pending_acks[i].payload_len;
                
                LoRa.beginPacket();
                LoRa.write((uint8_t*)&hdr, sizeof(hdr));
                LoRa.write(pending_acks[i].payload, pending_acks[i].payload_len);
                LoRa.endPacket();
                
                pending_acks[i].sent_time = now;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// PARENT TIMEOUT AND SELF-HEALING
// ════════════════════════════════════════════════════════════════════════════

void check_parent_timeout() {
    if (node_state != STATE_CONNECTED) return;
    if (parent_id == 0) return;
    
    if (millis() - last_parent_heard >= PARENT_TIMEOUT) {
        Serial.println(F("════════════════════════════════════════════════════"));
        Serial.println(F("⚠️  PARENT TIMEOUT! Switching to backup..."));
        Serial.println(F("════════════════════════════════════════════════════"));
        
        switch_parent();
    }
}

void switch_parent() {
    // Mark current parent as failed
    Neighbor* old = find_neighbor(parent_id);
    if (old != NULL) {
        old->fail_count = 255;  // Mark as dead
    }
    
    uint8_t old_parent = parent_id;
    parent_id = 0;
    
    // Find best alternative parent (lower rank than us)
    uint8_t best_id = 0;
    uint8_t best_rank = 255;
    int8_t best_rssi = -128;
    
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id == 0) continue;
        if (neighbors[i].node_id == old_parent) continue;  // Skip failed
        if (neighbors[i].fail_count >= 3) continue;        // Skip unreliable
        if (neighbors[i].rank >= my_rank) continue;        // Must be lower rank
        
        // Check if recently heard
        if (millis() - neighbors[i].last_seen > PARENT_TIMEOUT) continue;
        
        // Better rank, or same rank with better RSSI
        if (neighbors[i].rank < best_rank ||
            (neighbors[i].rank == best_rank && neighbors[i].rssi > best_rssi)) {
            best_id = neighbors[i].node_id;
            best_rank = neighbors[i].rank;
            best_rssi = neighbors[i].rssi;
        }
    }
    
    if (best_id != 0) {
        select_parent(best_id, best_rank + 1, best_rssi);
        Serial.println(F("Self-healing successful!"));
    } else {
        // No backup available - become orphan
        node_state = STATE_ORPHAN;
        my_rank = 255;
        Serial.println(F("No backup parent! Becoming ORPHAN..."));
        Serial.println(F("Waiting for beacons..."));
    }
    
    print_neighbor_table();
}

// ════════════════════════════════════════════════════════════════════════════
// NEIGHBOR TABLE MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

void update_neighbor(uint8_t node_id, uint8_t rank, int8_t rssi) {
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
    
    if (slot < 0) {
        if (empty_slot < 0) return;
        slot = empty_slot;
    }
    
    neighbors[slot].node_id = node_id;
    neighbors[slot].rank = rank;
    neighbors[slot].rssi = rssi;
    neighbors[slot].last_seen = millis();
    // Don't reset fail_count here - only on successful ACK
}

Neighbor* find_neighbor(uint8_t node_id) {
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id == node_id) {
            return &neighbors[i];
        }
    }
    return NULL;
}

void print_neighbor_table() {
    Serial.println(F("┌──────────┬──────┬───────┬───────┬──────────┐"));
    Serial.println(F("│ Neighbor │ Rank │ RSSI  │ Fails │ Parent?  │"));
    Serial.println(F("├──────────┼──────┼───────┼───────┼──────────┤"));
    
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id != 0) {
            Serial.print(F("│   0x"));
            if (neighbors[i].node_id < 0x10) Serial.print(F("0"));
            Serial.print(neighbors[i].node_id, HEX);
            Serial.print(F("   │  "));
            Serial.print(neighbors[i].rank);
            Serial.print(F("   │  "));
            if (neighbors[i].rssi >= 0) Serial.print(F(" "));
            if (neighbors[i].rssi > -10) Serial.print(F(" "));
            Serial.print(neighbors[i].rssi);
            Serial.print(F(" │   "));
            Serial.print(neighbors[i].fail_count);
            Serial.print(F("   │   "));
            if (neighbors[i].node_id == parent_id) {
                Serial.print(F("YES"));
            } else {
                Serial.print(F("no "));
            }
            Serial.println(F("    │"));
        }
    }
    Serial.println(F("└──────────┴──────┴───────┴───────┴──────────┘"));
}

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION
// ════════════════════════════════════════════════════════════════════════════

bool is_duplicate(uint8_t src_id, uint8_t seq_num) {
    for (uint8_t i = 0; i < MAX_SEEN_PACKETS; i++) {
        if (seen_packets[i].src_id == src_id && 
            seen_packets[i].seq_num == seq_num) {
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

// ════════════════════════════════════════════════════════════════════════════
// STATUS DISPLAY
// ════════════════════════════════════════════════════════════════════════════

void print_status() {
    Serial.println();
    Serial.println(F("╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║               NODE STATUS                        ║"));
    Serial.println(F("╠══════════════════════════════════════════════════╣"));
    Serial.print(F("║ Node ID: 0x"));
    if (MY_NODE_ID < 0x10) Serial.print(F("0"));
    Serial.print(MY_NODE_ID, HEX);
    Serial.print(F("     State: "));
    switch (node_state) {
        case STATE_ORPHAN:     Serial.print(F("ORPHAN    ")); break;
        case STATE_CONNECTED:  Serial.print(F("CONNECTED ")); break;
        case STATE_SWITCHING:  Serial.print(F("SWITCHING ")); break;
        default:               Serial.print(F("UNKNOWN   ")); break;
    }
    Serial.println(F("        ║"));
    
    Serial.print(F("║ Rank: "));
    Serial.print(my_rank);
    Serial.print(F("          Parent: "));
    if (parent_id != 0) {
        Serial.print(F("0x"));
        if (parent_id < 0x10) Serial.print(F("0"));
        Serial.print(parent_id, HEX);
    } else {
        Serial.print(F("none"));
    }
    Serial.println(F("                  ║"));
    
    Serial.print(F("║ Sent: "));
    Serial.print(packets_sent);
    Serial.print(F("  ACKed: "));
    Serial.print(packets_acked);
    Serial.print(F("  Failed: "));
    Serial.print(packets_failed);
    Serial.println(F("              ║"));
    
    Serial.println(F("╚══════════════════════════════════════════════════╝"));
    
    print_neighbor_table();
}
