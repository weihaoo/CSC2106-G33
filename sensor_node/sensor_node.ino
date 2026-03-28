// =============================================================================
// CSC2106 Group 33 — LoRa Mesh Sensor Node (with Relay Capability)
// Singapore Institute of Technology
//
// File   : sensor_node.ino
// Role   : Sensor Node + Relay (Person 1)
// NodeIDs: 0x03 (Sensor 1) or 0x04 (Sensor 2)
//
// Hardware : LilyGo T-Beam (SX1262, 923 MHz) + DHT22
// Libraries: RadioLib, XPowersLib
//
// This node:
//   - Reads sensor data (DHT22) and sends it toward the edge every TX_INTERVAL_MS
//   - Broadcasts beacons so other sensors can discover it as a parent
//   - Forwards DATA packets from downstream nodes (payload-agnostic)
//   - ACKs downstream senders on receipt of valid DATA
//   - Selects best parent (edge, relay, or another sensor) via beacon scoring
//
// NOTE: Change NODE_ID below before flashing to each sensor node.
//       Sensor 1 = 0x03, Sensor 2 = 0x04
// =============================================================================

// CHANGE THIS BEFORE FLASHING: Sensor 1 = SENSOR-03, Sensor 2 = SENSOR-04
#define NODE_NAME "SENSOR-03"
#include "logging.h"

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
#define TX_INTERVAL_MS        5000UL   // 30s during development (change to 120000UL for final)

// Beacon phase offset — spreads beacons/transmissions to avoid collisions
#define BEACON_PHASE_OFFSET_MS  ((NODE_ID % 5) * 2000UL)
// -----------------------------------------------------------------------------
// LoRa RADIO PINS (T-Beam SX1262)
// -----------------------------------------------------------------------------
#define RADIO_NSS   18
#define RADIO_DIO1  33
#define RADIO_RST   23
#define RADIO_BUSY  32

// -----------------------------------------------------------------------------
// DHT22 SENSOR CONFIG
// Set SIMULATION_MODE to false to use real DHT22 sensor data.
// Set SIMULATION_MODE to true  to use random simulated data.
// -----------------------------------------------------------------------------
#define SIMULATION_MODE false

#include <DHT.h>
#define DHT_PIN  4
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

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
  uint8_t  parent_health; // parent_health from beacon (0=no route, 100=healthy)
  uint32_t last_seen_ms;  // millis() when we last heard a beacon from this parent
  bool     valid;         // have we ever heard from this parent?
};

// Candidate table — we track up to MAX_CANDIDATES potential parents
ParentInfo candidates[MAX_CANDIDATES];
uint8_t current_parent_idx = 0xFF;  // index into candidates[], 0xFF = none yet
uint8_t my_rank = RANK_SENSOR;      // Dynamic rank: starts at 2, updated to parent.rank+1

// -----------------------------------------------------------------------------
// DEDUP TABLE (circular buffer — prevents forwarding the same packet twice)
// Uses DedupEntry from mesh_protocol.h.
// -----------------------------------------------------------------------------
DedupEntry dedup_table[DEDUP_TABLE_SIZE];
uint8_t    dedup_head = 0;  // next slot to overwrite (circular)

// -----------------------------------------------------------------------------
// FORWARDING QUEUE COUNTER
// Simple counter tracking how many packets are pending forward.
// Used to compute queue_pct for beacons.
// -----------------------------------------------------------------------------
volatile uint8_t pending_forwards = 0;
#define MAX_PENDING_FORWARDS  10  // denominator for queue_pct calculation

// -----------------------------------------------------------------------------
// SEQUENCE NUMBER
// -----------------------------------------------------------------------------
uint8_t seq_num = 0;

// -----------------------------------------------------------------------------
// ACK TRACKING — for send_with_ack() to detect ACKs during receive_and_process()
// -----------------------------------------------------------------------------
volatile bool ack_received = false;
uint8_t       ack_expected_seq = 0;    // seq_num we're waiting for ACK on
bool          waiting_for_ack = false;  // true while send_with_ack() is active

// -----------------------------------------------------------------------------
// FORWARD DECLARATIONS
// -----------------------------------------------------------------------------
void     init_pmu();
void     init_radio();
bool     read_sensor(float &temp, float &hum);
void     build_sensor_payload(uint8_t *payload, float temp, float hum, bool sensor_ok);
void     build_mesh_header(uint8_t *packet, uint8_t dst_id, uint8_t payload_len);
bool     send_with_ack(uint8_t *packet, uint8_t total_len);
bool     wait_for_ack(uint32_t timeout_ms);

// Relay capability functions (mirrored from relay_node.ino)
void     receive_and_process();
bool     is_duplicate(uint8_t src_id, uint8_t seq_num);
void     record_dedup(uint8_t src_id, uint8_t seq_num);
void     send_ack(uint8_t dst_id, uint8_t seq_num);
void     forward_packet(uint8_t *buf, int len);
void     broadcast_beacon();
void     process_beacon(uint8_t *buf, int len, int rssi);
int      score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct, uint8_t parent_health = 100, bool debug = false, uint8_t node_id = 0);
void     update_parent_selection();
bool     has_valid_parent();
uint8_t  get_parent_id();
void     check_parent_timeout();
uint8_t  compute_queue_pct();

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  log_boot_banner("Sensor Node");

  // Initialise candidates table
  for (int i = 0; i < MAX_CANDIDATES; i++) {
    candidates[i].valid = false;
  }

  // Initialise dedup table
  for (int i = 0; i < DEDUP_TABLE_SIZE; i++) {
    dedup_table[i].valid = false;
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

#if SIMULATION_MODE
  randomSeed(analogRead(0));
  LOG_INFO("WARNING: Running in SIMULATION MODE — using random temp/humidity data");
  LOG_INFO("         Fix DHT22 wiring and set SIMULATION_MODE to false for real data");
#else
  dht.begin();
  delay(2000);  // DHT22 needs 2s after power-on before first read
  LOG_OK("DHT22 initialized on GPIO 4");
#endif

  // Apply beacon phase offset
  sprintf(buf, "Beacon phase offset: %lu ms", BEACON_PHASE_OFFSET_MS);
  LOG_INFO(buf);
  delay(BEACON_PHASE_OFFSET_MS);

  // Beacon immediately at boot so other nodes can discover us
  broadcast_beacon();

  // Start in RX mode — event-driven loop will handle everything
  rxFlag = false;
  radio.startReceive();

  LOG_INFO("Sensor node ready. Listening for parent beacons...");
}

// =============================================================================
// MAIN LOOP — EVENT-DRIVEN (non-blocking)
//
// Unlike the original blocking listen-then-send pattern, this loop:
//   1. Beacons periodically (so downstream nodes can discover us)
//   2. Checks parent timeout
//   3. Processes any incoming packet (beacon, data, ack) — non-blocking
//   4. Sends sensor data on a timer (with ACK + retry)
//
// This allows the sensor to relay packets from other nodes at any time,
// not just during a dedicated listen window.
// =============================================================================
void loop() {
  static uint32_t last_tx_ms = 0;
  static uint32_t last_beacon_ms = millis();

  // --- Step 1: Broadcast beacon if due ---
  uint32_t now = millis();
  if (now - last_beacon_ms >= BEACON_INTERVAL_MS) {
    last_beacon_ms = now;
    broadcast_beacon();
  }

  // --- Step 2: Check if parent has timed out ---
  check_parent_timeout();

  // --- Step 3: Process incoming packets (non-blocking) ---
  receive_and_process();

  // --- Step 4: Send sensor data if TX interval has elapsed ---
  now = millis();
  if (now - last_tx_ms >= TX_INTERVAL_MS && has_valid_parent()) {
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
      Serial.println("TX | All retries failed. Invalidating parent and re-evaluating.");
      // Invalidate the failed parent so it isn't immediately reselected
      if (current_parent_idx < MAX_CANDIDATES) {
        candidates[current_parent_idx].valid = false;
      }
      current_parent_idx = 0xFF;
      my_rank = RANK_SENSOR;  // Reset rank
      update_parent_selection();
      // Rapid beacon so downstream nodes know our topology changed
      broadcast_beacon();
    }
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
// Reads temperature and humidity. Uses random data in SIMULATION_MODE,
// real DHT22 data otherwise.
// =============================================================================
bool read_sensor(float &temp, float &hum) {
#if SIMULATION_MODE
  // Generate random but realistic values
  temp = 20.0 + (random(0, 151) / 10.0);   // 20.0–35.0 °C
  hum  = 50.0 + (random(0, 401) / 10.0);   // 50.0–90.0 %
  return true;
#else
  // REAL DHT22 SENSOR
  float t_sensor = dht.readTemperature();
  float h_sensor = dht.readHumidity();
  if (isnan(t_sensor) || isnan(h_sensor)) {
    return false;
  }
  temp = t_sensor;
  hum  = h_sensor;
  return true;
#endif
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
//   [6] rank         my_rank (dynamic)
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
  packet[6] = my_rank;         // dynamic rank
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
//
// During the ACK wait, we use receive_and_process() to handle incoming packets.
// This lets us forward other nodes' data even while waiting for our own ACK.
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
    int state = lbt_transmit(radio, packet, total_len);
    if (state != RADIOLIB_ERR_NONE) {
      Serial.print("TX | TX failed, RadioLib code ");
      Serial.println(state);
      delay(200);
      continue;
    }

    // Switch to RX and wait for ACK
    ack_received = false;
    ack_expected_seq = packet[5];  // the seq_num we sent
    waiting_for_ack = true;

    radio.startReceive();
    // NOTE: Do NOT clear rxFlag here — if an ACK arrived during TX,
    // the DIO1 interrupt already set it and we need to process it.

    if (wait_for_ack(ACK_TIMEOUT_MS)) {
      waiting_for_ack = false;
      return true;  // ACK received
    }

    waiting_for_ack = false;
    Serial.println("TX | No ACK received, retrying...");
    delay(100 * attempt);  // small back-off between retries
  }
  return false;  // all retries exhausted
}

// =============================================================================
// WAIT FOR ACK
// Listens on the radio for up to `timeout_ms` milliseconds.
// Uses receive_and_process() so we can still forward other nodes' packets
// while waiting for our own ACK. ACKs addressed to us are detected via
// the ack_received flag set inside receive_and_process().
// =============================================================================
bool wait_for_ack(uint32_t timeout_ms) {
  uint32_t start = millis();

  while (millis() - start < timeout_ms) {
    receive_and_process();

    if (ack_received) {
      return true;
    }
    delay(1);
  }
  return false;  // timeout
}

// =============================================================================
// RECEIVE AND PROCESS — NON-BLOCKING PACKET HANDLER
//
// Called every loop() iteration. Reads one packet if available and handles:
//   - BEACON: from lower-rank node → update candidate table
//   - DATA:   from another node → validate, ACK, forward to parent
//   - ACK:    addressed to us → signal send_with_ack() if waiting
//
// This is the core function that enables mesh relay capability.
// Mirrors the relay_node's receive_and_process() logic.
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

  // --- Parse header fields ---
  uint8_t flags   = buf[0];
  uint8_t src_id  = buf[1];
  uint8_t dst_id  = buf[2];
  uint8_t prev_hop = buf[3];
  uint8_t ttl     = buf[4];
  uint8_t seq     = buf[5];
  uint8_t rank    = buf[6];
  uint8_t pay_len = buf[7];

  // --- Validation 2: Ignore our own packets ---
  if (src_id == NODE_ID) {
    return;  // our own transmission echoing back
  }

  // --- Handle by packet type ---
  uint8_t pkt_type = GET_PKT_TYPE(flags);

  // ---- BEACON ----
  if (pkt_type == PKT_TYPE_BEACON) {
    // Extract parent_health from beacon payload to distinguish
    // "sensor with a route" (health>0) from "orphaned sensor" (health=0)
    uint8_t beacon_health = 0;
    if (len >= MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE && pay_len >= BEACON_PAYLOAD_SIZE) {
      beacon_health = buf[MESH_HEADER_SIZE + 3];  // parent_health byte
    }

    // Accept beacons from:
    //   1. Lower rank (always — closer to edge), OR
    //   2. Same rank with a valid parent, when we have no parent
    //      (allows sensor-to-sensor relay without creating loops)
    bool accept = (rank < my_rank) ||
                  (rank == my_rank && beacon_health > 0 && !has_valid_parent());

    if (accept) {
      // Validate CRC before processing
      uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
      uint16_t computed_crc = crc16_ccitt(buf, 8);
      if (received_crc == computed_crc) {
        process_beacon(buf, len, rssi);
      }
    } else {
      // Log that we heard the beacon but filtered it — helps debug connectivity
      Serial.print("BCN | FILTERED src=0x");
      Serial.print(src_id, HEX);
      Serial.print(" | rank=");
      Serial.print(rank);
      Serial.print(" (my_rank=");
      Serial.print(my_rank);
      Serial.print(") | health=");
      Serial.print(beacon_health);
      Serial.print(" | rssi=");
      Serial.println(rssi);
    }
    return;  // beacons are never forwarded
  }

  // ---- ACK ----
  if (pkt_type == PKT_TYPE_ACK) {
    // Validate CRC
    uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
    uint16_t computed_crc = crc16_ccitt(buf, 8);
    if (received_crc != computed_crc) {
      Serial.println("ACK | CRC mismatch — ignoring");
      return;
    }

    // Check if this is an ACK for our own transmission
    if (dst_id == NODE_ID && waiting_for_ack && seq == ack_expected_seq) {
      Serial.print("ACK | Received from 0x");
      Serial.println(src_id, HEX);
      ack_received = true;

      // Refresh parent liveness: an ACK from our parent proves it's alive,
      // even if we missed its beacons due to channel congestion
      for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (candidates[i].valid && candidates[i].node_id == src_id) {
          candidates[i].last_seen_ms = millis();
          break;
        }
      }
    }
    return;  // ACKs are never forwarded
  }

  // ---- DATA — only process DATA packets beyond this point ----

  // --- Validation 3: Destination check (prevents cross-path forwarding) ---
  if (dst_id != NODE_ID) {
    return;  // Not addressed to us — ignore
  }

  // --- Validation 4: Dedup check ---
  if (is_duplicate(src_id, seq)) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=duplicate");

    // Re-ACK: the sender retried because our earlier ACK was lost
    if (flags & PKT_FLAG_ACK_REQ) {
      send_ack(prev_hop, seq);
    }
    return;
  }

  // --- Validation 5: CRC check (big-endian at bytes 8-9) ---
  uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
  uint16_t computed_crc = crc16_ccitt(buf, 8);
  if (received_crc != computed_crc) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=crc_fail");
    return;
  }

  // --- Validation 6: TTL check ---
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
  if (flags & PKT_FLAG_ACK_REQ) {
    send_ack(prev_hop, seq);  // ACK goes to prev_hop
  }

  // --- Forward the packet (payload-agnostic) ---
  if (!has_valid_parent()) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=no_parent");
    return;
  }

  // --- Loop prevention: don't forward back to the node that sent it to us ---
  if (get_parent_id() == prev_hop) {
    Serial.print("DROP | uid=");
    Serial.print(src_id, HEX);
    Serial.print(seq, HEX);
    Serial.println(" | reason=loop_detected");
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
  dedup_head = (dedup_head + 1) % DEDUP_TABLE_SIZE;
}

// =============================================================================
// SEND ACK
// Sends a PKT_TYPE_ACK (10-byte MeshHeader, payload_len=0) to dst_id.
// =============================================================================
void send_ack(uint8_t dst_id, uint8_t seq_num) {
  uint8_t ack[MESH_HEADER_SIZE];

  ack[0] = PKT_TYPE_ACK;   // 0x40 — explicit ACK type
  ack[1] = NODE_ID;         // src = this sensor
  ack[2] = dst_id;          // dst = the node that sent us the DATA
  ack[3] = NODE_ID;         // prev_hop = this sensor
  ack[4] = 1;               // ttl = 1 (ACK only needs to reach next hop)
  ack[5] = seq_num;         // echo back the same seq_num
  ack[6] = my_rank;         // our dynamic rank
  ack[7] = 0;               // payload_len = 0 (pure ACK)

  // Write CRC big-endian at bytes 8-9
  uint16_t crc = crc16_ccitt(ack, 8);
  ack[8] = (crc >> 8) & 0xFF;
  ack[9] = crc & 0xFF;

  int state = lbt_transmit(radio, ack, MESH_HEADER_SIZE);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.print("ACK  | to=");
    Serial.print(dst_id, HEX);
    Serial.print(" | for_seq=");
    Serial.println(seq_num);
  } else {
    Serial.print("ACK  | FAILED, code ");
    Serial.println(state);
  }

  // Return to receive mode after ACK — don't clear rxFlag
  radio.startReceive();
}

// =============================================================================
// FORWARD PACKET (Payload-Agnostic, with ACK + Retry)
//
// The sensor-as-relay:
//   1. Sets prev_hop = NODE_ID
//   2. Sets PKT_FLAG_FWD in flags byte
//   3. Keeps PKT_FLAG_ACK_REQ so parent acknowledges receipt
//   4. Decrements TTL by 1
//   5. Recalculates CRC (big-endian) over modified bytes 0-7
//   6. Sends with ACK + retry for reliable forwarding
//
// RULE: bytes MESH_HEADER_SIZE onward are NEVER inspected. They are copied
//       as a block. The relay does not know or care what sensor type or
//       payload format is inside. This is the payload-agnostic guarantee.
// =============================================================================
void forward_packet(uint8_t *buf, int len) {
  // Track pending forwards for beacon queue_pct
  if (pending_forwards < MAX_PENDING_FORWARDS) pending_forwards++;

  // --- Modify only the header fields we own ---
  buf[0] = buf[0] | PKT_FLAG_FWD;       // set FWD, keep ACK_REQ for reliable forwarding
  buf[2] = get_parent_id();              // dst = our parent
  buf[3] = NODE_ID;                      // prev_hop = this sensor
  buf[4] = buf[4] - 1;                   // decrement TTL

  // --- Recalculate CRC (big-endian) over modified bytes 0-7 ---
  uint16_t crc = crc16_ccitt(buf, 8);
  buf[8] = (crc >> 8) & 0xFF;
  buf[9] = crc & 0xFF;

  // --- Validate total length ---
  int total_len = MESH_HEADER_SIZE + buf[7];  // header + payload_len bytes
  if (total_len > len) {
    Serial.println("FWD  | ERROR: payload_len exceeds received packet length");
    if (pending_forwards > 0) pending_forwards--;
    radio.startReceive();
    return;
  }

  // --- Send with ACK + retry for reliable forwarding ---
  uint8_t fwd_seq = buf[5];
  bool success = false;

  for (uint8_t attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.print("FWD  | Attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(MAX_RETRIES);
    Serial.print(" → parent=0x");
    Serial.println(get_parent_id(), HEX);

    int state = lbt_transmit(radio, buf, total_len);
    if (state != RADIOLIB_ERR_NONE) {
      Serial.print("FWD  | TX failed, RadioLib code ");
      Serial.println(state);
      delay(200);
      continue;
    }

    // Wait for ACK from parent
    ack_received = false;
    ack_expected_seq = fwd_seq;
    waiting_for_ack = true;

    radio.startReceive();

    if (wait_for_ack(ACK_TIMEOUT_MS)) {
      waiting_for_ack = false;
      success = true;
      Serial.println("FWD  | ACK received from parent");
      break;
    }

    waiting_for_ack = false;
    Serial.println("FWD  | No ACK, retrying...");
    delay(100 * attempt);  // back-off
  }

  if (!success) {
    Serial.println("FWD  | All forward retries failed — packet may be lost");
  }

  if (pending_forwards > 0) pending_forwards--;

  // Return to receive mode
  radio.startReceive();
}

// =============================================================================
// BROADCAST BEACON
// Sends a beacon so downstream nodes know this sensor exists as a relay option.
//
// MeshHeader:
//   flags      = PKT_TYPE_BEACON
//   src_id     = NODE_ID
//   dst_id     = 0xFF (broadcast)
//   prev_hop   = NODE_ID
//   ttl        = 1 (beacons don't get forwarded)
//   seq_num    = 0 (beacons don't use sequence numbers)
//   rank       = my_rank (dynamic: parent.rank + 1)
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

  // Propagate downstream congestion: report max of own queue and parent's
  // reported queue_pct. Without this, relay/sensor always shows queue=0%
  // even when the edge's aggregation buffer is full.
  if (has_valid_parent()) {
    uint8_t parent_qpct = candidates[current_parent_idx].queue_pct;
    if (parent_qpct > qpct) qpct = parent_qpct;
  }

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
  beacon[6] = my_rank;      // dynamic rank
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

  int state = lbt_transmit(radio, beacon, MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.print("BCN  | rank=");
    Serial.print(my_rank);
    Serial.print(" | queue=");
    Serial.print(qpct);
    Serial.print("% | lq=");
    Serial.print(link_quality);
    Serial.print(" | health=");
    Serial.println(beacon[13]);
  } else {
    Serial.print("BCN  | TX failed, code ");
    Serial.println(state);
  }

  radio.startReceive();
}

// =============================================================================
// PROCESS BEACON
// Parses a beacon packet and updates the candidate table.
//
// MeshHeader fields used:
//   [1] src_id   — which node sent the beacon
//   [6] rank     — that node's rank (0=edge, 1=relay, etc.)
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
  uint8_t health = 0;
  if (len >= MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE && payload_len >= BEACON_PAYLOAD_SIZE) {
    // buf[MESH_HEADER_SIZE + 0] = schema_version (skip)
    queue_pct = buf[MESH_HEADER_SIZE + 1];  // queue_pct
    health    = buf[MESH_HEADER_SIZE + 3];  // parent_health
  }

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
  candidates[slot].parent_health = health;
  candidates[slot].last_seen_ms = millis();
  candidates[slot].valid        = true;

  // Re-evaluate parent after every beacon update
  update_parent_selection();
}

// =============================================================================
// PARENT SCORING
// score = (60 × rank_score) + (25 × rssi_score) + (15 × (100 − queue_pct))
//
// rank_score  : rank=0 (edge) → 100, rank=1 (relay) → 85, rank=2 → 70, ...
// rssi_score  : maps RSSI [-120, -60] → [0, 100]
// queue_pct   : lower is better; contribution = 15 × (100 - queue_pct) / 100
// =============================================================================
int score_parent(uint8_t rank, int8_t rssi, uint8_t queue_pct, uint8_t parent_health, bool debug, uint8_t node_id) {
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

  // Hard penalty: if parent queue is near-full, halve the score.
  bool penalized = false;
  if (queue_pct >= 80) {
    total = total / 2;
    penalized = true;
  }

  // Hard penalty: if candidate has no route to edge (parent_health=0),
  // reduce score drastically so nodes prefer candidates with a live path.
  bool no_route = false;
  if (parent_health == 0 && rank > 0) {
    total = total / 4;
    no_route = true;
  }

  // Debug output showing score breakdown
  if (debug) {
    Serial.print("SCORE | 0x");
    Serial.print(node_id, HEX);
    Serial.print(" | rank=");
    Serial.print(rank);
    Serial.print(" (");
    Serial.print(60 * rank_score / 100);
    Serial.print(") | rssi=");
    Serial.print(rssi);
    Serial.print(" (");
    Serial.print(25 * rssi_score / 100);
    Serial.print(") | queue=");
    Serial.print(queue_pct);
    Serial.print("% (");
    Serial.print(15 * queue_score / 100);
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

// =============================================================================
// UPDATE PARENT SELECTION
// Scores all valid, non-stale candidates and switches parent if better found.
// Hysteresis: only switch if new score is ≥ PARENT_SWITCH_HYSTERESIS better.
// Also updates my_rank to parent.rank + 1.
// Sends a rapid beacon if parent changes (self-organizing).
// =============================================================================
void update_parent_selection() {
  uint8_t old_parent_id = get_parent_id();
  int   best_score = -1;
  uint8_t best_idx = 0xFF;

  // Count valid candidates for debug header
  int valid_count = 0;
  for (int i = 0; i < MAX_CANDIDATES; i++) {
    if (candidates[i].valid) {
      uint32_t age = millis() - candidates[i].last_seen_ms;
      if (age <= PARENT_TIMEOUT_MS) valid_count++;
    }
  }

  if (valid_count > 0) {
    Serial.println("─────────────────────────────────────────────");
    Serial.print("EVAL  | Comparing ");
    Serial.print(valid_count);
    Serial.println(" candidate(s):");
  }

  for (int i = 0; i < MAX_CANDIDATES; i++) {
    if (!candidates[i].valid) continue;

    // Skip stale candidates (self-healing)
    uint32_t age = millis() - candidates[i].last_seen_ms;
    if (age > PARENT_TIMEOUT_MS) continue;

    // Pass debug=true and node_id to show score breakdown
    int s = score_parent(candidates[i].rank,
                         candidates[i].rssi,
                         candidates[i].queue_pct,
                         candidates[i].parent_health,
                         true,  // debug output enabled
                         candidates[i].node_id);
    if (s > best_score) {
      best_score = s;
      best_idx   = i;
    }
  }

  if (best_idx == 0xFF) return;  // no valid candidates

  if (current_parent_idx == 0xFF) {
    // No current parent — take the best one immediately
    current_parent_idx = best_idx;
    my_rank = candidates[current_parent_idx].rank + 1;
    Serial.print("PARENT | Selected 0x");
    Serial.print(candidates[current_parent_idx].node_id, HEX);
    Serial.print(" (score=");
    Serial.print(best_score);
    Serial.print(", my_rank=");
    Serial.print(my_rank);
    Serial.println(")");
    Serial.println("─────────────────────────────────────────────");
  } else {
    // We have a current parent — only switch if score improvement >= hysteresis
    int current_score = score_parent(candidates[current_parent_idx].rank,
                                     candidates[current_parent_idx].rssi,
                                     candidates[current_parent_idx].queue_pct,
                                     candidates[current_parent_idx].parent_health);
    int score_diff = best_score - current_score;

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
      Serial.print(", diff=+");
      Serial.print(score_diff);
      Serial.print(" >= hysteresis=");
      Serial.print(PARENT_SWITCH_HYSTERESIS);
      Serial.println(")");
      current_parent_idx = best_idx;
      my_rank = candidates[current_parent_idx].rank + 1;
      Serial.println("─────────────────────────────────────────────");
    } else if (best_idx != current_parent_idx && score_diff > 0) {
      // Better candidate exists but doesn't meet hysteresis threshold
      Serial.print("PARENT | Keeping 0x");
      Serial.print(candidates[current_parent_idx].node_id, HEX);
      Serial.print(" (best=0x");
      Serial.print(candidates[best_idx].node_id, HEX);
      Serial.print(" diff=+");
      Serial.print(score_diff);
      Serial.print(" < hysteresis=");
      Serial.print(PARENT_SWITCH_HYSTERESIS);
      Serial.println(")");
      Serial.println("─────────────────────────────────────────────");
    }
  }

  // Rapid beacon if parent changed (self-organizing)
  uint8_t new_parent_id = get_parent_id();
  if (new_parent_id != old_parent_id && new_parent_id != 0xFF) {
    Serial.println("PARENT | Topology changed, sending rapid beacon");
    broadcast_beacon();
  }
}

// =============================================================================
// CHECK PARENT TIMEOUT
// If we haven't heard from the current parent in PARENT_TIMEOUT_MS, drop it.
// Also cleans up stale candidates so their slots can be reused.
// =============================================================================
void check_parent_timeout() {
  // Clean up all stale candidates (self-healing)
  for (int i = 0; i < MAX_CANDIDATES; i++) {
    if (!candidates[i].valid) continue;
    uint32_t age = millis() - candidates[i].last_seen_ms;
    if (age > PARENT_TIMEOUT_MS) {
      Serial.print("PARENT | Expired stale candidate 0x");
      Serial.println(candidates[i].node_id, HEX);
      candidates[i].valid = false;
      // If this was our current parent, clear it
      if (i == current_parent_idx) {
        current_parent_idx = 0xFF;
        my_rank = RANK_SENSOR;  // reset rank
      }
    }
  }

  // If we lost our current parent, try to find a new one
  if (current_parent_idx == 0xFF) {
    update_parent_selection();
    if (current_parent_idx == 0xFF) {
      // Still no parent — will keep waiting for beacons
      static uint32_t last_orphan_log = 0;
      if (millis() - last_orphan_log > 10000UL) {
        last_orphan_log = millis();
        Serial.println("PARENT | No parent available. Waiting for beacons...");
      }
    }
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
