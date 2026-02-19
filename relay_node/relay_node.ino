// ════════════════════════════════════════════════════════════════════════════
// RELAY NODE - LoRa Mesh Relay
// 
// Role: Intermediate relay node - forwards packets between sensors and edge
// 
// CONFIGURATION:
//   - Set MY_NODE_ID to 0x03
//
// HARDWARE:
//   - Arduino Uno + RFM95W LoRa module
//
// FEATURES:
//   - Receives beacons from edge nodes and other relays
//   - Selects best parent (gradient routing)
//   - Forwards data packets (payload-agnostic)
//   - Broadcasts own beacons for downstream nodes
//   - Self-healing on parent failure
// ════════════════════════════════════════════════════════════════════════════

#include <SPI.h>
#include <LoRa.h>
#include "mesh_protocol.h"

// ════════════════════════════════════════════════════════════════════════════
// NODE CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

#define MY_NODE_ID      0x03    // Relay node ID
#define MY_ROLE         ROLE_RELAY

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
uint8_t my_rank = 255;          // Start as orphan
uint8_t parent_id = 0;          // No parent initially
uint8_t node_state = STATE_ORPHAN;

// Tables
Neighbor neighbors[MAX_NEIGHBORS];
PendingAck pending_acks[MAX_PENDING_ACKS];
SeenPacket seen_packets[MAX_SEEN_PACKETS];
uint8_t seen_index = 0;

// Counters and timers
uint8_t seq_counter = 0;
uint32_t last_beacon_time = 0;
uint32_t last_parent_heard = 0;

// Statistics
uint32_t packets_forwarded = 0;
uint32_t packets_dropped = 0;

// Buffers
uint8_t rx_buffer[64];

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    
    Serial.println();
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.println(F("            RELAY NODE - Mesh Forwarder"));
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.print(F("Node ID: 0x"));
    Serial.println(MY_NODE_ID, HEX);
    Serial.println(F("Role: RELAY"));
    Serial.println(F("State: ORPHAN (waiting for beacons...)"));
    Serial.println(F("════════════════════════════════════════════════════"));
    
    // Initialize LoRa
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
    
    if (!LoRa.begin(LORA_FREQUENCY)) {
        Serial.println(F("ERROR: LoRa init failed!"));
        while (1);
    }
    
    LoRa.setSpreadingFactor(LORA_SPREADING);
    LoRa.setSignalBandwidth(LORA_BANDWIDTH);
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.enableCrc();
    
    Serial.println(F("LoRa initialized"));
    
    // Initialize tables
    memset(neighbors, 0, sizeof(neighbors));
    memset(pending_acks, 0, sizeof(pending_acks));
    memset(seen_packets, 0, sizeof(seen_packets));
    
    // Randomize timing
    last_beacon_time = millis() - random(0, BEACON_INTERVAL / 2);
    
    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.println(F("Listening for beacons..."));
}

// ════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
    // 1. Receive packets
    check_lora_receive();
    
    // 2. Check pending ACKs
    check_pending_acks();
    
    // 3. Check parent timeout
    check_parent_timeout();
    
    // 4. Broadcast beacon (if connected)
    if (node_state == STATE_CONNECTED) {
        if (millis() - last_beacon_time >= BEACON_INTERVAL) {
            broadcast_beacon();
            last_beacon_time = millis();
        }
    }
    
    // 5. Status update
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
        process_packet(rx_buffer, len, rssi);
    }
}

void process_packet(uint8_t* packet, uint8_t len, int rssi) {
    if (len < sizeof(MeshHeader)) return;
    
    MeshHeader* hdr = (MeshHeader*)packet;
    uint8_t* payload = packet + sizeof(MeshHeader);
    uint8_t pkt_type = get_packet_type(hdr->flags);
    
    // Ignore own packets
    if (hdr->src_id == MY_NODE_ID) return;
    
    switch (pkt_type) {
        case PKT_TYPE_BEACON:
            handle_beacon(hdr, rssi);
            break;
            
        case PKT_TYPE_DATA:
            handle_data(hdr, payload, len, rssi);
            break;
            
        case PKT_TYPE_ACK:
            handle_ack(hdr);
            break;
            
        case PKT_TYPE_RELAY:
            // LoRaWAN relay packets - forward same as data
            handle_data(hdr, payload, len, rssi);
            break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// BEACON HANDLING
// ════════════════════════════════════════════════════════════════════════════

void handle_beacon(MeshHeader* hdr, int rssi) {
    Serial.print(F("RX BEACON: from=0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" rank="));
    Serial.print(hdr->rank);
    Serial.print(F(" RSSI="));
    Serial.println(rssi);
    
    // Update neighbor
    update_neighbor(hdr->src_id, hdr->rank, rssi);
    
    // Reset parent timer
    if (hdr->src_id == parent_id) {
        last_parent_heard = millis();
    }
    
    // Consider as parent?
    if (hdr->rank < my_rank || node_state == STATE_ORPHAN) {
        consider_parent(hdr->src_id, hdr->rank, rssi);
    }
}

void consider_parent(uint8_t node_id, uint8_t their_rank, int8_t rssi) {
    uint8_t new_rank = their_rank + 1;
    
    // First parent
    if (node_state == STATE_ORPHAN || parent_id == 0) {
        select_parent(node_id, new_rank, rssi);
        return;
    }
    
    // Compare with current
    Neighbor* current = find_neighbor(parent_id);
    if (current == NULL) {
        select_parent(node_id, new_rank, rssi);
        return;
    }
    
    // Lower rank wins
    if (their_rank < current->rank) {
        select_parent(node_id, new_rank, rssi);
        return;
    }
    
    // Same rank, better RSSI (with hysteresis)
    if (their_rank == current->rank && rssi > current->rssi + RSSI_HYSTERESIS) {
        select_parent(node_id, new_rank, rssi);
    }
}

void select_parent(uint8_t new_parent, uint8_t new_rank, int8_t rssi) {
    uint8_t old_parent = parent_id;
    
    parent_id = new_parent;
    my_rank = new_rank;
    node_state = STATE_CONNECTED;
    last_parent_heard = millis();
    
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.println(F("         PARENT SELECTED!"));
    Serial.println(F("════════════════════════════════════════════════════"));
    Serial.print(F("Parent: 0x"));
    Serial.println(new_parent, HEX);
    Serial.print(F("My Rank: "));
    Serial.println(my_rank);
    Serial.print(F("RSSI: "));
    Serial.println(rssi);
    
    if (old_parent != 0 && old_parent != new_parent) {
        Serial.print(F("(Switched from 0x"));
        Serial.print(old_parent, HEX);
        Serial.println(F(")"));
    }
    Serial.println(F("════════════════════════════════════════════════════"));
}

void broadcast_beacon() {
    if (my_rank == 255) return;
    
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
// DATA FORWARDING (PAYLOAD-AGNOSTIC!)
// ════════════════════════════════════════════════════════════════════════════

void handle_data(MeshHeader* hdr, uint8_t* payload, uint8_t total_len, int rssi) {
    
    // Deduplication
    if (is_duplicate(hdr->src_id, hdr->seq_num)) {
        // Still send ACK to stop retries
        if (is_ack_requested(hdr->flags)) {
            send_ack(hdr->prev_hop, hdr->seq_num);
        }
        Serial.println(F("RX DATA: Duplicate, ACK sent, not forwarding"));
        return;
    }
    mark_seen(hdr->src_id, hdr->seq_num);
    
    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.println(F("📦 DATA RECEIVED - FORWARDING"));
    Serial.print(F("   Original src: 0x"));
    Serial.println(hdr->src_id, HEX);
    Serial.print(F("   Previous hop: 0x"));
    Serial.println(hdr->prev_hop, HEX);
    Serial.print(F("   Payload len: "));
    Serial.println(hdr->payload_len);
    
    // ════════════════════════════════════════════════════════════════════
    // PAYLOAD-AGNOSTIC: We DON'T parse the payload!
    // Just print raw bytes for debug
    // ════════════════════════════════════════════════════════════════════
    Serial.print(F("   Payload (hex): "));
    for (uint8_t i = 0; i < hdr->payload_len && i < 16; i++) {
        if (payload[i] < 0x10) Serial.print(F("0"));
        Serial.print(payload[i], HEX);
        Serial.print(F(" "));
    }
    Serial.println();
    
    // Send ACK to previous hop
    if (is_ack_requested(hdr->flags)) {
        send_ack(hdr->prev_hop, hdr->seq_num);
    }
    
    // Check TTL
    if (hdr->ttl <= 1) {
        Serial.println(F("   TTL expired, dropping"));
        packets_dropped++;
        return;
    }
    
    // Check if we have a parent
    if (parent_id == 0) {
        Serial.println(F("   No parent, dropping"));
        packets_dropped++;
        return;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // FORWARD TO PARENT (payload unchanged!)
    // ════════════════════════════════════════════════════════════════════
    
    // Update header for forwarding
    hdr->ttl--;
    hdr->prev_hop = MY_NODE_ID;
    hdr->flags |= PKT_FLAG_IS_FWD;
    
    // Random backoff
    delay(random(10, 50));
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)hdr, sizeof(MeshHeader));
    LoRa.write(payload, hdr->payload_len);  // Forward unchanged!
    LoRa.endPacket();
    
    packets_forwarded++;
    
    Serial.print(F("   FORWARDED to parent 0x"));
    Serial.println(parent_id, HEX);
    Serial.print(F("   Total forwarded: "));
    Serial.println(packets_forwarded);
    Serial.println(F("────────────────────────────────────────────────────"));
}

// ════════════════════════════════════════════════════════════════════════════
// ACK HANDLING
// ════════════════════════════════════════════════════════════════════════════

void handle_ack(MeshHeader* hdr) {
    if (hdr->dst_id != MY_NODE_ID) return;
    
    Serial.print(F("RX ACK from 0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" seq="));
    Serial.println(hdr->seq_num);
    
    // Clear pending ACK
    for (uint8_t i = 0; i < MAX_PENDING_ACKS; i++) {
        if (pending_acks[i].active &&
            pending_acks[i].seq_num == hdr->seq_num &&
            pending_acks[i].dest_id == hdr->src_id) {
            
            pending_acks[i].active = false;
            
            Neighbor* n = find_neighbor(hdr->src_id);
            if (n) n->fail_count = 0;
            
            Serial.println(F("ACK matched!"));
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
    
    delay(random(5, 30));
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&ack, sizeof(ack));
    LoRa.endPacket();
    
    Serial.print(F("TX ACK to 0x"));
    Serial.print(to_node, HEX);
    Serial.print(F(" seq="));
    Serial.println(seq);
}

void check_pending_acks() {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!pending_acks[i].active) continue;
        
        if (now - pending_acks[i].sent_time >= ACK_TIMEOUT) {
            pending_acks[i].retries++;
            
            if (pending_acks[i].retries >= MAX_RETRIES) {
                Serial.print(F("FAIL: No ACK for seq="));
                Serial.println(pending_acks[i].seq_num);
                
                pending_acks[i].active = false;
                
                Neighbor* n = find_neighbor(pending_acks[i].dest_id);
                if (n) {
                    n->fail_count++;
                    if (n->fail_count >= 3 && pending_acks[i].dest_id == parent_id) {
                        switch_parent();
                    }
                }
            } else {
                Serial.print(F("RETRY #"));
                Serial.println(pending_acks[i].retries);
                // Retransmit would go here
                pending_acks[i].sent_time = now;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// SELF-HEALING
// ════════════════════════════════════════════════════════════════════════════

void check_parent_timeout() {
    if (node_state != STATE_CONNECTED) return;
    if (parent_id == 0) return;
    
    if (millis() - last_parent_heard >= PARENT_TIMEOUT) {
        Serial.println(F("════════════════════════════════════════════════════"));
        Serial.println(F("⚠️  PARENT TIMEOUT!"));
        Serial.println(F("════════════════════════════════════════════════════"));
        switch_parent();
    }
}

void switch_parent() {
    Neighbor* old = find_neighbor(parent_id);
    if (old) old->fail_count = 255;
    
    uint8_t old_parent = parent_id;
    parent_id = 0;
    
    // Find best alternative
    uint8_t best_id = 0;
    uint8_t best_rank = 255;
    int8_t best_rssi = -128;
    
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id == 0) continue;
        if (neighbors[i].node_id == old_parent) continue;
        if (neighbors[i].fail_count >= 3) continue;
        if (neighbors[i].rank >= my_rank) continue;
        if (millis() - neighbors[i].last_seen > PARENT_TIMEOUT) continue;
        
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
        node_state = STATE_ORPHAN;
        my_rank = 255;
        Serial.println(F("No backup! Becoming ORPHAN..."));
    }
    
    print_neighbor_table();
}

// ════════════════════════════════════════════════════════════════════════════
// NEIGHBOR TABLE
// ════════════════════════════════════════════════════════════════════════════

void update_neighbor(uint8_t node_id, uint8_t rank, int8_t rssi) {
    int8_t slot = -1;
    int8_t empty = -1;
    
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id == node_id) {
            slot = i;
            break;
        }
        if (neighbors[i].node_id == 0 && empty < 0) {
            empty = i;
        }
    }
    
    if (slot < 0) {
        if (empty < 0) return;
        slot = empty;
    }
    
    neighbors[slot].node_id = node_id;
    neighbors[slot].rank = rank;
    neighbors[slot].rssi = rssi;
    neighbors[slot].last_seen = millis();
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
    Serial.println(F("┌──────────┬──────┬───────┬───────────┐"));
    Serial.println(F("│ Neighbor │ Rank │ RSSI  │ Parent?   │"));
    Serial.println(F("├──────────┼──────┼───────┼───────────┤"));
    
    for (uint8_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].node_id != 0) {
            Serial.print(F("│   0x"));
            if (neighbors[i].node_id < 0x10) Serial.print(F("0"));
            Serial.print(neighbors[i].node_id, HEX);
            Serial.print(F("   │  "));
            Serial.print(neighbors[i].rank);
            Serial.print(F("   │ "));
            if (neighbors[i].rssi > -10) Serial.print(F(" "));
            if (neighbors[i].rssi >= 0) Serial.print(F(" "));
            Serial.print(neighbors[i].rssi);
            Serial.print(F("  │   "));
            if (neighbors[i].node_id == parent_id) {
                Serial.print(F("YES"));
            } else {
                Serial.print(F("no "));
            }
            Serial.println(F("     │"));
        }
    }
    Serial.println(F("└──────────┴──────┴───────┴───────────┘"));
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
// STATUS
// ════════════════════════════════════════════════════════════════════════════

void print_status() {
    Serial.println();
    Serial.println(F("╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║              RELAY NODE STATUS                   ║"));
    Serial.println(F("╠══════════════════════════════════════════════════╣"));
    Serial.print(F("║ Node: 0x"));
    Serial.print(MY_NODE_ID, HEX);
    Serial.print(F("   State: "));
    if (node_state == STATE_ORPHAN) Serial.print(F("ORPHAN    "));
    else if (node_state == STATE_CONNECTED) Serial.print(F("CONNECTED "));
    else Serial.print(F("SWITCHING "));
    Serial.println(F("         ║"));
    
    Serial.print(F("║ Rank: "));
    Serial.print(my_rank);
    Serial.print(F("      Parent: "));
    if (parent_id) {
        Serial.print(F("0x"));
        if (parent_id < 0x10) Serial.print(F("0"));
        Serial.print(parent_id, HEX);
    } else {
        Serial.print(F("none"));
    }
    Serial.println(F("                    ║"));
    
    Serial.print(F("║ Forwarded: "));
    Serial.print(packets_forwarded);
    Serial.print(F("  Dropped: "));
    Serial.print(packets_dropped);
    Serial.println(F("                    ║"));
    
    Serial.println(F("╚══════════════════════════════════════════════════╝"));
    
    print_neighbor_table();
}
