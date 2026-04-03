// ════════════════════════════════════════════════════════════════════════════
// EDGE NODE — LORAWAN UPLINK & RADIO MODE SWITCHING
// CSC2106 Group 33
//
// Contains:
//   - send_lorawan_uplink()  — pack BridgeAggV1 format and send via LoRaWAN
//   - switch_to_mesh_rx()    — reinit radio for mesh mode after LoRaWAN TX
//
// Included by edge_node.ino AFTER all globals are declared.
// ════════════════════════════════════════════════════════════════════════════

#ifndef EDGE_LORAWAN_H
#define EDGE_LORAWAN_H

// ════════════════════════════════════════════════════════════════════════════
// EXTERN DECLARATIONS (defined in edge_node.ino)
// ════════════════════════════════════════════════════════════════════════════

extern AggRecord agg_buffer[];
extern uint8_t agg_count;
extern uint32_t last_flush_time;
extern bool lorawan_joined;
extern uint32_t uplinks_sent;
extern uint32_t boot_time;
extern SX1262 radio;
extern volatile bool rxFlag;

#ifdef ENABLE_LORAWAN
extern LoRaWANNode lorawan_node;
#endif

// ════════════════════════════════════════════════════════════════════════════
// LORAWAN UPLINK (PHASE 4 & 5)
// ════════════════════════════════════════════════════════════════════════════

inline void send_lorawan_uplink()
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

    // Records (16 bytes each — includes latency_ms field)
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
        uplink_buf[idx++] = (rec->latency_ms >> 8) & 0xFF;    // Latency high byte
        uplink_buf[idx++] = rec->latency_ms & 0xFF;           // Latency low byte
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

inline void switch_to_mesh_rx()
{
    // Full reinit to cleanly reset SX1262 after LoRaWAN TX
    radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING,
                LORA_CODING_RATE, LORA_SYNC_WORD, LORA_TX_POWER);

    radio.setDio1Action(setRxFlag);
    rxFlag = false;
    radio.startReceive();

    Serial.println(F("[RADIO] Switched back to MESH_LISTEN mode"));
}

#endif // EDGE_LORAWAN_H
