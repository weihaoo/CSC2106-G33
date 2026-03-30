// ============================================================================
// CSC2106 Group 33 -- LoRa Mesh Sensor Node (with Relay Capability)
// Singapore Institute of Technology
//
// Reads DHT22 sensor data and sends it toward the edge every TX_INTERVAL_MS.
// Also relays DATA packets from downstream nodes (payload-agnostic).
//
// NOTE: Change NODE_ID below before flashing to each sensor node.
//       Sensor 1 = 0x03, Sensor 2 = 0x04
// ============================================================================

// CHANGE THIS BEFORE FLASHING: Sensor 1 = SENSOR-03, Sensor 2 = SENSOR-04
#define NODE_NAME "SENSOR-03"
#include "../shared/logging.h"

// -----------------------------------------------------------------------------
// NODE CONFIGURATION -- Change before flashing!
// Must be defined BEFORE including mesh_common.h
// -----------------------------------------------------------------------------
#define NODE_ID       0x03    // 0x03 = Sensor 1, 0x04 = Sensor 2

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
#define TX_INTERVAL_MS          5000UL   // 30s during development (change to 120000UL for final)
#define BEACON_PHASE_OFFSET_MS  ((NODE_ID % 5) * 2000UL)

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

// ============================================================================
// GLOBAL VARIABLE DEFINITIONS
// These satisfy the extern declarations in mesh_radio.h and mesh_common.h.
// ============================================================================

XPowersAXP2101 PMU;
SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);
volatile bool rxFlag = false;

// Parent tracking
ParentInfo candidates[MAX_CANDIDATES];
uint8_t    current_parent_idx = 0xFF;  // index into candidates[], 0xFF = none yet
uint8_t    my_rank = RANK_SENSOR;      // Dynamic rank: starts at 2, updated to parent.rank+1

// Dedup table (circular buffer)
DedupEntry dedup_table[DEDUP_TABLE_SIZE];
uint8_t    dedup_head = 0;

// Forwarding queue counter
volatile uint8_t pending_forwards = 0;

// Parent whitelist: only accept beacons from primary edge (0x01) and relay (0x02).
// This prevents the sensor from shortcutting directly to the secondary edge (0x06),
// which would bypass the relay and break multi-hop routing in failover tests.
const uint8_t allowed_parents[] = {0x01, 0x02};
const uint8_t allowed_parents_count = 2;

// Sequence number
uint8_t seq_num = 0;

// ACK tracking
volatile bool ack_received      = false;
uint8_t       ack_expected_seq  = 0;
bool          waiting_for_ack   = false;

// ============================================================================
// LOCAL HEADERS (must come after global definitions they reference)
// ============================================================================
#include "sensor_routing.h"
#include "sensor_packets.h"

// ============================================================================
// SENSOR READ
// Reads temperature and humidity. Uses random data in SIMULATION_MODE,
// real DHT22 data otherwise.
// ============================================================================
bool read_sensor(float &temp, float &hum) {
#if SIMULATION_MODE
    // Generate random but realistic values
    temp = 20.0 + (random(0, 151) / 10.0);   // 20.0-35.0 C
    hum  = 50.0 + (random(0, 401) / 10.0);   // 50.0-90.0 %
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

// ============================================================================
// BUILD SENSOR PAYLOAD (SENSOR_PAYLOAD_SIZE = 7 bytes)
//
// Byte layout (big-endian integers):
//   [0] schema_version = 0x01
//   [1] sensor_type    = 0x03 (DHT22)
//   [2-3] temp_c_x10   (signed int16, big-endian)
//   [4-5] humidity_x10 (unsigned int16, big-endian)
//   [6] status         (0x00 = OK, 0x01 = read error)
// ============================================================================
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

// ============================================================================
// BUILD MESH HEADER (MESH_HEADER_SIZE = 10 bytes) into packet[0..9]
// ============================================================================
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

// ============================================================================
// BROADCAST BEACON
// Sends a beacon so downstream nodes know this sensor exists as a relay option.
// ============================================================================
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

    // Compute link_quality from parent RSSI (map [-120,-60] -> [0,100])
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

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    log_boot_banner("Sensor Node");

    char buf[64];
    sprintf(buf, "Firmware %s | Node 0x%02X", FIRMWARE_VERSION, NODE_ID);
    LOG_INFO(buf);

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

    // Start in RX mode -- event-driven loop will handle everything
    rxFlag = false;
    radio.startReceive();

    LOG_INFO("Sensor node ready. Listening for parent beacons...");
}

// ============================================================================
// MAIN LOOP -- EVENT-DRIVEN (non-blocking)
//
//   1. Beacons periodically (so downstream nodes can discover us)
//   2. Checks parent timeout (every 5s)
//   3. Processes any incoming packet (beacon, data, ack) -- non-blocking
//   4. Sends sensor data on a timer (with ACK + retry + strikes)
// ============================================================================
void loop() {
    static uint32_t last_tx_ms = 0;
    static uint32_t last_beacon_ms = millis();

    // --- Step 1: Broadcast beacon if due ---
    uint32_t now = millis();
    if (now - last_beacon_ms >= BEACON_INTERVAL_MS) {
        last_beacon_ms = now;
        broadcast_beacon();
    }

    // --- Step 2: Check if parent has timed out (every 5s, not every loop) ---
    {
        static uint32_t last_stale_check_ms = 0;
        if (now - last_stale_check_ms >= 5000UL) {
            last_stale_check_ms = now;
            check_parent_timeout();
        }
    }

    // --- Step 3: Process incoming packets (non-blocking) ---
    receive_and_process();

    // --- Step 4: Send sensor data if TX interval has elapsed ---
    now = millis();
    if (now - last_tx_ms >= TX_INTERVAL_MS && has_valid_parent()) {
        last_tx_ms = now;

        // --- Read sensor ---
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

        // --- Apply TX jitter (0-2000ms) ---
        uint32_t jitter = random(0, 2000);
        Serial.print("TX | Jitter delay: ");
        Serial.print(jitter);
        Serial.println(" ms");
        delay(jitter);

        // --- Send with ACK + retry ---
        bool success = send_with_ack(packet, MESH_HEADER_SIZE + SENSOR_PAYLOAD_SIZE);

        if (success) {
            // Reset strike counter on successful delivery
            if (current_parent_idx < MAX_CANDIDATES) {
                candidates[current_parent_idx].fail_count = 0;
            }
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
            // Strikes system: don't invalidate after a single failure round.
            // Marginal links can recover from transient fading events.
            if (current_parent_idx < MAX_CANDIDATES) {
                candidates[current_parent_idx].fail_count++;
                Serial.print("TX | Strike ");
                Serial.print(candidates[current_parent_idx].fail_count);
                Serial.print("/");
                Serial.println(MAX_PARENT_STRIKES);

                if (candidates[current_parent_idx].fail_count >= MAX_PARENT_STRIKES) {
                    Serial.println("TX | Max strikes reached. Invalidating parent.");
                    candidates[current_parent_idx].valid = false;
                    current_parent_idx = 0xFF;
                    my_rank = RANK_SENSOR;
                    update_parent_selection();
                    broadcast_beacon();
                }
            } else {
                // No current parent index -- just re-evaluate
                current_parent_idx = 0xFF;
                my_rank = RANK_SENSOR;
                update_parent_selection();
                broadcast_beacon();
            }
        }
    }
}
