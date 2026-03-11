// ════════════════════════════════════════════════════════════════════════════
// EDGE NODE FIRMWARE - CSC2106 Group 33 (Person 3)
//
// Role: Mesh sink (rank 0) + LoRaWAN uplink to The Things Network
//
// Hardware: LilyGo T-Beam (SX1262 LoRa radio + AXP2101 PMU)
//
// Responsibilities:
//   1. Listen for mesh packets from sensors/relay
//   2. Validate and acknowledge received packets
//   3. Aggregate sensor readings into BridgeAggV1 format
//   4. Send aggregated data to TTN via LoRaWAN (OTAA)
//   5. Broadcast rank-0 beacons every 10s
//   6. Time-share one radio between mesh RX and LoRaWAN TX
//
// BEFORE FLASHING:
//   1. Install libraries: RadioLib, XPowersLib (Arduino Library Manager)
//   2. Edit config.h: Set NODE_ID (0x01 or 0x06) and OTAA credentials from Person 4
//   3. Attach LoRa antenna to SMA connector (CRITICAL!)
// ════════════════════════════════════════════════════════════════════════════

// CHANGE THIS BEFORE FLASHING: EDGE-01 for Node 0x01, EDGE-06 for Node 0x06
#define NODE_NAME "EDGE-06"
#include "logging.h"

#include <Wire.h>
#include <XPowersLib.h>
#include <RadioLib.h>
#include "config.h"
#include "mesh_protocol.h"

// ════════════════════════════════════════════════════════════════════════════
// HARDWARE OBJECTS
// ════════════════════════════════════════════════════════════════════════════

XPowersAXP2101 PMU;
SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);

// Uncomment the next line to enable LoRaWAN join + uplink (requires real TTN credentials in config.h)
// #define ENABLE_LORAWAN

#ifdef ENABLE_LORAWAN
LoRaWANNode lorawan_node(&radio, &AS923); // AS923 for Singapore
#endif

// ════════════════════════════════════════════════════════════════════════════
// DIO1 RECEIVE INTERRUPT FLAG
// SX1262 signals packet arrival via DIO1 interrupt, not a polled available().
// ════════════════════════════════════════════════════════════════════════════

volatile bool rxFlag = false;

void IRAM_ATTR setRxFlag()
{
    rxFlag = true;
}

// ════════════════════════════════════════════════════════════════════════════
// RADIO MODE (Time-sharing between mesh and LoRaWAN)
// ════════════════════════════════════════════════════════════════════════════

enum RadioMode
{
    MESH_LISTEN, // Listening for mesh packets (default state)
    LORAWAN_TXRX // Sending LoRaWAN uplink + waiting for RX windows
};

RadioMode radio_mode = MESH_LISTEN;
bool lorawan_joined = false;
bool lorawan_enabled = false;

// ════════════════════════════════════════════════════════════════════════════
// AGGREGATION BUFFER (stores up to 7 sensor readings)
// ════════════════════════════════════════════════════════════════════════════

#define MAX_OPAQUE_PAYLOAD 32  // Max payload bytes we'll store (any sensor type)

struct AggRecord
{
    uint8_t mesh_src_id;
    uint8_t mesh_seq;
    uint8_t hop_estimate;
    uint16_t edge_uptime_s;
    uint8_t opaque_len;                    // Actual payload length from mesh header
    uint8_t opaque_payload[MAX_OPAQUE_PAYLOAD]; // Raw payload bytes (sensor-agnostic)
    bool valid;
};

AggRecord agg_buffer[MAX_AGG_RECORDS];
uint8_t agg_count = 0;
uint32_t last_flush_time = 0;

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION TABLE
// ════════════════════════════════════════════════════════════════════════════

DedupEntry dedup_table[DEDUP_TABLE_SIZE];
uint8_t dedup_index = 0;

// ════════════════════════════════════════════════════════════════════════════
// TIMERS & COUNTERS
// ════════════════════════════════════════════════════════════════════════════

uint32_t last_beacon_time = 0;
uint32_t boot_time = 0;
uint32_t packets_received = 0;
uint32_t packets_dropped = 0;
uint32_t uplinks_sent = 0;

// ════════════════════════════════════════════════════════════════════════════
// SETUP - RUNS ONCE AT BOOT
// ════════════════════════════════════════════════════════════════════════════

void setup()
{
    Serial.begin(115200);
    delay(1000);

    log_boot_banner("Edge Node");

    char buf[64];
    sprintf(buf, "Node ID: 0x%02X | Rank: %d", NODE_ID, MY_RANK);
    LOG_INFO(buf);

    // Initialize PMU
    LOG_INFO("Initializing PMU (AXP2101)...");
    Wire.begin(PMU_SDA, PMU_SCL);

    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL))
    {
        LOG_ERROR("PMU init failed! Check I2C wiring.");
        while (true)
            delay(1000);
    }

    PMU.setALDO2Voltage(3300);
    PMU.enableALDO2();
    LOG_OK("PMU initialized - LoRa radio powered via ALDO2");

    delay(100);

    // ──────────────────────────────────────────────────────────────────────
    // PHASE 3: LORAWAN OTAA JOIN (BEFORE mesh init — radio must be clean)
    // LoRaWAN uses sync word 0x34; mesh uses 0x33. If mesh inits first,
    // the Join Accept from TTN is invisible to the radio.
    // ──────────────────────────────────────────────────────────────────────

#ifdef ENABLE_LORAWAN
    Serial.println(F("\n[INIT] Starting LoRaWAN OTAA join..."));
    Serial.print(F("     DevEUI: "));
    Serial.println((unsigned long)DEV_EUI, HEX);
    Serial.print(F("     JoinEUI: "));
    Serial.println((unsigned long)JOIN_EUI, HEX);

    // Begin LoRaWAN with OTAA (radio is fresh, no mesh settings to conflict)
    int state = lorawan_node.beginOTAA(JOIN_EUI, DEV_EUI, nullptr, APP_KEY);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.print(F("[ERROR] LoRaWAN begin failed, code "));
        Serial.println(state);
    }

    // Attempt OTAA join (blocking, up to 60s)
    state = lorawan_node.activateOTAA();
    if (state == RADIOLIB_ERR_NONE)
    {
        Serial.println(F("[OK] LoRaWAN OTAA join successful!"));
        lorawan_joined = true;
    }
    else
    {
        Serial.print(F("[WARN] LoRaWAN join failed, code "));
        Serial.println(state);
        Serial.println(F("     Will retry on next uplink attempt"));
    }

    lorawan_enabled = true;
#else
    Serial.println(F("\n[INIT] LoRaWAN DISABLED (mesh-only testing mode)"));
#endif

    // ──────────────────────────────────────────────────────────────────────
    // PHASE 4: Initialize radio for mesh (after LoRaWAN join completes)
    // ──────────────────────────────────────────────────────────────────────

    LOG_INFO("Initializing RadioLib for mesh...");

#ifdef ENABLE_LORAWAN
    state = radio.begin(LORA_FREQUENCY);
#else
    int state = radio.begin(LORA_FREQUENCY);
#endif
    if (state != RADIOLIB_ERR_NONE)
    {
        LOG_ERROR("RadioLib init failed");
        sprintf(buf, "Error code: %d", state);
        LOG_ERROR(buf);
        while (true)
            delay(1000);
    }

    radio.setSpreadingFactor(LORA_SPREADING);
    radio.setBandwidth(LORA_BANDWIDTH);
    radio.setCodingRate(LORA_CODING_RATE);
    radio.setSyncWord(LORA_SYNC_WORD);
    radio.setOutputPower(LORA_TX_POWER);

    sprintf(buf, "Radio ready: %.1f MHz, SF%d, BW%.0f kHz",
            LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH);
    LOG_OK(buf);

    // Set up DIO1 interrupt for non-blocking receive
    radio.setDio1Action(setRxFlag);

    // Start receiving mesh packets
    rxFlag = false;
    radio.startReceive();

    // ──────────────────────────────────────────────────────────────────────
    // INITIALIZATION COMPLETE
    // ──────────────────────────────────────────────────────────────────────

    memset(agg_buffer, 0, sizeof(agg_buffer));
    memset(dedup_table, 0, sizeof(dedup_table));

    boot_time = millis();
    last_flush_time = millis();
    last_beacon_time = millis() - BEACON_INTERVAL_MS + BEACON_PHASE_OFFSET;

    Serial.println(F("\n════════════════════════════════════════════════════"));
    Serial.println(F("Edge node ready! Listening for mesh packets..."));
    Serial.println(F("════════════════════════════════════════════════════\n"));
}

// ════════════════════════════════════════════════════════════════════════════
// MAIN LOOP - COOPERATIVE TIME-SHARING
// ════════════════════════════════════════════════════════════════════════════

void loop()
{
    uint32_t now = millis();

    if (radio_mode == MESH_LISTEN)
    {
        // ──────────────────────────────────────────────────────────────────
        // NORMAL STATE: Listen for mesh packets
        // ──────────────────────────────────────────────────────────────────

        receive_mesh_packets();
        broadcast_beacon_if_due();

        // Check if we need to flush to LoRaWAN
        bool timeout_flush = (now - last_flush_time >= AGG_FLUSH_TIMEOUT_MS);
        bool buffer_flush = (agg_count >= MAX_AGG_RECORDS);

#ifdef ENABLE_LORAWAN
        if ((timeout_flush || buffer_flush) && agg_count > 0 && lorawan_joined && lorawan_enabled)
        {
            // Switch to LoRaWAN mode to send uplink
            Serial.println(F("\n[FLUSH] Trigger detected, switching to LoRaWAN mode"));
            radio_mode = LORAWAN_TXRX;
            send_lorawan_uplink();
            switch_to_mesh_rx();
            radio_mode = MESH_LISTEN;
        }
#else
        // LoRaWAN disabled: drain buffer periodically so queue% doesn't stay at 100%
        if ((timeout_flush || buffer_flush) && agg_count > 0)
        {
            Serial.print(F("\n[FLUSH] LoRaWAN disabled — discarding "));
            Serial.print(agg_count);
            Serial.println(F(" aggregated records (would send if LoRaWAN enabled)"));
            agg_count = 0;
            last_flush_time = now;
        }
#endif
    }
    else
    {
        // ──────────────────────────────────────────────────────────────────
        // LORAWAN_TXRX STATE: Currently handled synchronously in send_lorawan_uplink()
        // This state is temporary (~5s) and automatically returns to MESH_LISTEN
        // ──────────────────────────────────────────────────────────────────
    }

    delay(2); // Small tick delay to prevent tight looping
}

// ════════════════════════════════════════════════════════════════════════════
// MESH PACKET RECEPTION (PHASE 2)
// ════════════════════════════════════════════════════════════════════════════

void receive_mesh_packets()
{
    // Check if DIO1 interrupt fired (packet arrived)
    if (!rxFlag)
    {
        return;
    }
    rxFlag = false;

    // Get the actual packet length BEFORE readData
    int len = radio.getPacketLength();
    uint8_t buf[128];
    int state = radio.readData(buf, sizeof(buf));
    int rssi = radio.getRSSI();
    float snr = radio.getSNR();

    // Restart RX immediately so the radio doesn't sit in standby while we
    // process (or drop) this packet. Mirrors the relay_node pattern.
    rxFlag = false;
    radio.startReceive();

    // Check if readData succeeded
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.print(F("[DROP] readData error, code "));
        Serial.println(state);
        return;
    }

    packets_received++;

    // ──────────────────────────────────────────────────────────────────────
    // VALIDATION STAGE 1: Minimum length check
    // ──────────────────────────────────────────────────────────────────────

    if (len < MESH_HEADER_SIZE)
    {
        Serial.print(F("[DROP] Packet too short ("));
        Serial.print(len);
        Serial.println(F(" bytes)"));
        packets_dropped++;
        return;
    }

    MeshHeader *hdr = (MeshHeader *)buf;
    uint8_t *payload = buf + MESH_HEADER_SIZE;

    // ──────────────────────────────────────────────────────────────────────
    // VALIDATION STAGE 2: Self-check (ignore own packets)
    // ──────────────────────────────────────────────────────────────────────

    if (hdr->src_id == NODE_ID)
    {
        // This is our own packet echoing back, ignore it
        return;
    }

    // ──────────────────────────────────────────────────────────────────────
    // VALIDATION STAGE 3: CRC check (before dedup, so corrupted packets
    // don't pollute the dedup table)
    // ──────────────────────────────────────────────────────────────────────

    if (!validate_mesh_crc(hdr))
    {
        Serial.print(F("[DROP] CRC_FAIL | src=0x"));
        Serial.print(hdr->src_id, HEX);
        Serial.print(F(" seq="));
        Serial.println(hdr->seq_num);
        packets_dropped++;
        return;
    }

    // ──────────────────────────────────────────────────────────────────────
    // VALIDATION STAGE 4: Packet type — handle beacons and ACKs early
    // (beacons must NOT go through dedup since they reuse seq_num=0)
    // ──────────────────────────────────────────────────────────────────────

    uint8_t pkt_type = GET_PKT_TYPE(hdr->flags);

    if (pkt_type == PKT_TYPE_BEACON)
    {
        handle_beacon(hdr, rssi, snr);
        return;
    }

    if (pkt_type == PKT_TYPE_ACK)
    {
        // Edge nodes don't process ACKs (we don't send data upward in mesh)
        return;
    }

    // ──────────────────────────────────────────────────────────────────────
    // VALIDATION STAGE 5: Destination check (DATA packets must be for us)
    // ──────────────────────────────────────────────────────────────────────

    if (hdr->dst_id != NODE_ID && hdr->dst_id != 0xFF)
    {
        return;  // Not addressed to us — ignore
    }

    // ──────────────────────────────────────────────────────────────────────
    // VALIDATION STAGE 6: Deduplication check (DATA packets only)
    // ──────────────────────────────────────────────────────────────────────

    if (is_duplicate(hdr->src_id, hdr->seq_num))
    {
        Serial.print(F("[DROP] Duplicate | src=0x"));
        Serial.print(hdr->src_id, HEX);
        Serial.print(F(" seq="));
        Serial.println(hdr->seq_num);
        packets_dropped++;
        return;
    }

    // ──────────────────────────────────────────────────────────────────────
    // VALIDATION STAGE 6: TTL check
    // ──────────────────────────────────────────────────────────────────────

    if (hdr->ttl == 0)
    {
        Serial.print(F("[DROP] TTL=0 | src=0x"));
        Serial.print(hdr->src_id, HEX);
        Serial.print(F(" seq="));
        Serial.println(hdr->seq_num);
        packets_dropped++;
        return;
    }

    // ──────────────────────────────────────────────────────────────────────
    // DATA PACKET VALID: Mark as seen and process
    // ──────────────────────────────────────────────────────────────────────

    mark_seen(hdr->src_id, hdr->seq_num);
    handle_data(hdr, payload, rssi, snr);
}

// ════════════════════════════════════════════════════════════════════════════
// BEACON HANDLING
// ════════════════════════════════════════════════════════════════════════════

void handle_beacon(MeshHeader *hdr, int rssi, float snr)
{
    Serial.print(F("[BCN] RX from 0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" | rank="));
    Serial.print(hdr->rank);
    Serial.print(F(" | rssi="));
    Serial.println(rssi);

    // Edge nodes don't select parents (we ARE the top of the hierarchy)
    // But we can log beacons for debugging/visibility
}

void broadcast_beacon_if_due()
{
    if (millis() - last_beacon_time < BEACON_INTERVAL_MS)
    {
        return;
    }

    // Build MeshHeader
    MeshHeader hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.flags = PKT_TYPE_BEACON;
    hdr.src_id = NODE_ID;
    hdr.dst_id = 0xFF; // Broadcast
    hdr.prev_hop = NODE_ID;
    hdr.ttl = 1;                              // Beacons don't hop
    hdr.seq_num = 0x00;                       // Beacons don't use sequence numbers
    hdr.rank = MY_RANK;                       // Always 0 for edge
    hdr.payload_len = BEACON_PAYLOAD_SIZE;

    // Compute CRC over bytes 0-7
    set_mesh_crc(&hdr);

    // Build BeaconPayload
    BeaconPayload bcn;
    bcn.schema_version = 0x01;
    bcn.queue_pct = (agg_count * 100) / MAX_AGG_RECORDS; // Real queue %
    bcn.link_quality = 100;                              // Edge has perfect "link" to itself
    bcn.parent_health = lorawan_joined ? 100 : 0;        // Health = LoRaWAN status

    // Transmit
    uint8_t pkt[MESH_HEADER_SIZE + BEACON_PAYLOAD_SIZE];
    memcpy(pkt, &hdr, MESH_HEADER_SIZE);
    memcpy(pkt + MESH_HEADER_SIZE, &bcn, BEACON_PAYLOAD_SIZE);

    int state = lbt_transmit(radio, pkt, sizeof(pkt));

    if (state == RADIOLIB_ERR_NONE)
    {
        Serial.print(F("[BCN] TX | rank="));
        Serial.print(MY_RANK);
        Serial.print(F(" | queue="));
        Serial.print(bcn.queue_pct);
        Serial.println(F("%"));
    }
    else
    {
        Serial.print(F("[BCN] TX failed, code "));
        Serial.println(state);
    }

    last_beacon_time = millis();
    rxFlag = false;
    radio.startReceive(); // Return to RX mode
}

// ════════════════════════════════════════════════════════════════════════════
// DATA PACKET HANDLING (PHASE 3)
// ════════════════════════════════════════════════════════════════════════════

void handle_data(MeshHeader *hdr, uint8_t *payload, int rssi, float snr)
{
    uint8_t hop_count = DEFAULT_TTL - hdr->ttl; // Estimate hops from remaining TTL

    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.print(F("RX_MESH | src=0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" | seq="));
    Serial.print(hdr->seq_num);
    Serial.print(F(" | hops="));
    Serial.print(hop_count);
    Serial.print(F(" | rssi="));
    Serial.println(rssi);

    // ──────────────────────────────────────────────────────────────────────
    // SEND ACK (if requested)
    // ──────────────────────────────────────────────────────────────────────

    if (IS_ACK_REQUESTED(hdr->flags))
    {
        send_ack(hdr->prev_hop, hdr->seq_num);
    }

    // ──────────────────────────────────────────────────────────────────────
    // ADD TO AGGREGATION BUFFER (PHASE 4)
    // ──────────────────────────────────────────────────────────────────────

    // Accept any payload length (payload-agnostic), clamp to max buffer size
    uint8_t copy_len = hdr->payload_len;
    if (copy_len > MAX_OPAQUE_PAYLOAD) copy_len = MAX_OPAQUE_PAYLOAD;

    if (agg_count < MAX_AGG_RECORDS && copy_len > 0)
    {
        AggRecord *rec = &agg_buffer[agg_count];

        rec->mesh_src_id = hdr->src_id;
        rec->mesh_seq = hdr->seq_num;
        rec->hop_estimate = hop_count;
        rec->edge_uptime_s = (millis() - boot_time) / 1000;
        rec->opaque_len = copy_len;
        memcpy(rec->opaque_payload, payload, copy_len);
        rec->valid = true;

        agg_count++;

        Serial.print(F("AGG | Added to buffer | count="));
        Serial.print(agg_count);
        Serial.print(F("/"));
        Serial.println(MAX_AGG_RECORDS);
    }
    else
    {
        Serial.println(F("[WARN] Aggregation buffer full or zero-length payload"));
    }

    Serial.println(F("────────────────────────────────────────────────────"));
}

// ════════════════════════════════════════════════════════════════════════════
// ACK SENDING (PHASE 3)
// ════════════════════════════════════════════════════════════════════════════

void send_ack(uint8_t to_node, uint8_t seq)
{
    MeshHeader ack;
    memset(&ack, 0, sizeof(ack));

    ack.flags = PKT_TYPE_ACK;
    ack.src_id = NODE_ID;
    ack.dst_id = to_node;
    ack.prev_hop = NODE_ID;
    ack.ttl = 1;
    ack.seq_num = seq; // Echo the sequence number
    ack.rank = MY_RANK;
    ack.payload_len = 0;
    set_mesh_crc(&ack);

    delay(random(10, 50)); // Small jitter to avoid collision

    int state = lbt_transmit(radio, (uint8_t *)&ack, MESH_HEADER_SIZE);

    if (state == RADIOLIB_ERR_NONE)
    {
        Serial.print(F("ACK | to=0x"));
        Serial.print(to_node, HEX);
        Serial.print(F(" | seq="));
        Serial.println(seq);
    }

    rxFlag = false;
    radio.startReceive(); // Return to RX mode
}

// ════════════════════════════════════════════════════════════════════════════
// LORAWAN UPLINK (PHASE 4 & 5)
// ════════════════════════════════════════════════════════════════════════════

void send_lorawan_uplink()
{
    if (agg_count == 0)
    {
        Serial.println(F("[FLUSH] No records to send, skipping uplink"));
        return;
    }

    // ──────────────────────────────────────────────────────────────────────
    // PACK BRIDGEAGGV1 FORMAT
    // ──────────────────────────────────────────────────────────────────────

    uint8_t uplink_buf[242]; // Max AS923 LoRaWAN payload size for SF7 is ~242 bytes
    uint8_t idx = 0;

    // Header (3 bytes)
    uplink_buf[idx++] = BRIDGE_AGG_SCHEMA_V1; // schema_version = 0x02
    uplink_buf[idx++] = NODE_ID;              // bridge_id
    uplink_buf[idx++] = agg_count;            // record_count

    // Records (14 bytes each)
    for (uint8_t i = 0; i < agg_count; i++)
    {
        if (!agg_buffer[i].valid)
            continue;

        AggRecord *rec = &agg_buffer[i];

        uplink_buf[idx++] = rec->mesh_src_id;
        uplink_buf[idx++] = rec->mesh_seq;
        uplink_buf[idx++] = rec->hop_estimate;
        uplink_buf[idx++] = (rec->edge_uptime_s >> 8) & 0xFF; // High byte
        uplink_buf[idx++] = rec->edge_uptime_s & 0xFF;        // Low byte
        uplink_buf[idx++] = rec->opaque_len;                   // actual payload length
        memcpy(&uplink_buf[idx], rec->opaque_payload, rec->opaque_len);
        idx += rec->opaque_len;
    }

    Serial.println(F("\n════════════════════════════════════════════════════"));
    Serial.print(F("FLUSH | records="));
    Serial.print(agg_count);
    Serial.print(F(" | bytes="));
    Serial.println(idx);

    // ──────────────────────────────────────────────────────────────────────
    // SEND VIA LORAWAN (blocking, ~5s for RX windows)
    // ──────────────────────────────────────────────────────────────────────

#ifdef ENABLE_LORAWAN
    int state = lorawan_node.sendReceive(uplink_buf, idx, LORAWAN_FPORT);

    if (state == RADIOLIB_ERR_NONE)
    {
        Serial.println(F("UPLINK | OK | TTN received"));
        uplinks_sent++;
    }
    else if (state == RADIOLIB_ERR_RX_TIMEOUT)
    {
        Serial.println(F("UPLINK | OK (no downlink)"));
        uplinks_sent++;
    }
    else
    {
        Serial.print(F("UPLINK | FAILED | code "));
        Serial.println(state);
    }
#endif

    Serial.print(F("Total uplinks sent: "));
    Serial.println(uplinks_sent);
    Serial.println(F("════════════════════════════════════════════════════\n"));

    // ──────────────────────────────────────────────────────────────────────
    // CLEAR AGGREGATION BUFFER
    // ──────────────────────────────────────────────────────────────────────

    memset(agg_buffer, 0, sizeof(agg_buffer));
    agg_count = 0;
    last_flush_time = millis();
}

// ════════════════════════════════════════════════════════════════════════════
// RADIO MODE SWITCHING (PHASE 5)
// ════════════════════════════════════════════════════════════════════════════

void switch_to_mesh_rx()
{
    // Reconfigure radio back to mesh LoRa parameters
    radio.setFrequency(LORA_FREQUENCY);
    radio.setSpreadingFactor(LORA_SPREADING);
    radio.setBandwidth(LORA_BANDWIDTH);
    radio.setCodingRate(LORA_CODING_RATE);
    radio.setSyncWord(LORA_SYNC_WORD);
    radio.setOutputPower(LORA_TX_POWER);

    rxFlag = false;
    radio.startReceive();

    Serial.println(F("[RADIO] Switched back to MESH_LISTEN mode"));
}

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION HELPERS
// ════════════════════════════════════════════════════════════════════════════

bool is_duplicate(uint8_t src_id, uint8_t seq_num)
{
    uint32_t now = millis();

    for (uint8_t i = 0; i < DEDUP_TABLE_SIZE; i++)
    {
        if (!dedup_table[i].valid) continue;

        // Expire old entries
        if (now - dedup_table[i].timestamp > DEDUP_WINDOW_MS)
        {
            dedup_table[i].valid = false;
            continue;
        }

        if (dedup_table[i].src_id == src_id &&
            dedup_table[i].seq_num == seq_num)
        {
            return true;
        }
    }

    return false;
}

void mark_seen(uint8_t src_id, uint8_t seq_num)
{
    dedup_table[dedup_index].src_id = src_id;
    dedup_table[dedup_index].seq_num = seq_num;
    dedup_table[dedup_index].timestamp = millis();
    dedup_table[dedup_index].valid = true;

    dedup_index = (dedup_index + 1) % DEDUP_TABLE_SIZE;
}
