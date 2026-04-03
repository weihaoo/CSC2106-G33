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
extern uint32_t metrics_window_start_ms;
extern uint32_t metrics_window_delivered;
extern uint32_t metrics_window_onair_bytes;
extern uint32_t metrics_total_delivered;
extern uint32_t metrics_total_expected;
extern uint32_t metrics_total_lost;
extern uint32_t metrics_hop_samples;
extern uint32_t metrics_hop_sum;
extern uint32_t metrics_latency_samples;
extern int64_t metrics_latency_sum_ms;

#ifdef ENABLE_LORAWAN
extern LoRaWANNode lorawan_node;
#endif

// Optional metrics trailer appended after BridgeAggV1 records.
#define BRIDGE_METRICS_TAG          0xA1
#define BRIDGE_METRICS_VER          0x01
#define BRIDGE_METRICS_TRAILER_LEN  32

inline void append_u16_be(uint8_t *buf, uint8_t &idx, uint16_t v)
{
    buf[idx++] = (v >> 8) & 0xFF;
    buf[idx++] = v & 0xFF;
}

inline void append_u32_be(uint8_t *buf, uint8_t &idx, uint32_t v)
{
    buf[idx++] = (v >> 24) & 0xFF;
    buf[idx++] = (v >> 16) & 0xFF;
    buf[idx++] = (v >> 8) & 0xFF;
    buf[idx++] = v & 0xFF;
}

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

    // Append edge-calculated runtime metrics for dashboard visualization.
    if ((uint16_t)idx + BRIDGE_METRICS_TRAILER_LEN <= sizeof(uplink_buf))
    {
        uint32_t now_ms = millis();
        uint32_t window_ms = now_ms - metrics_window_start_ms;
        if (window_ms == 0) window_ms = 1;

        uint32_t throughput_pps_x100 = ((uint64_t)metrics_window_delivered * 100000ULL) / window_ms;
        uint32_t throughput_bps = ((uint64_t)metrics_window_onair_bytes * 8000ULL) / window_ms;

        uint32_t pdr_x100 = 0;
        uint32_t loss_x100 = 0;
        if (metrics_total_expected > 0)
        {
            pdr_x100 = ((uint64_t)metrics_total_delivered * 10000ULL) / metrics_total_expected;
            loss_x100 = ((uint64_t)metrics_total_lost * 10000ULL) / metrics_total_expected;
        }

        uint32_t hop_avg_x100 = 0;
        if (metrics_hop_samples > 0)
        {
            hop_avg_x100 = ((uint64_t)metrics_hop_sum * 100ULL) / metrics_hop_samples;
        }

        uint32_t latency_avg_ms = 0;
        if (metrics_latency_samples > 0)
        {
            latency_avg_ms = (uint32_t)(metrics_latency_sum_ms / (int64_t)metrics_latency_samples);
        }

        if (throughput_pps_x100 > 0xFFFF) throughput_pps_x100 = 0xFFFF;
        if (pdr_x100 > 0xFFFF) pdr_x100 = 0xFFFF;
        if (loss_x100 > 0xFFFF) loss_x100 = 0xFFFF;
        if (hop_avg_x100 > 0xFFFF) hop_avg_x100 = 0xFFFF;
        if (latency_avg_ms > 0xFFFF) latency_avg_ms = 0xFFFF;

        uplink_buf[idx++] = BRIDGE_METRICS_TAG;
        uplink_buf[idx++] = BRIDGE_METRICS_VER;
        append_u32_be(uplink_buf, idx, window_ms);
        append_u16_be(uplink_buf, idx, (uint16_t)throughput_pps_x100);
        append_u32_be(uplink_buf, idx, throughput_bps);
        append_u16_be(uplink_buf, idx, (uint16_t)pdr_x100);
        append_u16_be(uplink_buf, idx, (uint16_t)loss_x100);
        append_u32_be(uplink_buf, idx, metrics_total_delivered);
        append_u32_be(uplink_buf, idx, metrics_total_expected);
        append_u32_be(uplink_buf, idx, metrics_total_lost);
        append_u16_be(uplink_buf, idx, (uint16_t)hop_avg_x100);
        append_u16_be(uplink_buf, idx, (uint16_t)latency_avg_ms);
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
