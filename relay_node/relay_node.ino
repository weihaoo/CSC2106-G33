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
#include "../shared/logging.h"

// -----------------------------------------------------------------------------
// NODE CONFIGURATION
// Must be defined BEFORE including mesh_common.h
// -----------------------------------------------------------------------------
#define NODE_ID       0x02    // Relay node

// -----------------------------------------------------------------------------
// PARENT WHITELIST (Optional - for forced multi-hop testing)
// Uncomment to force this relay to ONLY accept specific parent(s).
// Useful for Scenario 3 (Multi-Hop Chain) to force a linear topology.
//
// EXAMPLE:
//   Relay 0x02: Force to use Edge 0x01 only → #define PARENT_WHITELIST {0x01}
//
// To disable: Keep the line commented out (normal auto-parent selection).
// -----------------------------------------------------------------------------
// #define PARENT_WHITELIST {0x01}  // <-- Uncomment and set allowed parent(s)

// -----------------------------------------------------------------------------
// SHARED HEADERS (mesh_radio.h provides init_pmu, init_radio, pin defs, ISR;
//                 mesh_common.h provides dedup, send_ack, wait_for_ack,
//                 score_parent, process_beacon, compute_queue_pct)
// -----------------------------------------------------------------------------
#include "../shared/mesh_radio.h"
#include "../shared/mesh_common.h"

// -----------------------------------------------------------------------------
// TIMING PARAMETERS (node-specific; shared ones come from mesh_protocol.h)
// -----------------------------------------------------------------------------
// Beacon phase offset — spreads beacons across the 10s window to avoid collisions
#define BEACON_PHASE_OFFSET_MS  ((NODE_ID % 5) * 2000UL)

// -----------------------------------------------------------------------------
// RELAY ROUTING HEADER (must come before globals — defines ParentSetEntry struct)
// -----------------------------------------------------------------------------
#include "relay_routing.h"

// ============================================================================
// GLOBAL VARIABLE DEFINITIONS
// These satisfy the extern declarations in mesh_radio.h and mesh_common.h.
// ============================================================================

XPowersAXP2101 PMU;
SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);
volatile bool rxFlag = false;

// Parent tracking (DAG: multiple active parents)
ParentInfo candidates[MAX_CANDIDATES];
uint8_t    my_rank = RANK_RELAY;

// DAG parent set
ParentSetEntry parent_set[MAX_ACTIVE_PARENTS];
uint8_t active_parent_count = 0;

// Dedup table (circular buffer)
DedupEntry dedup_table[DEDUP_TABLE_SIZE];
uint8_t    dedup_head = 0;

// Forwarding queue counter
volatile uint8_t pending_forwards = 0;

// ACK tracking — for forward_packet() to detect ACKs from parent
volatile bool ack_received = false;
uint8_t       ack_expected_seq = 0;
bool          waiting_for_ack = false;

// ============================================================================
// LOCAL HEADERS (must come after global definitions they reference)
// ============================================================================
#include "relay_packets.h"

// =============================================================================
// BROADCAST BEACON
// =============================================================================
void broadcast_beacon() {
    // Suppress beacon if orphaned — prevents attracting children into a black hole
    if (active_parent_count == 0) {
        LOG_BEACON("Suppressed — no active parents (orphaned)");
        return;
    }

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

    radio.startReceive();
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    log_boot_banner("Relay Node");

    char buf[64];
    sprintf(buf, "Firmware %s | Node 0x%02X", FIRMWARE_VERSION, NODE_ID);
    LOG_INFO(buf);

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
    static uint32_t last_stale_check_ms = millis();

    uint32_t now = millis();

    // --- Step 1: Broadcast beacon if due ---
    if (now - last_beacon_ms >= BEACON_INTERVAL_MS) {
        last_beacon_ms = now;
        broadcast_beacon();
    }

    // --- Step 2: Check for stale parents (self-healing) ---
    if (now - last_stale_check_ms >= 5000UL) {  // Check every 5s
        last_stale_check_ms = now;
        check_parent_staleness();
    }

    // --- Step 3: Process incoming packets ---
    receive_and_process();
}
