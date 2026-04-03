#ifndef EDGE_METRICS_H
#define EDGE_METRICS_H

#include <Arduino.h>
#include "../shared/mesh_protocol.h"
#include "config.h"

// Keep source tracking bounded for embedded RAM usage.
#define MAX_METRIC_SOURCES 16

struct SourceMetric {
    uint8_t src_id;
    uint8_t last_seq;
    uint32_t delivered;
    uint32_t expected;
    uint32_t lost;
    bool initialized;
};

extern SourceMetric source_metrics[MAX_METRIC_SOURCES];

extern uint32_t metrics_window_start_ms;
extern uint32_t metrics_window_delivered;
extern uint32_t metrics_window_payload_bytes;
extern uint32_t metrics_window_onair_bytes;

extern uint32_t metrics_total_delivered;
extern uint32_t metrics_total_expected;
extern uint32_t metrics_total_lost;

extern uint32_t metrics_hop_samples;
extern uint32_t metrics_hop_sum;
extern uint8_t metrics_hop_min;
extern uint8_t metrics_hop_max;

extern uint32_t metrics_latency_samples;
extern int32_t metrics_latency_min_ms;
extern int32_t metrics_latency_max_ms;
extern int64_t metrics_latency_sum_ms;

extern uint32_t packets_received;
extern uint32_t packets_dropped;

inline SourceMetric* find_or_create_source_metric(uint8_t src_id)
{
    for (uint8_t i = 0; i < MAX_METRIC_SOURCES; i++) {
        if (source_metrics[i].initialized && source_metrics[i].src_id == src_id) {
            return &source_metrics[i];
        }
    }

    for (uint8_t i = 0; i < MAX_METRIC_SOURCES; i++) {
        if (!source_metrics[i].initialized) {
            source_metrics[i].src_id = src_id;
            source_metrics[i].last_seq = 0;
            source_metrics[i].delivered = 0;
            source_metrics[i].expected = 0;
            source_metrics[i].lost = 0;
            source_metrics[i].initialized = true;
            return &source_metrics[i];
        }
    }

    return nullptr;
}

inline void metrics_init()
{
    memset(source_metrics, 0, sizeof(source_metrics));

    metrics_window_start_ms = millis();
    metrics_window_delivered = 0;
    metrics_window_payload_bytes = 0;
    metrics_window_onair_bytes = 0;

    metrics_total_delivered = 0;
    metrics_total_expected = 0;
    metrics_total_lost = 0;

    metrics_hop_samples = 0;
    metrics_hop_sum = 0;
    metrics_hop_min = 0;
    metrics_hop_max = 0;

    metrics_latency_samples = 0;
    metrics_latency_min_ms = 0;
    metrics_latency_max_ms = 0;
    metrics_latency_sum_ms = 0;
}

inline void metrics_record_latency(int32_t latency_ms)
{
    if (metrics_latency_samples == 0) {
        metrics_latency_min_ms = latency_ms;
        metrics_latency_max_ms = latency_ms;
    } else {
        if (latency_ms < metrics_latency_min_ms) metrics_latency_min_ms = latency_ms;
        if (latency_ms > metrics_latency_max_ms) metrics_latency_max_ms = latency_ms;
    }

    metrics_latency_sum_ms += latency_ms;
    metrics_latency_samples++;
}

inline void metrics_record_delivery(uint8_t src_id, uint8_t seq, uint8_t hop_count, uint8_t payload_len)
{
    SourceMetric* sm = find_or_create_source_metric(src_id);
    if (sm == nullptr) {
        return;
    }

    uint32_t expected_inc = 1;
    uint32_t lost_inc = 0;

    if (sm->delivered == 0) {
        sm->last_seq = seq;
    } else {
        uint8_t delta = (uint8_t)(seq - sm->last_seq);

        // delta in [1..64] is treated as in-order progress (including wrap).
        // 0 or large backward jumps are treated as stream reset/reseed.
        if (delta >= 1 && delta <= 64) {
            expected_inc = delta;
            if (delta > 1) {
                lost_inc = delta - 1;
            }
        } else {
            expected_inc = 1;
            lost_inc = 0;
        }

        sm->last_seq = seq;
    }

    sm->delivered++;
    sm->expected += expected_inc;
    sm->lost += lost_inc;

    metrics_total_delivered++;
    metrics_total_expected += expected_inc;
    metrics_total_lost += lost_inc;

    metrics_window_delivered++;
    metrics_window_payload_bytes += payload_len;
    metrics_window_onair_bytes += (uint32_t)payload_len + MESH_HEADER_SIZE;

    if (metrics_hop_samples == 0) {
        metrics_hop_min = hop_count;
        metrics_hop_max = hop_count;
    } else {
        if (hop_count < metrics_hop_min) metrics_hop_min = hop_count;
        if (hop_count > metrics_hop_max) metrics_hop_max = hop_count;
    }
    metrics_hop_sum += hop_count;
    metrics_hop_samples++;
}

inline void metrics_report_if_due()
{
    uint32_t now = millis();
    uint32_t elapsed_ms = now - metrics_window_start_ms;
    if (elapsed_ms < METRICS_REPORT_INTERVAL_MS || elapsed_ms == 0) {
        return;
    }

    float elapsed_s = elapsed_ms / 1000.0f;

    float throughput_pps = metrics_window_delivered / elapsed_s;
    float throughput_payload_Bps = metrics_window_payload_bytes / elapsed_s;
    float throughput_onair_bps = (metrics_window_onair_bytes * 8.0f) / elapsed_s;

    float pdr_pct = 0.0f;
    float loss_pct = 0.0f;
    if (metrics_total_expected > 0) {
        pdr_pct = (100.0f * metrics_total_delivered) / metrics_total_expected;
        loss_pct = (100.0f * metrics_total_lost) / metrics_total_expected;
    }

    float hop_avg = 0.0f;
    if (metrics_hop_samples > 0) {
        hop_avg = (float)metrics_hop_sum / metrics_hop_samples;
    }

    float latency_avg = 0.0f;
    if (metrics_latency_samples > 0) {
        latency_avg = (float)metrics_latency_sum_ms / metrics_latency_samples;
    }

    Serial.println(F("\n================ EDGE METRICS ================"));
    Serial.print(F("THROUGHPUT | window="));
    Serial.print(elapsed_ms);
    Serial.print(F(" ms | pkt/s="));
    Serial.print(throughput_pps, 2);
    Serial.print(F(" | payload_B/s="));
    Serial.print(throughput_payload_Bps, 2);
    Serial.print(F(" | onair_bps="));
    Serial.println(throughput_onair_bps, 2);

    Serial.print(F("DELIVERY   | delivered="));
    Serial.print(metrics_total_delivered);
    Serial.print(F(" | expected="));
    Serial.print(metrics_total_expected);
    Serial.print(F(" | PDR="));
    Serial.print(pdr_pct, 2);
    Serial.print(F("% | loss="));
    Serial.print(metrics_total_lost);
    Serial.print(F(" ("));
    Serial.print(loss_pct, 2);
    Serial.println(F("%)"));

    Serial.print(F("RADIO DROP | packets_received="));
    Serial.print(packets_received);
    Serial.print(F(" | packets_dropped="));
    Serial.println(packets_dropped);

    if (metrics_hop_samples > 0) {
        Serial.print(F("HOPS       | avg="));
        Serial.print(hop_avg, 2);
        Serial.print(F(" | min="));
        Serial.print(metrics_hop_min);
        Serial.print(F(" | max="));
        Serial.println(metrics_hop_max);
    } else {
        Serial.println(F("HOPS       | no samples yet"));
    }

    if (metrics_latency_samples > 0) {
        Serial.print(F("LATENCY    | avg="));
        Serial.print(latency_avg, 2);
        Serial.print(F(" ms | min="));
        Serial.print(metrics_latency_min_ms);
        Serial.print(F(" ms | max="));
        Serial.print(metrics_latency_max_ms);
        Serial.print(F(" ms | samples="));
        Serial.println(metrics_latency_samples);
    } else {
        Serial.println(F("LATENCY    | no valid timestamp pairs yet"));
    }

    for (uint8_t i = 0; i < MAX_METRIC_SOURCES; i++) {
        if (!source_metrics[i].initialized || source_metrics[i].delivered == 0) {
            continue;
        }

        float src_pdr_pct = 0.0f;
        float src_loss_pct = 0.0f;
        if (source_metrics[i].expected > 0) {
            src_pdr_pct = (100.0f * source_metrics[i].delivered) / source_metrics[i].expected;
            src_loss_pct = (100.0f * source_metrics[i].lost) / source_metrics[i].expected;
        }

        Serial.print(F("SRC 0x"));
        Serial.print(source_metrics[i].src_id, HEX);
        Serial.print(F(" | delivered="));
        Serial.print(source_metrics[i].delivered);
        Serial.print(F(" | expected="));
        Serial.print(source_metrics[i].expected);
        Serial.print(F(" | lost="));
        Serial.print(source_metrics[i].lost);
        Serial.print(F(" | PDR="));
        Serial.print(src_pdr_pct, 2);
        Serial.print(F("% | loss="));
        Serial.print(src_loss_pct, 2);
        Serial.println(F("%"));
    }

    Serial.println(F("==============================================\n"));

    metrics_window_delivered = 0;
    metrics_window_payload_bytes = 0;
    metrics_window_onair_bytes = 0;
    metrics_window_start_ms = now;
}

#endif // EDGE_METRICS_H
