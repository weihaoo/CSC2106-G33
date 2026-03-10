// =============================================================================
// CSC2106 Group 33 — LoRa Mesh Relay Node
// Singapore Institute of Technology
//
// File   : relay_node.ino
// Role   : Relay Node (Person 2)
// NodeID : 0x02
//
// Hardware : LilyGo T-Beam (SX1262, 923 MHz)
// Libraries: RadioLib, XPowersLib
//
// Core behaviour:
//   - Beacons immediately at boot (rank=1) so sensors can find it
//   - Listens for packets from sensor nodes
//   - Validates (length, self, dedup, CRC, TTL) then ACKs sender
//   - Forwards packet to parent edge node(s) — DAG: multiple active parents
//   - Maintains parent SET (not single parent) for load balancing
//   - Selects parent per-packet using weighted random selection
//   - Payload-agnostic forwarding — never reads beyond byte 9 of MeshHeader
// =============================================================================

#define NODE_NAME "RELAY-02"
#include "logging.h"
#include "mesh_protocol.h"

#include <Wire.h>
#include <XPowersLib.h>
#include <RadioLib.h>

// -----------------------------------------------------------------------------
// NODE CONFIGURATION
// -----------------------------------------------------------------------------
#define NODE_ID       0x02    // Relay node

// Beacon phase offset — spreads beacons across the 10s window to avoid collisions
#define BEACON_PHASE_OFFSET_MS  ((NODE_ID % 5) * 2000UL)

// -----------------------------------------------------------------------------
// LoRa RADIO PINS (T-Beam SX1262)
// -----------------------------------------------------------------------------
#define RADIO_NSS   18
#define RADIO_DIO1  33
#define RADIO_RST   23
#define RADIO_BUSY  32

// -----------------------------------------------------------------------------
// FORWARDING QUEUE COUNTER
// -----------------------------------------------------------------------------
volatile uint8_t pending_forwards = 0;
#define MAX_PENDING_FORWARDS  10

// -----------------------------------------------------------------------------
// DAG PARENT SET (MULTIPLE ACTIVE PARENTS)
// -----------------------------------------------------------------------------
struct ParentSetEntry {
    uint8_t  node_id;
    int      score;
    uint32_t last_seen_ms;
    bool     active;
};

// Candidate table (all potential parents)
struct ParentInfo {
    uint8_t  node_id;
    int8_t   rssi;
    uint8_t  rank;
    uint8_t  queue_pct;
    uint32_t last_seen_ms;
    bool     valid;
};

ParentInfo candidates[MAX_CANDIDATES];
ParentSetEntry parent_set[MAX_ACTIVE_PARENTS];
uint8_t active_parent_count = 0;
uint8_t my_rank = RANK_RELAY;

// -----------------------------------------------------------------------------
// DEDUP TABLE
// -----------------------------------------------------------------------------
DedupEntry dedup_table[DEDUP_TABLE_SIZE];
uint8_t    dedup_head = 0;

// -----------------------------------------------------------------------------
// DIO1 RECEIVE INTERRUPT FLAG
// -----------------------------------------------------------------------------
volatile bool rxFlag = false;

void IRAM_ATTR setRxFlag() {
    rxFlag = true;
}

// -----------------------------------------------------------------------------
// OBJECTS
// -----------------------------------------------------------------------------
XPowersAXP2101 PMU;
SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);

// -----------------------------------------------------------------------------
// FORWARD DECLARATIONS
// -----------------------------------------------------------------------------
void     init_pmu();
void     init_radio();
void     receive_and_process();
bool     is_duplicate(uint8_t src_id, uint8_t seq_num);
void     record_dedup(uint8_t src_id, uint8_t seq_num);
void     send_ack(uint8_t dst_id, uint8_t seq_num);
void     forward_packet(uint8_t *buf, int len);
void     broadcast_beacon();
void     process_beacon(uint8_t *buf, int len, int rssi);
int      score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct);
void     update_parent_set();
uint8_t  select_parent_for_packet();
bool     is_stale(uint32_t last_seen);
uint8_t  compute_queue_pct();

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    
    log_boot_banner("Relay Node");
    
    // Initialise tables
    for (int i = 0; i < DEDUP_TABLE_SIZE; i++) {
        dedup_table[i].valid = false;
    }
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        candidates[i].valid = false;
    }
    for (int i = 0; i < MAX_ACTIVE_PARENTS; i++) {
        parent_set[i].active = false;
    }
    
    LOG_INFO("Initializing PMU (AXP2101)...");
    init_pmu();
    LOG_OK("PMU initialized - LoRa radio powered");
    
    LOG_INFO("Initializing LoRa radio...");
    init_radio();
    
    char buf[64];
    sprintf(buf, "Radio ready: %.1f MHz, SF%d, BW%.0f kHz", 
            LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH);
    LOG_OK(buf);
    
    // Apply beacon phase offset
    sprintf(buf, "Beacon phase offset: %lu ms", BEACON_PHASE_OFFSET_MS);
    LOG_INFO(buf);
    delay(BEACON_PHASE_OFFSET_MS);
    
    // Beacon immediately
    broadcast_beacon();
    
    LOG_INFO("Relay ready. Listening for packets...");
    LOG_INFO("DAG mode: Multiple active parents supported");
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
    static uint32_t last_beacon_ms = millis();
    
    uint32_t now = millis();
    if (now - last_beacon_ms >= BEACON_INTERVAL_MS) {
        last_beacon_ms = now;
        broadcast_beacon();
    }
    
    receive_and_process();
}

// =============================================================================
// PMU INITIALISATION
// =============================================================================
void init_pmu() {
    Wire.begin(21, 22);
    if (!PMU.begin(Wire, 0x34, 21, 22)) {
        LOG_ERROR("PMU init failed! Check board wiring.");
        while (true) delay(1000);
    }
    PMU.setALDO2Voltage(3300);
    PMU.enableALDO2();
    delay(10);
}

// =============================================================================
// RADIO INITIALISATION
// =============================================================================
void init_radio() {
    int state = radio.begin(LORA_FREQUENCY);
    if (state != RADIOLIB_ERR_NONE) {
        LOG_ERROR("RadioLib init failed");
        char buf[32];
        sprintf(buf, "Error code: %d", state);
        LOG_ERROR(buf);
        while (true) delay(1000);
    }
    radio.setSpreadingFactor(LORA_SPREADING);
    radio.setBandwidth(LORA_BANDWIDTH);
    radio.setCodingRate(LORA_CODING_RATE);
    radio.setSyncWord(LORA_SYNC_WORD);
    radio.setOutputPower(LORA_TX_POWER);
    radio.setDio1Action(setRxFlag);
    rxFlag = false;
    radio.startReceive();
}

// =============================================================================
// RECEIVE AND PROCESS
// =============================================================================
void receive_and_process() {
    if (!rxFlag) return;
    rxFlag = false;
    
    int len = radio.getPacketLength();
    uint8_t buf[64];
    int state = radio.readData(buf, sizeof(buf));
    int rssi = (int)radio.getRSSI();
    
    rxFlag = false;
    radio.startReceive();
    
    if (state != RADIOLIB_ERR_NONE) {
        LOG_DROP("Radio read error");
        return;
    }
    
    // Validation 1: Minimum length
    if (len < MESH_HEADER_SIZE) {
        LOG_DROP("Packet too short");
        return;
    }
    
    // Extract header fields
    uint8_t src_id  = buf[1];
    uint8_t dst_id  = buf[2];
    uint8_t flags   = buf[0];
    uint8_t seq     = buf[5];
    uint8_t ttl     = buf[4];
    uint8_t rank    = buf[6];
    
    // Validation 2: Ignore own packets
    if (src_id == NODE_ID) {
        return;
    }
    
    // Check beacon
    if (GET_PKT_TYPE(flags) == PKT_TYPE_BEACON) {
        if (rank < my_rank) {
            process_beacon(buf, len, rssi);
        }
        return;
    }
    
    // Drop ACK packets
    if (GET_PKT_TYPE(flags) == PKT_TYPE_ACK) {
        return;
    }
    
    // Validation 3: Destination check
    if (dst_id != NODE_ID) {
        return;
    }
    
    // Validation 4: Dedup check
    if (is_duplicate(src_id, seq)) {
        char msg[64];
        sprintf(msg, "Duplicate packet #%d from %s", seq, node_name(src_id));
        LOG_DROP(msg);
        return;
    }
    
    // Validation 5: CRC check
    uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
    uint16_t computed_crc = crc16_ccitt(buf, 8);
    if (received_crc != computed_crc) {
        char msg[64];
        sprintf(msg, "CRC fail on packet #%d from %s", seq, node_name(src_id));
        LOG_DROP(msg);
        return;
    }
    
    // Validation 6: TTL check
    if (ttl == 0) {
        char msg[64];
        sprintf(msg, "TTL expired on packet #%d from %s", seq, node_name(src_id));
        LOG_DROP(msg);
        return;
    }
    
    // Record in dedup table
    record_dedup(src_id, seq);
    
    // ACK the sender
    if (flags & PKT_FLAG_ACK_REQ) {
        send_ack(buf[3], seq);
    }
    
    // Forward the packet (DAG selection)
    forward_packet(buf, len);
}

// =============================================================================
// IS DUPLICATE
// =============================================================================
bool is_duplicate(uint8_t src_id, uint8_t seq_num) {
    uint32_t now = millis();
    for (int i = 0; i < DEDUP_TABLE_SIZE; i++) {
        if (!dedup_table[i].valid) continue;
        if (now - dedup_table[i].timestamp > DEDUP_WINDOW_MS) {
            dedup_table[i].valid = false;
            continue;
        }
        if (dedup_table[i].src_id == src_id &&
            dedup_table[i].seq_num == seq_num) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// RECORD DEDUP
// =============================================================================
void record_dedup(uint8_t src_id, uint8_t seq_num) {
    dedup_table[dedup_head].src_id = src_id;
    dedup_table[dedup_head].seq_num = seq_num;
    dedup_table[dedup_head].timestamp = millis();
    dedup_table[dedup_head].valid = true;
    dedup_head = (dedup_head + 1) % DEDUP_TABLE_SIZE;
}

// =============================================================================
// SEND ACK
// =============================================================================
void send_ack(uint8_t dst_id, uint8_t seq_num) {
    uint8_t ack[MESH_HEADER_SIZE];
    
    ack[0] = PKT_TYPE_ACK;
    ack[1] = NODE_ID;
    ack[2] = dst_id;
    ack[3] = NODE_ID;
    ack[4] = 1;
    ack[5] = seq_num;
    ack[6] = my_rank;
    ack[7] = 0;
    
    uint16_t crc = crc16_ccitt(ack, 8);
    ack[8] = (crc >> 8) & 0xFF;
    ack[9] = crc & 0xFF;
    
    lbt_transmit(radio, ack, MESH_HEADER_SIZE);
    
    char msg[64];
    sprintf(msg, "Sent ACK for packet #%d to %s", seq_num, node_name(dst_id));
    LOG_ACK(msg);
    
    rxFlag = false;
    radio.startReceive();
}

// =============================================================================
// FORWARD PACKET (DAG - Multiple Parent Selection)
// =============================================================================
void forward_packet(uint8_t *buf, int len) {
    log_separator_packet();
    
    // DAG: Select parent from active parent set using weighted random
    uint8_t target = select_parent_for_packet();
    
    if (target == 0xFF) {
        LOG_DROP("No active parents in DAG set");
        return;
    }
    
    uint8_t src_id = buf[1];
    uint8_t seq = buf[5];
    uint8_t ttl = buf[4];
    uint8_t payload_len = buf[7];
    
    // Log the forwarding action
    char msg[80];
    sprintf(msg, "Forwarding packet #%d to %s", seq, node_name(target));
    LOG_FWD(msg);
    
    sprintf(msg, "Source: %s | TTL: %d→%d | Payload: %d bytes", 
            node_name(src_id), ttl, ttl-1, payload_len);
    log_detail(msg);
    
    // Show DAG selection info
    sprintf(msg, "DAG selection: %d active parents, chose %s (weighted random)", 
            active_parent_count, node_name(target));
    LOG_DAG(msg);
    
    // Update counters
    if (pending_forwards < MAX_PENDING_FORWARDS) pending_forwards++;
    
    // Modify header
    buf[0] = (buf[0] | PKT_FLAG_FWD) & ~PKT_FLAG_ACK_REQ;
    buf[2] = target;
    buf[3] = NODE_ID;
    buf[4] = ttl - 1;
    
    // Recalculate CRC
    uint16_t crc = crc16_ccitt(buf, 8);
    buf[8] = (crc >> 8) & 0xFF;
    buf[9] = crc & 0xFF;
    
    // Transmit
    int total_len = MESH_HEADER_SIZE + payload_len;
    if (total_len > len) {
        LOG_ERROR("Payload length exceeds packet size");
        if (pending_forwards > 0) pending_forwards--;
        return;
    }
    
    int state = lbt_transmit(radio, buf, total_len);
    if (state != RADIOLIB_ERR_NONE) {
        LOG_ERROR("Forward transmission failed");
    }
    
    if (pending_forwards > 0) pending_forwards--;
    
    rxFlag = false;
    radio.startReceive();
    
    log_separator_packet();
}

// =============================================================================
// BROADCAST BEACON
// =============================================================================
void broadcast_beacon() {
    uint8_t beacon[MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE];
    
    uint8_t qpct = compute_queue_pct();
    
    // Propagate downstream congestion
    for (int i = 0; i < active_parent_count; i++) {
        if (parent_set[i].active) {
            // Find candidate with this node_id to get queue_pct
            for (int j = 0; j < MAX_CANDIDATES; j++) {
                if (candidates[j].valid && candidates[j].node_id == parent_set[i].node_id) {
                    if (candidates[j].queue_pct > qpct) qpct = candidates[j].queue_pct;
                }
            }
        }
    }
    
    // Compute average link quality from active parents
    uint8_t link_quality = 0;
    int quality_sum = 0;
    int quality_count = 0;
    for (int i = 0; i < active_parent_count; i++) {
        if (parent_set[i].active) {
            for (int j = 0; j < MAX_CANDIDATES; j++) {
                if (candidates[j].valid && candidates[j].node_id == parent_set[i].node_id) {
                    int8_t rssi = candidates[j].rssi;
                    int clamped = rssi;
                    if (clamped < -120) clamped = -120;
                    if (clamped > -60) clamped = -60;
                    quality_sum += (clamped + 120) * 100 / 60;
                    quality_count++;
                }
            }
        }
    }
    if (quality_count > 0) {
        link_quality = quality_sum / quality_count;
    }
    
    // Build beacon header
    beacon[0] = PKT_TYPE_BEACON;
    beacon[1] = NODE_ID;
    beacon[2] = 0xFF;
    beacon[3] = NODE_ID;
    beacon[4] = 1;
    beacon[5] = 0x00;
    beacon[6] = my_rank;
    beacon[7] = BEACON_PAYLOAD_SIZE;
    
    uint16_t crc = crc16_ccitt(beacon, 8);
    beacon[8] = (crc >> 8) & 0xFF;
    beacon[9] = crc & 0xFF;
    
    // Beacon payload
    beacon[10] = 0x01;
    beacon[11] = qpct;
    beacon[12] = link_quality;
    beacon[13] = (active_parent_count > 0) ? 100 : 0;
    
    lbt_transmit(radio, beacon, MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE);
    
    char msg[80];
    sprintf(msg, "Broadcasting beacon | Rank: %d | Queue: %d%% | Active parents: %d", 
            my_rank, qpct, active_parent_count);
    LOG_BEACON(msg);
    
    rxFlag = false;
    radio.startReceive();
}

// =============================================================================
// PROCESS BEACON
// =============================================================================
void process_beacon(uint8_t *buf, int len, int rssi) {
    uint8_t src_id = buf[1];
    uint8_t rank = buf[6];
    uint8_t pay_len = buf[7];
    uint8_t queue_pct = 0;
    
    if (len >= MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE && pay_len >= BEACON_PAYLOAD_SIZE) {
        queue_pct = buf[MESH_HEADER_SIZE + 1];
    }
    
    char msg[80];
    sprintf(msg, "Received beacon from %s", node_name(src_id));
    LOG_BEACON(msg);
    
    sprintf(msg, "Signal: %d dBm | Rank: %d | Queue: %d%%", rssi, rank, queue_pct);
    log_detail(msg);
    
    // Update candidate table
    int slot = -1;
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (candidates[i].valid && candidates[i].node_id == src_id) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        for (int i = 0; i < MAX_CANDIDATES; i++) {
            if (!candidates[i].valid) {
                slot = i;
                break;
            }
        }
    }
    if (slot == -1) slot = 0;
    
    candidates[slot].node_id = src_id;
    candidates[slot].rssi = (int8_t)rssi;
    candidates[slot].rank = rank;
    candidates[slot].queue_pct = queue_pct;
    candidates[slot].last_seen_ms = millis();
    candidates[slot].valid = true;
    
    // Update DAG parent set
    update_parent_set();
}

// =============================================================================
// SCORE PARENT
// =============================================================================
int score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct) {
    int rank_score = (rank == 0) ? 100 : max(0, 100 - (rank * 15));
    
    int rssi_clamped = rssi;
    if (rssi_clamped < -120) rssi_clamped = -120;
    if (rssi_clamped > -60) rssi_clamped = -60;
    int rssi_score = (rssi_clamped + 120) * 100 / 60;
    
    int queue_score = (100 - (int)queue_pct);
    
    int total = (60 * rank_score / 100)
              + (25 * rssi_score / 100)
              + (15 * queue_score / 100);
    
    if (queue_pct >= 80) {
        total = total / 2;
    }
    
    return total;
}

// =============================================================================
// UPDATE PARENT SET (DAG)
// =============================================================================
void update_parent_set() {
    uint32_t now = millis();
    active_parent_count = 0;
    
    // Score all candidates and build active parent set
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (!candidates[i].valid) continue;
        if (is_stale(candidates[i].last_seen_ms)) continue;
        
        int score = score_parent(candidates[i].rank,
                                  candidates[i].rssi,
                                  candidates[i].queue_pct);
        
        if (score >= PARENT_THRESHOLD_SCORE) {
            if (active_parent_count < MAX_ACTIVE_PARENTS) {
                parent_set[active_parent_count].node_id = candidates[i].node_id;
                parent_set[active_parent_count].score = score;
                parent_set[active_parent_count].last_seen_ms = candidates[i].last_seen_ms;
                parent_set[active_parent_count].active = true;
                active_parent_count++;
            }
        }
    }
    
    // Update my_rank based on best parent
    int best_score = -1;
    uint8_t best_rank = 255;
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (candidates[i].valid && !is_stale(candidates[i].last_seen_ms)) {
            int s = score_parent(candidates[i].rank, candidates[i].rssi, candidates[i].queue_pct);
            if (s > best_score) {
                best_score = s;
                best_rank = candidates[i].rank;
            }
        }
    }
    if (best_rank != 255) {
        my_rank = best_rank + 1;
    }
    
    // Log parent set
    char msg[80];
    sprintf(msg, "Parent set updated: %d active parents", active_parent_count);
    LOG_PARENT(msg);
    
    for (int i = 0; i < active_parent_count; i++) {
        sprintf(msg, "  [%d] %s (score: %d)", 
                i+1, node_name(parent_set[i].node_id), parent_set[i].score);
        log_detail(msg);
    }
}

// =============================================================================
// SELECT PARENT FOR PACKET (WEIGHTED RANDOM)
// =============================================================================
uint8_t select_parent_for_packet() {
    if (active_parent_count == 0) return 0xFF;
    if (active_parent_count == 1) return parent_set[0].node_id;
    
    // Weighted random selection based on score
    int total_score = 0;
    for (int i = 0; i < active_parent_count; i++) {
        total_score += parent_set[i].score;
    }
    
    if (total_score == 0) return parent_set[0].node_id;
    
    int r = random(0, total_score);
    int cumulative = 0;
    for (int i = 0; i < active_parent_count; i++) {
        cumulative += parent_set[i].score;
        if (r < cumulative) {
            return parent_set[i].node_id;
        }
    }
    
    return parent_set[active_parent_count - 1].node_id;
}

// =============================================================================
// IS STALE
// =============================================================================
bool is_stale(uint32_t last_seen) {
    return (millis() - last_seen) > PARENT_TIMEOUT_MS;
}

// =============================================================================
// COMPUTE QUEUE PERCENT
// =============================================================================
uint8_t compute_queue_pct() {
    return (uint8_t)((pending_forwards * 100) / MAX_PENDING_FORWARDS);
}
