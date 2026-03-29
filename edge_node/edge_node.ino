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
#include "../shared/logging.h"

#include <Wire.h>
#include <XPowersLib.h>
#include <RadioLib.h>
#include "config.h"
#include "../shared/mesh_protocol.h"

// ════════════════════════════════════════════════════════════════════════════
// HARDWARE OBJECTS
// ════════════════════════════════════════════════════════════════════════════

XPowersAXP2101 PMU;
SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);

// Uncomment the next line to enable LoRaWAN join + uplink (requires real TTN credentials in config.h)
#define ENABLE_LORAWAN

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
// LOCAL HEADERS (included AFTER all globals so externs resolve)
// ════════════════════════════════════════════════════════════════════════════

#include "edge_lorawan.h"
#include "edge_packets.h"

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

    // ──────────────────────────────────────────────────────────────────────
    // Initialize PMU (inline — edge cannot use mesh_radio.h due to
    // pin define conflicts with config.h)
    // ──────────────────────────────────────────────────────────────────────

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
    // PHASE 3: Initialize radio hardware (must happen before any radio use)
    // ──────────────────────────────────────────────────────────────────────

    LOG_INFO("Initializing SX1262 radio hardware...");

    int state = radio.begin(923.2);  // AS923 default join freq (not 434 MHz default!)
    if (state != RADIOLIB_ERR_NONE)
    {
        LOG_ERROR("RadioLib init failed");
        sprintf(buf, "Error code: %d", state);
        LOG_ERROR(buf);
        while (true)
            delay(1000);
    }
    LOG_OK("SX1262 hardware initialized");

    // ──────────────────────────────────────────────────────────────────────
    // PHASE 4: LORAWAN OTAA JOIN (radio is initialized but no mesh params)
    // LoRaWAN uses sync word 0x34; mesh uses 0x33. We must join before
    // setting mesh params, otherwise the Join Accept is invisible.
    // ──────────────────────────────────────────────────────────────────────

#ifdef ENABLE_LORAWAN
    Serial.println(F("\n[INIT] Starting LoRaWAN OTAA join..."));
    Serial.print(F("     DevEUI: "));
    Serial.println((unsigned long)DEV_EUI, HEX);
    Serial.print(F("     JoinEUI: "));
    Serial.println((unsigned long)JOIN_EUI, HEX);

    // Begin LoRaWAN with OTAA
    // RadioLib 7.x requires both nwkKey and appKey — for LoRaWAN 1.0.x they are the same
    state = lorawan_node.beginOTAA(JOIN_EUI, DEV_EUI, APP_KEY, APP_KEY);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.print(F("[ERROR] LoRaWAN begin failed, code "));
        Serial.println(state);
    }

    // Attempt OTAA join (blocking, waits for RX1/RX2 windows)
    state = lorawan_node.activateOTAA();
    if (state == RADIOLIB_LORAWAN_NEW_SESSION)
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

    // Set DR5 (SF7/BW125) — default join DR2 (SF10) exceeds AS923 400ms dwell time
    // for our 94-byte payload. DR5 supports up to ~242 bytes within dwell limits.
    lorawan_node.setDatarate(5);

    lorawan_enabled = true;
#else
    Serial.println(F("\n[INIT] LoRaWAN DISABLED (mesh-only testing mode)"));
#endif

    // ──────────────────────────────────────────────────────────────────────
    // PHASE 5: Configure radio for mesh (after LoRaWAN join completes)
    // ──────────────────────────────────────────────────────────────────────

    LOG_INFO("Reinitializing radio for mesh...");

    // Full reinit to cleanly reset all SX1262 registers after LoRaWAN join
    // (individual set calls don't fully undo what LoRaWAN changed internally)
    state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING,
                        LORA_CODING_RATE, LORA_SYNC_WORD, LORA_TX_POWER);
    if (state != RADIOLIB_ERR_NONE)
    {
        LOG_ERROR("Radio mesh reinit failed");
        sprintf(buf, "Error code: %d", state);
        LOG_ERROR(buf);
        while (true)
            delay(1000);
    }

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
        else if ((timeout_flush || buffer_flush) && agg_count > 0 && !lorawan_joined)
        {
            // LoRaWAN join failed: drain buffer so queue% doesn't stay at 100%
            Serial.print(F("\n[FLUSH] LoRaWAN not joined — discarding "));
            Serial.print(agg_count);
            Serial.println(F(" records"));
            agg_count = 0;
            last_flush_time = now;
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
