// ============================================================================
// MESH COMMON FUNCTIONS -- SHARED
// CSC2106 Group 33 -- LoRa Mesh Network
//
// Functions that are IDENTICAL across sensor and relay nodes:
//   - Deduplication (is_duplicate, record_dedup)
//   - ACK send/wait
//   - Parent scoring (score_parent)
//   - Beacon processing (process_beacon)
//   - Queue percentage (compute_queue_pct)
//
// Each .ino must #define NODE_ID before including this header.
// Each .ino must define the extern variables and callbacks listed below.
// ============================================================================

#ifndef MESH_COMMON_H
#define MESH_COMMON_H

#include <Arduino.h>
#include "mesh_protocol.h"

// NODE_ID must be defined by each .ino before including this header
#ifndef NODE_ID
  #error "Define NODE_ID before including mesh_common.h"
#endif

// ============================================================================
// PARENT CANDIDATE TRACKING
// Used by both sensor and relay nodes to track potential parents.
// ============================================================================

struct ParentInfo {
    uint8_t  node_id;        // ID of this candidate
    int8_t   rssi;           // last RSSI from this candidate's beacon
    uint8_t  rank;           // rank reported in beacon (0=edge, 1=relay, ...)
    uint8_t  queue_pct;      // queue occupancy reported in beacon
    uint8_t  parent_health;  // parent_health from beacon (0=no route, 100=healthy)
    uint32_t last_seen_ms;   // millis() when we last heard a beacon
    bool     valid;          // have we ever heard from this candidate?
    uint8_t  fail_count;     // consecutive send/forward failure rounds (strikes)
};

// ============================================================================
// FORWARDING QUEUE
// ============================================================================

#define MAX_PENDING_FORWARDS  10  // denominator for queue_pct calculation

// ============================================================================
// EXTERN DECLARATIONS
// Each .ino defines these variables.
// ============================================================================

extern ParentInfo    candidates[MAX_CANDIDATES];
extern DedupEntry    dedup_table[DEDUP_TABLE_SIZE];
extern uint8_t       dedup_head;
extern uint8_t       my_rank;
extern volatile bool rxFlag;
extern SX1262        radio;
extern volatile bool ack_received;
extern uint8_t       ack_expected_seq;
extern bool          waiting_for_ack;
extern volatile uint8_t pending_forwards;

// Parent whitelist — if non-empty, only accept beacons from these node IDs.
// Each .ino defines its own list (e.g., sensors only accept relay + primary edge).
// Edge nodes set count=0 to accept all (they don't select parents anyway).
extern const uint8_t  allowed_parents[];
extern const uint8_t  allowed_parents_count;

// ============================================================================
// EXTERN FUNCTION DECLARATIONS
// Each .ino provides its own implementation of these.
// ============================================================================

extern void receive_and_process();
extern void on_beacon_updated();  // sensor calls update_parent_selection(), relay calls update_parent_set()

// ============================================================================
// DEDUPLICATION
// ============================================================================

// Returns true if (src_id, seq_num) was seen within the last DEDUP_WINDOW_MS.
// Expires old entries as it scans.
inline bool is_duplicate(uint8_t src_id, uint8_t seq_num) {
    uint32_t now = millis();
    for (int i = 0; i < DEDUP_TABLE_SIZE; i++) {
        if (!dedup_table[i].valid) continue;
        // Expire old entries
        if (now - dedup_table[i].timestamp > DEDUP_WINDOW_MS) {
            dedup_table[i].valid = false;
            continue;
        }
        if (dedup_table[i].src_id  == src_id &&
            dedup_table[i].seq_num == seq_num) {
            return true;
        }
    }
    return false;
}

// Writes (src_id, seq_num) into the circular dedup table.
inline void record_dedup(uint8_t src_id, uint8_t seq_num) {
    dedup_table[dedup_head].src_id    = src_id;
    dedup_table[dedup_head].seq_num   = seq_num;
    dedup_table[dedup_head].timestamp = millis();
    dedup_table[dedup_head].valid     = true;
    dedup_head = (dedup_head + 1) % DEDUP_TABLE_SIZE;
}

// ============================================================================
// SEND ACK
// Sends a 10-byte PKT_TYPE_ACK (no payload) to dst_id.
// ============================================================================

inline void send_ack(uint8_t dst_id, uint8_t seq_num) {
    uint8_t ack[MESH_HEADER_SIZE];

    ack[0] = PKT_TYPE_ACK;   // flags = ACK type
    ack[1] = NODE_ID;         // src = this node
    ack[2] = dst_id;          // dst = the node that sent us the DATA
    ack[3] = NODE_ID;         // prev_hop = this node
    ack[4] = 1;               // ttl = 1 (ACK only needs one hop)
    ack[5] = seq_num;         // echo back the same seq_num
    ack[6] = my_rank;         // our dynamic rank
    ack[7] = 0;               // payload_len = 0 (pure ACK)

    // CRC big-endian at bytes 8-9
    uint16_t crc = crc16_ccitt(ack, 8);
    ack[8] = (crc >> 8) & 0xFF;
    ack[9] = crc & 0xFF;

    lbt_transmit(radio, ack, MESH_HEADER_SIZE);

    Serial.print("ACK  | to=0x");
    Serial.print(dst_id, HEX);
    Serial.print(" | for_seq=");
    Serial.println(seq_num);

    radio.startReceive();
}

// ============================================================================
// WAIT FOR ACK
// Polls receive_and_process() for up to timeout_ms, checking ack_received.
// Returns true if ACK matching ack_expected_seq is received before timeout.
// ============================================================================

inline bool wait_for_ack(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        receive_and_process();
        if (ack_received) return true;
        delay(1);
    }
    return false;
}

// ============================================================================
// PARENT SCORING
// score = RANK% x rank_score + RSSI% x rssi_score + QUEUE% x queue_score
// Weights defined in mesh_protocol.h (SCORE_WEIGHT_RANK/RSSI/QUEUE).
//
// rank_score  : rank=0 (edge) -> 100, rank=1 (relay) -> 85, rank=2 -> 70, ...
// rssi_score  : maps RSSI [-120, -60] -> [0, 100]
// queue_score : lower is better; 100 - queue_pct
// ============================================================================

inline int score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct,
                        uint8_t parent_health = 100, bool debug = false,
                        uint8_t node_id = 0) {
    // Rank score
    int rank_score = (rank == 0) ? 100 : max(0, 100 - (rank * 15));

    // RSSI score: clamp to [-120, -60], map to [0, 100]
    int rssi_clamped = rssi;
    if (rssi_clamped < -120) rssi_clamped = -120;
    if (rssi_clamped > -60)  rssi_clamped = -60;
    int rssi_score = (rssi_clamped + 120) * 100 / 60;

    // Queue score
    int queue_score = (100 - (int)queue_pct);

    int total = (SCORE_WEIGHT_RANK  * rank_score  / 100)
              + (SCORE_WEIGHT_RSSI  * rssi_score  / 100)
              + (SCORE_WEIGHT_QUEUE * queue_score  / 100);

    // Queue penalty: keep edge parents discoverable during temporary congestion.
    // Non-edge parents receive a stronger penalty when near-full.
    bool penalized = false;
    if (queue_pct >= 80) {
        if (rank == RANK_EDGE) {
            total = (total * 70) / 100;
        } else {
            total = total / 2;
        }
        penalized = true;
    }

    // No-route penalty: if candidate has no route to edge (parent_health=0),
    // hard-reject so nodes prefer candidates with a live forwarding path.
    bool no_route = false;
    if (parent_health == 0 && rank > 0) {
        total = 0;
        no_route = true;
    }

    // Debug output showing score breakdown
    if (debug) {
        Serial.print("SCORE | 0x");
        Serial.print(node_id, HEX);
        Serial.print(" | rank=");
        Serial.print(rank);
        Serial.print(" (");
        Serial.print(SCORE_WEIGHT_RANK * rank_score / 100);
        Serial.print(") | rssi=");
        Serial.print(rssi);
        Serial.print(" (");
        Serial.print(SCORE_WEIGHT_RSSI * rssi_score / 100);
        Serial.print(") | queue=");
        Serial.print(queue_pct);
        Serial.print("% (");
        Serial.print(SCORE_WEIGHT_QUEUE * queue_score / 100);
        Serial.print(") | health=");
        Serial.print(parent_health);
        Serial.print(" | total=");
        Serial.print(total);
        if (penalized) Serial.print(" [QUEUE_PENALIZED]");
        if (no_route) Serial.print(" [NO_ROUTE]");
        Serial.println();
    }

    return total;
}

// ============================================================================
// PROCESS BEACON
// Parses a beacon packet, updates the candidate table, then calls
// on_beacon_updated() which each node implements differently:
//   - sensor_node: calls update_parent_selection()
//   - relay_node:  calls update_parent_set()
// ============================================================================

inline void process_beacon(uint8_t *buf, int len, int rssi) {
    uint8_t src_id      = buf[1];
    uint8_t rank        = buf[6];
    uint8_t payload_len = buf[7];

    // Ignore our own beacons (safety check)
    if (src_id == NODE_ID) return;

    // Whitelist filter: if allowed_parents is non-empty, only accept listed nodes.
    // This enforces the intended topology (e.g., sensors must go through relay,
    // not shortcut directly to the secondary edge).
    if (allowed_parents_count > 0) {
        bool allowed = false;
        for (uint8_t i = 0; i < allowed_parents_count; i++) {
            if (src_id == allowed_parents[i]) { allowed = true; break; }
        }
        if (!allowed) {
            Serial.print("BCN | WHITELIST_REJECT src=0x");
            Serial.print(src_id, HEX);
            Serial.println(" | not in allowed_parents");
            return;
        }
    }

    // Parse beacon payload fields
    uint8_t queue_pct = 0;
    uint8_t health    = 0;
    if (len >= MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE && payload_len >= BEACON_PAYLOAD_SIZE) {
        queue_pct = buf[MESH_HEADER_SIZE + 1];
        health    = buf[MESH_HEADER_SIZE + 3];
    }

    // RSSI floor: reject beacons from nodes with unusable signal strength
    if (rssi < MIN_PARENT_RSSI) {
        Serial.print("BCN | REJECTED src=0x");
        Serial.print(src_id, HEX);
        Serial.print(" | rssi=");
        Serial.print(rssi);
        Serial.print(" < floor=");
        Serial.println(MIN_PARENT_RSSI);
        return;
    }

    // Log beacon reception
    Serial.print("BCN | src=0x");
    Serial.print(src_id, HEX);
    Serial.print(" | rank=");
    Serial.print(rank);
    Serial.print(" | rssi=");
    Serial.print(rssi);
    Serial.print(" | queue=");
    Serial.print(queue_pct);
    Serial.print("% | health=");
    Serial.println(health);

    // Find existing entry or empty slot in candidate table
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
    if (slot == -1) {
        // Table full -- evict last slot
        slot = MAX_CANDIDATES - 1;
    }

    // Update candidate fields
    candidates[slot].node_id       = src_id;
    candidates[slot].rssi          = (int8_t)rssi;
    candidates[slot].rank          = rank;
    candidates[slot].queue_pct     = queue_pct;
    candidates[slot].parent_health = health;
    candidates[slot].last_seen_ms  = millis();
    candidates[slot].valid         = true;
    candidates[slot].fail_count    = 0;  // fresh beacon resets strikes

    // Notify the node-specific parent selection logic
    on_beacon_updated();
}

// ============================================================================
// COMPUTE QUEUE PERCENTAGE
// ============================================================================

inline uint8_t compute_queue_pct() {
    return (uint8_t)((pending_forwards * 100) / MAX_PENDING_FORWARDS);
}

#endif // MESH_COMMON_H
