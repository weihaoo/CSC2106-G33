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
//   - Forwards packet to parent edge node (payload-agnostic — never reads
//     beyond byte 9 of the MeshHeader)
//   - Selects parent (Edge 1 or Edge 2) via beacon scoring
//   - Drops packets if no parent selected yet
// =============================================================================

#include <Wire.h>
#include <XPowersLib.h>
#include <RadioLib.h>
#include "mesh_protocol.h"

// -----------------------------------------------------------------------------
// NODE CONFIGURATION
// -----------------------------------------------------------------------------
#define NODE_ID       0x02    // Relay node

// Beacon phase offset — spreads beacons across the 10s window to avoid collisions
// relay NODE_ID=0x02, so offset = (2 % 5) * 2000 = 4000ms
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
// Simple counter tracking how many packets are pending forward.
// Used to compute queue_pct for beacons.
// -----------------------------------------------------------------------------
volatile uint8_t pending_forwards = 0;
#define MAX_PENDING_FORWARDS  10  // denominator for queue_pct calculation

// -----------------------------------------------------------------------------
// PARENT TRACKING
// -----------------------------------------------------------------------------
struct ParentInfo {
  uint8_t  node_id;
  int8_t   rssi;
  uint8_t  rank;
  uint8_t  queue_pct;
  uint32_t last_seen_ms;
  bool     valid;
};

ParentInfo candidates[MAX_CANDIDATES];
uint8_t current_parent_idx = 0xFF;  // 0xFF = no parent yet
uint8_t my_rank = RANK_RELAY;  // Start with design rank (1); updated to parent.rank+1 once parent found

// -----------------------------------------------------------------------------
// DEDUP TABLE (circular buffer, DEDUP_TABLE_SIZE entries, DEDUP_WINDOW_MS window)
// Uses DedupEntry from mesh_protocol.h.
// -----------------------------------------------------------------------------
DedupEntry dedup_table[DEDUP_TABLE_SIZE];
uint8_t    dedup_head = 0;  // next slot to overwrite (circular)

// ═══════════════════════════════════════════════════════════════════════════
// DIO1 RECEIVE INTERRUPT FLAG
// SX1262 signals packet arrival via DIO1 interrupt, not a polled available().
// ═══════════════════════════════════════════════════════════════════════════

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
void     update_parent_selection();
void     check_parent_timeout();
bool     has_valid_parent();
uint8_t  get_parent_id();
uint8_t  compute_queue_pct();

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== CSC2106 G33 | Relay Node 0x02 ===");

  // Initialise dedup table
  for (int i = 0; i < DEDUP_TABLE_SIZE; i++) {
    dedup_table[i].valid = false;
  }

  // Initialise candidates table
  for (int i = 0; i < MAX_CANDIDATES; i++) {
    candidates[i].valid = false;
  }

  init_pmu();
  init_radio();

  // Apply beacon phase offset before first beacon
  Serial.print("BOOT | Beacon phase offset: ");
  Serial.print(BEACON_PHASE_OFFSET_MS);
  Serial.println(" ms");
  delay(BEACON_PHASE_OFFSET_MS);

  // Beacon immediately so sensors can find us
  // (even before we have a parent edge selected)
  broadcast_beacon();

  Serial.println("BOOT | Relay ready. Listening...");
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
  static uint32_t last_beacon_ms = millis();

  // --- Beacon if due ---
  uint32_t now = millis();
  if (now - last_beacon_ms >= BEACON_INTERVAL_MS) {
    last_beacon_ms = now;
    broadcast_beacon();
  }

  // --- Check parent timeout ---
  check_parent_timeout();

  // --- Listen and process one incoming packet ---
  receive_and_process();
}

// =============================================================================
// PMU INITIALISATION
// =============================================================================
void init_pmu() {
  Wire.begin(21, 22);
  if (!PMU.begin(Wire, 0x34, 21, 22)) {  // 0x34 = AXP2101 I2C address
    Serial.println("ERROR: PMU init failed! Check board wiring.");
    while (true) delay(1000);
  }
  PMU.setALDO2Voltage(3300);
  PMU.enableALDO2();
  Serial.println("PMU OK — LoRa radio powered on via ALDO2");
  delay(10);
}

// =============================================================================
// RADIO INITIALISATION
// Uses shared radio constants from mesh_protocol.h.
// =============================================================================
void init_radio() {
  int state = radio.begin(LORA_FREQUENCY);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("ERROR: RadioLib init failed, code ");
    Serial.println(state);
    while (true) delay(1000);
  }
  radio.setSpreadingFactor(LORA_SPREADING);
  radio.setBandwidth(LORA_BANDWIDTH);
  radio.setCodingRate(LORA_CODING_RATE);
  radio.setSyncWord(LORA_SYNC_WORD);
  radio.setOutputPower(LORA_TX_POWER);
  radio.setDio1Action(setRxFlag);
  rxFlag = false;
  radio.startReceive();  // relay stays in RX by default
  Serial.print("RadioLib OK — SX1262 @ ");
  Serial.print(LORA_FREQUENCY, 0);
  Serial.println(" MHz, SF7");
}

// =============================================================================
// RECEIVE AND PROCESS
// Called every loop iteration. Reads one packet if available and handles it.
// =============================================================================
void receive_and_process() {
  if (!rxFlag) return;
  rxFlag = false;

  // Get actual packet length before readData
  int len = radio.getPacketLength();
  uint8_t buf[64];
  int state = radio.readData(buf, sizeof(buf));
  int rssi = (int)radio.getRSSI();

  // Restart receive immediately after reading
  rxFlag = false;
  radio.startReceive();

  // Check if readData succeeded
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("DROP | readData error, code ");
    Serial.println(state);
    return;
  }

  // --- Validation 1: Minimum length ---
  if (len < MESH_HEADER_SIZE) {
    Serial.println("DROP | reason=too_short");
    return;
  }

  // --- Validation 2: Ignore our own packets ---
  uint8_t src_id  = buf[1];
  uint8_t dst_id  = buf[2];
  uint8_t flags   = buf[0];
  uint8_t seq     = buf[5];
  uint8_t ttl     = buf[4];
  uint8_t rank    = buf[6];
  uint8_t pay_len = buf[7];

  if (src_id == NODE_ID) {
    // Silently discard — our own transmission echoing back
    return;
  }

  // --- Check if this is a beacon from an edge node ---
  if (GET_PKT_TYPE(flags) == PKT_TYPE_BEACON) {
    // Only accept beacons from nodes closer to the edge than us
    if (rank < my_rank) {
      process_beacon(buf, len, rssi);
    }
    return;  // beacons are never forwarded
  }

  // --- Drop ACK packets — they are point-to-point, never forwarded ---
  if (GET_PKT_TYPE(flags) == PKT_TYPE_ACK) {
    return;
  }

  // --- Only process DATA packets beyond this point ---

  // --- Validation 3: Dedup check ---
  if (is_duplicate(src_id, seq)) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=duplicate");
    return;
  }

  // --- Validation 4: CRC check (big-endian at bytes 8-9) ---
  uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
  uint16_t computed_crc = crc16_ccitt(buf, 8);
  if (received_crc != computed_crc) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=crc_fail");
    return;
  }

  // --- Validation 5: TTL check ---
  if (ttl == 0) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=ttl_drop");
    return;
  }

  // --- Record in dedup table (after all validations pass) ---
  record_dedup(src_id, seq);

  // --- ACK the sender BEFORE forwarding ---
  // Only ACK if the sender requested it
  if (flags & PKT_FLAG_ACK_REQ) {
    send_ack(buf[3], seq);  // ACK goes to prev_hop
  }

  // --- Forward the packet (payload-agnostic) ---
  if (!has_valid_parent()) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=no_parent");
    return;
  }

  forward_packet(buf, len);

  // Log successful forward
  Serial.print("FWD  | uid=");
  Serial.print(src_id, HEX);
  Serial.print(seq, HEX);
  Serial.print(" | rssi=");
  Serial.print(rssi);
  Serial.print(" | ttl=");
  Serial.print(ttl - 1);  // log the decremented TTL
  Serial.print(" | parent=");
  Serial.println(get_parent_id(), HEX);
}

// =============================================================================
// IS DUPLICATE
// Returns true if (src_id, seq_num) was seen within the last DEDUP_WINDOW_MS.
// =============================================================================
bool is_duplicate(uint8_t src_id, uint8_t seq_num) {
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

// =============================================================================
// RECORD DEDUP
// Writes (src_id, seq_num) into the circular dedup table.
// =============================================================================
void record_dedup(uint8_t src_id, uint8_t seq_num) {
  dedup_table[dedup_head].src_id    = src_id;
  dedup_table[dedup_head].seq_num   = seq_num;
  dedup_table[dedup_head].timestamp = millis();
  dedup_table[dedup_head].valid     = true;
  dedup_head = (dedup_head + 1) % DEDUP_TABLE_SIZE;  // advance circular pointer
}

// =============================================================================
// SEND ACK
// Sends a PKT_TYPE_ACK (10-byte MeshHeader, payload_len=0) to dst_id.
// Uses PKT_TYPE_ACK (0x40) — matches what sensor_node.ino waits for.
// =============================================================================
void send_ack(uint8_t dst_id, uint8_t seq_num) {
  uint8_t ack[MESH_HEADER_SIZE];

  ack[0] = PKT_TYPE_ACK;   // 0x40 — explicit ACK type
  ack[1] = NODE_ID;         // src = relay
  ack[2] = dst_id;          // dst = original sender (prev_hop from their packet)
  ack[3] = NODE_ID;         // prev_hop = relay
  ack[4] = 1;               // ttl = 1 (ACK only needs to reach next hop)
  ack[5] = seq_num;         // echo back the same seq_num
  ack[6] = my_rank;         // rank = 1 (relay)
  ack[7] = 0;               // payload_len = 0 (pure ACK)

  // Write CRC big-endian at bytes 8-9
  uint16_t crc = crc16_ccitt(ack, 8);
  ack[8] = (crc >> 8) & 0xFF;
  ack[9] = crc & 0xFF;

  int state = radio.transmit(ack, MESH_HEADER_SIZE);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.print("ACK  | to=");
    Serial.print(dst_id, HEX);
    Serial.print(" | for_seq=");
    Serial.println(seq_num);
  } else {
    Serial.print("ACK  | FAILED, code ");
    Serial.println(state);
  }

  // Return to receive mode after ACK
  rxFlag = false;
  radio.startReceive();
}

// =============================================================================
// FORWARD PACKET (Payload-Agnostic)
//
// This is the core of the project. The relay:
//   1. Sets prev_hop = NODE_ID
//   2. Sets PKT_FLAG_FWD in flags byte
//   3. Clears PKT_FLAG_ACK_REQ (edge doesn't need to ACK relay)
//   4. Decrements TTL by 1
//   5. Recalculates CRC (big-endian) over modified bytes 0-7
//   6. Copies ALL bytes (header + payload) and retransmits
//
// RULE: bytes MESH_HEADER_SIZE onward are NEVER inspected. They are copied
//       as a block. The relay does not know or care what sensor type or
//       payload format is inside. This is the payload-agnostic guarantee.
// =============================================================================
void forward_packet(uint8_t *buf, int len) {
  // Track pending forwards for beacon queue_pct
  if (pending_forwards < MAX_PENDING_FORWARDS) pending_forwards++;

  // --- Modify only the header fields we own ---
  buf[0] = (buf[0] | PKT_FLAG_FWD) & ~PKT_FLAG_ACK_REQ;  // set FWD, clear ACK_REQ
  buf[2] = get_parent_id();                                 // dst = our parent edge
  buf[3] = NODE_ID;                                         // prev_hop = relay
  buf[4] = buf[4] - 1;                                      // decrement TTL

  // --- Recalculate CRC (big-endian) over modified bytes 0-7 ---
  uint16_t crc = crc16_ccitt(buf, 8);
  buf[8] = (crc >> 8) & 0xFF;
  buf[9] = crc & 0xFF;

  // --- Transmit (header + payload, all bytes, no inspection of payload) ---
  int total_len = MESH_HEADER_SIZE + buf[7];  // header + payload_len bytes
  if (total_len > len) {
    // Sanity check — payload_len field claims more bytes than we received
    Serial.println("FWD  | ERROR: payload_len exceeds received packet length");
    if (pending_forwards > 0) pending_forwards--;
    rxFlag = false;
    radio.startReceive();
    return;
  }

  int state = radio.transmit(buf, total_len);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("FWD  | TX failed, RadioLib code ");
    Serial.println(state);
  }

  if (pending_forwards > 0) pending_forwards--;

  // Return to receive mode
  rxFlag = false;
  radio.startReceive();
}

// =============================================================================
// BROADCAST BEACON
// Sends a beacon so downstream sensor nodes know the relay exists.
//
// MeshHeader:
//   flags      = PKT_TYPE_BEACON
//   src_id     = NODE_ID
//   dst_id     = 0xFF (broadcast)
//   prev_hop   = NODE_ID
//   ttl        = 1 (beacons don't get forwarded)
//   seq_num    = 0 (beacons don't use sequence numbers)
//   rank       = my_rank (set to parent's rank + 1 after parent is found)
//   payload_len= BEACON_PAYLOAD_SIZE (4)
//
// Beacon payload (4 bytes):
//   [0] schema_version = 0x01
//   [1] queue_pct      = real TX queue occupancy (0-100)
//   [2] link_quality   = RSSI-based quality to parent (0-100), 0 if no parent
//   [3] parent_health  = 100 if parent valid, 0 if not
// =============================================================================
void broadcast_beacon() {
  uint8_t beacon[MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE];

  uint8_t qpct = compute_queue_pct();

  // Compute link_quality from parent RSSI (map [-120,-60] → [0,100])
  uint8_t link_quality = 0;
  if (has_valid_parent()) {
    int8_t rssi = candidates[current_parent_idx].rssi;
    int clamped = rssi;
    if (clamped < -120) clamped = -120;
    if (clamped > -60)  clamped = -60;
    link_quality = (uint8_t)((clamped + 120) * 100 / 60);
  }

  // MeshHeader (bytes 0-9)
  beacon[0] = PKT_TYPE_BEACON;
  beacon[1] = NODE_ID;
  beacon[2] = 0xFF;         // broadcast
  beacon[3] = NODE_ID;
  beacon[4] = 1;            // TTL = 1, beacons don't get forwarded
  beacon[5] = 0x00;         // seq_num not used for beacons
  beacon[6] = my_rank;      // dynamic rank (255=orphan, 1=connected to edge)
  beacon[7] = BEACON_PAYLOAD_SIZE;

  // CRC big-endian
  uint16_t crc = crc16_ccitt(beacon, 8);
  beacon[8] = (crc >> 8) & 0xFF;
  beacon[9] = crc & 0xFF;

  // Beacon payload (bytes 10-13)
  beacon[10] = 0x01;          // schema_version
  beacon[11] = qpct;          // queue_pct
  beacon[12] = link_quality;  // link_quality to parent
  beacon[13] = has_valid_parent() ? 100 : 0;  // parent_health

  int state = radio.transmit(beacon, MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.print("BCN  | queue=");
    Serial.print(qpct);
    Serial.print("% | lq=");
    Serial.print(link_quality);
    Serial.print(" | health=");
    Serial.println(beacon[13]);
  } else {
    Serial.print("BCN  | TX failed, code ");
    Serial.println(state);
  }

  rxFlag = false;
  radio.startReceive();
}

// =============================================================================
// PROCESS BEACON (from edge nodes only)
// Updates the candidate table with info from an edge node beacon.
// =============================================================================
void process_beacon(uint8_t *buf, int len, int rssi) {
  uint8_t src_id    = buf[1];
  uint8_t rank      = buf[6];
  uint8_t pay_len   = buf[7];
  uint8_t queue_pct = 0;

  if (len >= MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE && pay_len >= BEACON_PAYLOAD_SIZE) {
    // buf[MESH_HEADER_SIZE + 0] = schema_version (skip)
    queue_pct = buf[MESH_HEADER_SIZE + 1];
  }

  Serial.print("BCN  | src=0x");
  Serial.print(src_id, HEX);
  Serial.print(" | rank=");
  Serial.print(rank);
  Serial.print(" | rssi=");
  Serial.print(rssi);
  Serial.print(" | queue=");
  Serial.print(queue_pct);
  Serial.println("%");

  int slot = -1;
  for (int i = 0; i < MAX_CANDIDATES; i++) {
    if (candidates[i].valid && candidates[i].node_id == src_id) {
      slot = i; break;
    }
  }
  if (slot == -1) {
    for (int i = 0; i < MAX_CANDIDATES; i++) {
      if (!candidates[i].valid) { slot = i; break; }
    }
  }
  if (slot == -1) slot = 0;  // evict first slot if table full

  candidates[slot].node_id      = src_id;
  candidates[slot].rssi         = (int8_t)rssi;
  candidates[slot].rank         = rank;
  candidates[slot].queue_pct    = queue_pct;
  candidates[slot].last_seen_ms = millis();
  candidates[slot].valid        = true;

  // Re-evaluate parent after every beacon update
  update_parent_selection();
}

// =============================================================================
// PARENT SCORING
// Identical formula to sensor_node.ino — must stay in sync.
// score = (60 × rank_score) + (25 × rssi_score) + (15 × (100 − queue_pct))
// =============================================================================
int score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct) {
  int rank_score = (rank == 0) ? 100 : max(0, 100 - (rank * 15));

  int rssi_clamped = rssi;
  if (rssi_clamped < -120) rssi_clamped = -120;
  if (rssi_clamped > -60)  rssi_clamped = -60;
  int rssi_score = (rssi_clamped + 120) * 100 / 60;

  int queue_score = (100 - (int)queue_pct);

  int total = (60 * rank_score / 100)
            + (25 * rssi_score / 100)
            + (15 * queue_score / 100);

  return total;
}

// =============================================================================
// UPDATE PARENT SELECTION
// Scores Edge 1 and Edge 2 candidates and switches if improvement >= hysteresis.
// =============================================================================
void update_parent_selection() {
  int     best_score = -1;
  uint8_t best_idx   = 0xFF;

  for (int i = 0; i < MAX_CANDIDATES; i++) {
    if (!candidates[i].valid) continue;
    int s = score_parent(candidates[i].rank,
                         candidates[i].rssi,
                         candidates[i].queue_pct);
    if (s > best_score) {
      best_score = s;
      best_idx   = i;
    }
  }

  if (best_idx == 0xFF) return;

  if (current_parent_idx == 0xFF) {
    current_parent_idx = best_idx;
    my_rank = candidates[current_parent_idx].rank + 1;
    Serial.print("PARENT | Selected 0x");
    Serial.print(candidates[current_parent_idx].node_id, HEX);
    Serial.print(" (score=");
    Serial.print(best_score);
    Serial.println(")");
  } else {
    int current_score = score_parent(candidates[current_parent_idx].rank,
                                     candidates[current_parent_idx].rssi,
                                     candidates[current_parent_idx].queue_pct);
    if (best_score >= current_score + PARENT_SWITCH_HYSTERESIS
        && best_idx != current_parent_idx) {
      Serial.print("PARENT | Switching from 0x");
      Serial.print(candidates[current_parent_idx].node_id, HEX);
      Serial.print(" (score=");
      Serial.print(current_score);
      Serial.print(") to 0x");
      Serial.print(candidates[best_idx].node_id, HEX);
      Serial.print(" (score=");
      Serial.print(best_score);
      Serial.println(")");
      current_parent_idx = best_idx;
      my_rank = candidates[current_parent_idx].rank + 1;
    }
  }
}

// =============================================================================
// CHECK PARENT TIMEOUT
// Invalidates current parent if no beacon heard within PARENT_TIMEOUT_MS.
// =============================================================================
void check_parent_timeout() {
  if (current_parent_idx == 0xFF) return;
  if (!candidates[current_parent_idx].valid) return;

  uint32_t age = millis() - candidates[current_parent_idx].last_seen_ms;
  if (age > PARENT_TIMEOUT_MS) {
    Serial.print("PARENT | Timeout on 0x");
    Serial.print(candidates[current_parent_idx].node_id, HEX);
    Serial.print(" (");
    Serial.print(age / 1000);
    Serial.println("s since last beacon). Re-evaluating...");

    candidates[current_parent_idx].valid = false;
    current_parent_idx = 0xFF;
    my_rank = 255;

    // Try to fall back to the other edge immediately
    update_parent_selection();
  }
}

// =============================================================================
// HELPERS
// =============================================================================
bool has_valid_parent() {
  return (current_parent_idx != 0xFF && candidates[current_parent_idx].valid);
}

uint8_t get_parent_id() {
  if (!has_valid_parent()) return 0xFF;
  return candidates[current_parent_idx].node_id;
}

uint8_t compute_queue_pct() {
  return (uint8_t)((pending_forwards * 100) / MAX_PENDING_FORWARDS);
}
