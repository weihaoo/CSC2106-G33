// =============================================================================
// CSC2106 Group 33 — LoRa Mesh Sensor Node
// Singapore Institute of Technology
//
// File   : sensor_node.ino
// Role   : Sensor Node (Person 1)
// NodeIDs: 0x03 (Sensor 1) or 0x04 (Sensor 2)
//
// Hardware : LilyGo T-Beam (SX1262, 923 MHz) + DHT22
// Libraries: RadioLib, XPowersLib
//
// NOTE: Change NODE_ID below before flashing to each sensor node.
//       Sensor 1 = 0x03, Sensor 2 = 0x04
// =============================================================================

#include <Wire.h>
#include <XPowersLib.h>
#include <RadioLib.h>
#include "mesh_protocol.h"

// -----------------------------------------------------------------------------
// NODE CONFIGURATION — Change before flashing!
// -----------------------------------------------------------------------------
#define NODE_ID       0x03    // 0x03 = Sensor 1, 0x04 = Sensor 2

// -----------------------------------------------------------------------------
// TIMING PARAMETERS (node-specific; shared ones come from mesh_protocol.h)
// -----------------------------------------------------------------------------
#define TX_INTERVAL_MS        30000UL   // 30s during development (change to 120000UL for final)

// Beacon phase offset — spreads beacons/transmissions to avoid collisions
#define TX_PHASE_OFFSET_MS    ((NODE_ID % 5) * 2000UL)

// -----------------------------------------------------------------------------
// LoRa RADIO PINS (T-Beam SX1262)
// -----------------------------------------------------------------------------
#define RADIO_NSS   18
#define RADIO_DIO1  33
#define RADIO_RST   23
#define RADIO_BUSY  32

// -----------------------------------------------------------------------------
// DUMMY SENSOR CONFIG
// NOTE: Replace this section with real DHT22 code when sensor is available.
//       See commented-out DHT22 section below.
// -----------------------------------------------------------------------------
// #include <DHT.h>
// #define DHT_PIN  13
// #define DHT_TYPE DHT22
// DHT dht(DHT_PIN, DHT_TYPE);

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
// PARENT TRACKING
// -----------------------------------------------------------------------------
struct ParentInfo {
  uint8_t  node_id;       // ID of current parent
  int8_t   rssi;          // last RSSI from this parent's beacon
  uint8_t  rank;          // rank reported in beacon (0=edge, 1=relay)
  uint8_t  queue_pct;     // queue occupancy reported in beacon
  uint32_t last_seen_ms;  // millis() when we last heard a beacon from this parent
  bool     valid;         // have we ever heard from this parent?
};

// Candidate table — we track up to MAX_CANDIDATES potential parents
ParentInfo candidates[MAX_CANDIDATES];
uint8_t current_parent_idx = 0xFF;  // index into candidates[], 0xFF = none yet

// -----------------------------------------------------------------------------
// SEQUENCE NUMBER
// -----------------------------------------------------------------------------
uint8_t seq_num = 0;

// -----------------------------------------------------------------------------
// FORWARD DECLARATIONS
// -----------------------------------------------------------------------------
void     init_pmu();
void     init_radio();
bool     read_sensor(float &temp, float &hum);
void     build_sensor_payload(uint8_t *payload, float temp, float hum, bool sensor_ok);
void     build_mesh_header(uint8_t *packet, uint8_t dst_id, uint8_t payload_len);
bool     send_with_ack(uint8_t *packet, uint8_t total_len);
void     listen_for_beacons(uint32_t duration_ms);
void     process_beacon(uint8_t *buf, int len, int rssi);
int      score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct);
void     update_parent_selection();
bool     has_valid_parent();
uint8_t  get_parent_id();
void     check_parent_timeout();
bool     wait_for_ack(uint32_t timeout_ms);

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== CSC2106 G33 | Sensor Node 0x" + String(NODE_ID, HEX) + " ===");

  // Initialise candidates table
  for (int i = 0; i < MAX_CANDIDATES; i++) {
    candidates[i].valid = false;
  }

  init_pmu();
  init_radio();

  // Uncomment when using real DHT22:
  // dht.begin();
  // delay(2000);  // DHT22 needs 2s after power-on before first read

  Serial.println("BOOT | Waiting for beacon before first TX...");
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
  static uint32_t last_tx_ms = 0;

  // --- Step 1: Listen for beacons ---
  // If we have no parent yet, listen longer (up to TX_INTERVAL_MS).
  // If we have a parent, just do a short listen window between TX cycles.
  if (!has_valid_parent()) {
    // No parent yet — block and listen until we hear one
    Serial.println("LISTEN | No parent yet, scanning for beacons...");
    listen_for_beacons(TX_INTERVAL_MS);

    if (!has_valid_parent()) {
      Serial.println("LISTEN | No beacon heard. Continuing to wait...");
      return;  // loop again, keep listening
    }
  }

  // --- Step 2: Check if parent has timed out ---
  check_parent_timeout();

  if (!has_valid_parent()) {
    Serial.println("PARENT | Parent timed out. Scanning for new parent...");
    listen_for_beacons(TX_INTERVAL_MS);
    return;
  }

  // --- Step 3: TX cycle ---
  uint32_t now = millis();
  if (now - last_tx_ms >= TX_INTERVAL_MS) {
    last_tx_ms = now;

    // --- Read sensor (dummy data for now) ---
    float temp, hum;
    bool sensor_ok = read_sensor(temp, hum);

    if (!sensor_ok) {
      Serial.println("SENSOR | Read failed — skipping this cycle");
      return;
    }

    // --- Build payload ---
    uint8_t payload[SENSOR_PAYLOAD_SIZE];
    build_sensor_payload(payload, temp, hum, sensor_ok);

    // --- Build full packet (10-byte header + 7-byte payload) ---
    uint8_t packet[MESH_HEADER_SIZE + SENSOR_PAYLOAD_SIZE];
    build_mesh_header(packet, get_parent_id(), SENSOR_PAYLOAD_SIZE);
    memcpy(&packet[MESH_HEADER_SIZE], payload, SENSOR_PAYLOAD_SIZE);

    // --- Apply TX jitter (0–2000ms) ---
    uint32_t jitter = random(0, 2000);
    Serial.print("TX | Jitter delay: ");
    Serial.print(jitter);
    Serial.println(" ms");
    delay(jitter);

    // --- Send with ACK + retry ---
    bool success = send_with_ack(packet, MESH_HEADER_SIZE + SENSOR_PAYLOAD_SIZE);

    if (success) {
      Serial.print("TX | node=");
      Serial.print(NODE_ID, HEX);
      Serial.print(" | seq=");
      Serial.print(seq_num - 1);  // seq_num already incremented
      Serial.print(" | temp=");
      Serial.print(temp, 1);
      Serial.print("C | hum=");
      Serial.print(hum, 1);
      Serial.print("% | parent=");
      Serial.print(get_parent_id(), HEX);
      if (current_parent_idx < MAX_CANDIDATES) {
        Serial.print(" | rssi=");
        Serial.print(candidates[current_parent_idx].rssi);
      }
      Serial.println();
    } else {
      Serial.println("TX | All retries failed. Triggering parent re-evaluation.");
      // Trigger parent re-evaluation on next loop
      current_parent_idx = 0xFF;
    }

    // --- Short listen window after TX for any incoming beacons ---
    listen_for_beacons(2000);
  } else {
    // --- Between TX cycles: listen for beacons ---
    listen_for_beacons(500);
  }
}

// =============================================================================
// PMU INITIALISATION
// Must be called before radio init — SX1262 is powered via ALDO2 rail.
// =============================================================================
void init_pmu() {
  Wire.begin(21, 22);
  if (!PMU.begin(Wire, 0x34, 21, 22)) {  // 0x34 = AXP2101 I2C address
    Serial.println("ERROR: PMU init failed! Check board wiring.");
    while (true) delay(1000);
  }
  PMU.setALDO2Voltage(3300);  // 3.3V to SX1262
  PMU.enableALDO2();
  Serial.println("PMU OK — LoRa radio powered on via ALDO2");
  delay(10);  // let radio power stabilise
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
  Serial.print("RadioLib OK — SX1262 @ ");
  Serial.print(LORA_FREQUENCY, 0);
  Serial.println(" MHz, SF7");
}

// =============================================================================
// SENSOR READ
// Currently returns dummy data. Replace with real DHT22 read when ready.
// =============================================================================
bool read_sensor(float &temp, float &hum) {
  // --- DUMMY DATA (remove when DHT22 is wired up) ---
  temp = 24.0 + (float)(random(-20, 60)) / 10.0;  // 22.0–30.0°C range
  hum  = 60.0 + (float)(random(-100, 200)) / 10.0; // 50.0–80.0% range
  return true;

  // --- REAL DHT22 (uncomment when sensor is ready) ---
  // float t = dht.readTemperature();
  // float h = dht.readHumidity();
  // if (isnan(t) || isnan(h)) {
  //   return false;
  // }
  // temp = t;
  // hum  = h;
  // return true;
}

// =============================================================================
// BUILD SENSOR PAYLOAD (SENSOR_PAYLOAD_SIZE = 7 bytes)
//
// Byte layout (big-endian integers — matches SensorPayload struct definition):
//   [0] schema_version = 0x01
//   [1] sensor_type    = 0x03 (DHT22)
//   [2-3] temp_c_x10   (signed int16, big-endian)
//   [4-5] humidity_x10 (unsigned int16, big-endian)
//   [6] status         (0x00 = OK, 0x01 = read error)
// =============================================================================
void build_sensor_payload(uint8_t *payload, float temp, float hum, bool sensor_ok) {
  int16_t  temp_x10 = (int16_t)(temp * 10.0);
  uint16_t hum_x10  = (uint16_t)(hum * 10.0);

  payload[0] = 0x01;                        // schema_version
  payload[1] = SENSOR_TYPE_DHT22;           // sensor_type = DHT22
  payload[2] = (temp_x10 >> 8) & 0xFF;     // temp high byte
  payload[3] = temp_x10 & 0xFF;             // temp low byte
  payload[4] = (hum_x10 >> 8) & 0xFF;      // humidity high byte
  payload[5] = hum_x10 & 0xFF;             // humidity low byte
  payload[6] = sensor_ok ? 0x00 : 0x01;    // status
}

// =============================================================================
// BUILD MESH HEADER (MESH_HEADER_SIZE = 10 bytes) into packet[0..9]
//
// Uses crc16_ccitt() and set_mesh_crc() from mesh_protocol.h.
//
// Byte layout:
//   [0] flags        PKT_TYPE_DATA | PKT_FLAG_ACK_REQ
//   [1] src_id       NODE_ID
//   [2] dst_id       parent node ID
//   [3] prev_hop     NODE_ID (sensor is always the first hop)
//   [4] ttl          DEFAULT_TTL
//   [5] seq_num      incremented each call
//   [6] rank         RANK_SENSOR (2)
//   [7] payload_len
//   [8-9] crc16      big-endian, over bytes 0–7
// =============================================================================
void build_mesh_header(uint8_t *packet, uint8_t dst_id, uint8_t payload_len) {
  packet[0] = PKT_TYPE_DATA | PKT_FLAG_ACK_REQ;
  packet[1] = NODE_ID;
  packet[2] = dst_id;
  packet[3] = NODE_ID;         // prev_hop = self (first hop)
  packet[4] = DEFAULT_TTL;     // ttl
  packet[5] = seq_num++;       // increment sequence number
  packet[6] = RANK_SENSOR;     // rank = 2
  packet[7] = payload_len;

  // Write CRC big-endian at bytes 8-9
  uint16_t crc = crc16_ccitt(packet, 8);
  packet[8] = (crc >> 8) & 0xFF;
  packet[9] = crc & 0xFF;
}

// =============================================================================
// SEND WITH ACK + RETRY
// Returns true if ACK received within ACK_TIMEOUT_MS on any attempt.
// Returns false if all MAX_RETRIES attempts fail.
// =============================================================================
bool send_with_ack(uint8_t *packet, uint8_t total_len) {
  for (uint8_t attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.print("TX | Attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(MAX_RETRIES);
    Serial.print(" → parent=0x");
    Serial.println(get_parent_id(), HEX);

    // Transmit
    int state = radio.transmit(packet, total_len);
    if (state != RADIOLIB_ERR_NONE) {
      Serial.print("TX | TX failed, RadioLib code ");
      Serial.println(state);
      delay(200);
      continue;
    }

    // Switch to RX and wait for ACK
    rxFlag = false;
    radio.startReceive();
    if (wait_for_ack(ACK_TIMEOUT_MS)) {
      return true;  // ACK received
    }

    Serial.println("TX | No ACK received, retrying...");
    delay(100 * attempt);  // small back-off between retries
  }
  return false;  // all retries exhausted
}

// =============================================================================
// WAIT FOR ACK
// Listens on the radio for up to `timeout_ms` milliseconds.
// Returns true if a valid ACK (PKT_TYPE_ACK) addressed to this node is received.
//
// ACK packet format (10 bytes):
//   [0] flags   — must be PKT_TYPE_ACK (0x40)
//   [1] src_id  — the node sending the ACK (our parent)
//   [2] dst_id  — must match NODE_ID
//   [3] prev_hop
//   [4] ttl
//   [5] seq_num — echoes back the seq we just sent
//   [6] rank
//   [7] payload_len — 0 for a pure ACK
//   [8-9] crc16
// =============================================================================
bool wait_for_ack(uint32_t timeout_ms) {
  uint32_t start = millis();
  uint8_t  ack_buf[MESH_HEADER_SIZE];

  while (millis() - start < timeout_ms) {
    if (rxFlag) {
      rxFlag = false;
      int len = radio.getPacketLength();
      int state = radio.readData(ack_buf, sizeof(ack_buf));

      // Restart RX immediately so the radio doesn't go deaf if this
      // packet fails any check below (CRC, type, dst, src).
      rxFlag = false;
      radio.startReceive();

      if (state != RADIOLIB_ERR_NONE || len < MESH_HEADER_SIZE) {
        delay(1);
        continue;
      }

      // Validate CRC (big-endian at bytes 8-9)
      uint16_t received_crc = ((uint16_t)ack_buf[8] << 8) | ack_buf[9];
      uint16_t computed_crc = crc16_ccitt(ack_buf, 8);
      if (received_crc != computed_crc) {
        Serial.println("ACK | CRC mismatch on received packet — ignoring");
        delay(1);
        continue;
      }

      uint8_t flags   = ack_buf[0];
      uint8_t src_id  = ack_buf[1];
      uint8_t dst_id  = ack_buf[2];

      // Accept PKT_TYPE_ACK (0x40) from our parent addressed to us
      bool is_ack_pkt      = (GET_PKT_TYPE(flags) == PKT_TYPE_ACK);
      bool addressed_to_me = (dst_id == NODE_ID);
      bool from_parent     = (src_id == get_parent_id());

      if (is_ack_pkt && addressed_to_me && from_parent) {
        Serial.print("ACK | Received from 0x");
        Serial.println(src_id, HEX);
        return true;
      }
    }
    delay(1);
  }
  return false;  // timeout
}

// =============================================================================
// LISTEN FOR BEACONS
// Listens on the radio for up to `duration_ms` milliseconds.
// Any valid beacon packet received is processed and updates the candidate table.
// =============================================================================
void listen_for_beacons(uint32_t duration_ms) {
  rxFlag = false;
  radio.startReceive();
  uint32_t start = millis();
  uint8_t  buf[64];

  while (millis() - start < duration_ms) {
    if (rxFlag) {
      rxFlag = false;
      int len = radio.getPacketLength();
      int state = radio.readData(buf, sizeof(buf));
      int rssi = (int)radio.getRSSI();

      // Restart receive after reading
      rxFlag = false;
      radio.startReceive();

      if (state != RADIOLIB_ERR_NONE || len < MESH_HEADER_SIZE) {
        continue;
      }

      // Validate CRC (big-endian at bytes 8-9)
      uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
      uint16_t computed_crc = crc16_ccitt(buf, 8);

      if (received_crc != computed_crc) {
        // Silently discard corrupted packets
        continue;
      }

      uint8_t flags = buf[0];

      // Is this a BEACON packet?
      if (GET_PKT_TYPE(flags) == PKT_TYPE_BEACON) {
        process_beacon(buf, len, rssi);
      }
    }
    delay(1);
  }

  // After listening, re-evaluate which parent to use
  update_parent_selection();
}

// =============================================================================
// PROCESS BEACON
// Parses a beacon packet and updates the candidate table.
//
// MeshHeader fields used:
//   [1] src_id   — which node sent the beacon
//   [6] rank     — that node's rank (0=edge, 1=relay)
//
// Beacon payload (4 bytes at buf[MESH_HEADER_SIZE]):
//   [0] schema_version
//   [1] queue_pct      (0–100)
//   [2] link_quality   (informational)
//   [3] parent_health  (informational)
// =============================================================================
void process_beacon(uint8_t *buf, int len, int rssi) {
  uint8_t src_id      = buf[1];
  uint8_t rank        = buf[6];
  uint8_t payload_len = buf[7];

  // We don't want beacons from ourselves
  if (src_id == NODE_ID) return;

  uint8_t queue_pct = 0;
  if (len >= MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE && payload_len >= BEACON_PAYLOAD_SIZE) {
    // buf[MESH_HEADER_SIZE + 0] = schema_version (skip)
    queue_pct = buf[MESH_HEADER_SIZE + 1];  // queue_pct
  }

  Serial.print("BCN | src=0x");
  Serial.print(src_id, HEX);
  Serial.print(" | rank=");
  Serial.print(rank);
  Serial.print(" | rssi=");
  Serial.print(rssi);
  Serial.print(" | queue=");
  Serial.print(queue_pct);
  Serial.println("%");

  // Find existing entry or empty slot
  int slot = -1;
  for (int i = 0; i < MAX_CANDIDATES; i++) {
    if (candidates[i].valid && candidates[i].node_id == src_id) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    // Find empty slot
    for (int i = 0; i < MAX_CANDIDATES; i++) {
      if (!candidates[i].valid) {
        slot = i;
        break;
      }
    }
  }
  if (slot == -1) {
    // Table full — replace last slot (simple eviction)
    slot = MAX_CANDIDATES - 1;
  }

  candidates[slot].node_id      = src_id;
  candidates[slot].rssi         = (int8_t)rssi;
  candidates[slot].rank         = rank;
  candidates[slot].queue_pct    = queue_pct;
  candidates[slot].last_seen_ms = millis();
  candidates[slot].valid        = true;
}

// =============================================================================
// PARENT SCORING
// score = (60 × rank_score) + (25 × rssi_score) + (15 × (100 − queue_pct))
//
// rank_score  : rank=0 (edge) → 100, rank=1 (relay) → 85, rank=2 → 70, ...
// rssi_score  : maps RSSI [-120, -60] → [0, 100]
// queue_pct   : lower is better; contribution = 15 × (100 - queue_pct) / 100
// =============================================================================
int score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct) {
  // Rank score
  int rank_score = (rank == 0) ? 100 : max(0, 100 - (rank * 15));

  // RSSI score: clamp to [-120, -60], map to [0, 100]
  int rssi_clamped = rssi;
  if (rssi_clamped < -120) rssi_clamped = -120;
  if (rssi_clamped > -60)  rssi_clamped = -60;
  int rssi_score = (rssi_clamped + 120) * 100 / 60;  // 0–100

  // Queue score
  int queue_score = (100 - (int)queue_pct);  // 0–100, higher is better

  int total = (60 * rank_score / 100)
            + (25 * rssi_score / 100)
            + (15 * queue_score / 100);

  return total;
}

// =============================================================================
// UPDATE PARENT SELECTION
// Scores all valid candidates and switches parent if a better one is found.
// Hysteresis: only switch if new score is ≥ PARENT_SWITCH_HYSTERESIS better.
// =============================================================================
void update_parent_selection() {
  int   best_score = -1;
  uint8_t best_idx = 0xFF;

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

  if (best_idx == 0xFF) return;  // no valid candidates

  if (current_parent_idx == 0xFF) {
    // No current parent — take the best one immediately
    current_parent_idx = best_idx;
    Serial.print("PARENT | Selected 0x");
    Serial.print(candidates[current_parent_idx].node_id, HEX);
    Serial.print(" (score=");
    Serial.print(best_score);
    Serial.println(")");
  } else {
    // We have a current parent — only switch if score improvement >= hysteresis
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
    }
  }
}

// =============================================================================
// CHECK PARENT TIMEOUT
// If we haven't heard from the current parent in PARENT_TIMEOUT_MS, drop it.
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

    // Invalidate the timed-out parent
    candidates[current_parent_idx].valid = false;
    current_parent_idx = 0xFF;

    // Try to select another from remaining valid candidates
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
