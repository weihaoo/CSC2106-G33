// ============================================================================
// RELAY PACKETS — Packet handling for relay nodes
// CSC2106 Group 33 — LoRa Mesh Network
//
// Relay-specific packet receive/process, forwarding with DAG parent selection.
// Uses shared dedup, send_ack, wait_for_ack, score_parent from mesh_common.h.
// Uses relay_routing.h for DAG parent set management.
// ============================================================================

#ifndef RELAY_PACKETS_H
#define RELAY_PACKETS_H

#include <Arduino.h>
#include "../shared/mesh_protocol.h"
#include "../shared/mesh_common.h"
#include "relay_routing.h"

// ============================================================================
// FORWARD PACKET (DAG — Multiple Parent Selection, with ACK + Retry)
//
// Keeps ACK_REQ set so the parent (edge) acknowledges receipt.
// Retries up to MAX_RETRIES times if no ACK is received.
// This ensures reliable delivery across the relay→edge hop.
// ============================================================================

inline void forward_packet(uint8_t *buf, int len) {
    log_separator_packet();

    // Save outer ACK state (nested forwarding during wait_for_ack)
    bool saved_waiting = waiting_for_ack;
    bool saved_ack_rcvd = ack_received;
    uint8_t saved_ack_seq = ack_expected_seq;

    // DAG: Select parent from active parent set using weighted random
    uint8_t target = select_parent_for_packet();

    if (target == 0xFF) {
        LOG_DROP("No active parents in DAG set");
        waiting_for_ack = saved_waiting;
        ack_received = saved_ack_rcvd;
        ack_expected_seq = saved_ack_seq;
        return;
    }

    uint8_t src_id = buf[1];
    uint8_t seq = buf[5];
    uint8_t ttl = buf[4];
    uint8_t payload_len = buf[7];

    // Log the forwarding action
    char msg[80];
    sprintf(msg, "Forwarding packet #%d to %s", seq, node_name(target));
    LOG_FWD(msg);

    sprintf(msg, "Source: %s | TTL: %d→%d | Payload: %d bytes",
            node_name(src_id), ttl, ttl-1, payload_len);
    log_detail(msg);

    // Show DAG selection info
    sprintf(msg, "DAG selection: %d active parents, chose %s (weighted random)",
            active_parent_count, node_name(target));
    LOG_DAG(msg);

    // Update counters
    if (pending_forwards < MAX_PENDING_FORWARDS) pending_forwards++;

    // Modify header — keep ACK_REQ so parent acknowledges receipt
    buf[0] = buf[0] | PKT_FLAG_FWD;  // set FWD, keep ACK_REQ
    buf[2] = target;
    buf[3] = NODE_ID;
    buf[4] = ttl - 1;

    // Recalculate CRC
    uint16_t crc = crc16_ccitt(buf, 8);
    buf[8] = (crc >> 8) & 0xFF;
    buf[9] = crc & 0xFF;

    // Validate total length
    int total_len = MESH_HEADER_SIZE + payload_len;
    if (total_len > len) {
        LOG_ERROR("Payload length exceeds packet size");
        if (pending_forwards > 0) pending_forwards--;
        waiting_for_ack = saved_waiting;
        ack_received = saved_ack_rcvd;
        ack_expected_seq = saved_ack_seq;
        return;
    }

    // Send with ACK + retry for reliable forwarding
    bool success = false;
    for (uint8_t attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        sprintf(msg, "FWD attempt %d/%d → %s", attempt, MAX_RETRIES, node_name(target));
        LOG_FWD(msg);

        int state = lbt_transmit(radio, buf, total_len);
        if (state != RADIOLIB_ERR_NONE) {
            LOG_ERROR("Forward TX failed");
            delay(200);
            continue;
        }

        // Wait for ACK from parent
        ack_received = false;
        ack_expected_seq = seq;
        waiting_for_ack = true;

        radio.startReceive();

        if (wait_for_ack(ACK_TIMEOUT_MS)) {
            waiting_for_ack = false;
            success = true;
            sprintf(msg, "Forward ACK received for #%d", seq);
            LOG_OK(msg);
            break;
        }

        waiting_for_ack = false;
        sprintf(msg, "No ACK for forward #%d, retrying...", seq);
        LOG_WARN(msg);
        delay(100 * attempt);  // back-off
    }

    if (!success) {
        sprintf(msg, "All forward retries failed for #%d from %s", seq, node_name(src_id));
        LOG_ERROR(msg);

        // Strikes system: penalize the target parent for consecutive forward failures.
        // After MAX_PARENT_STRIKES failures, invalidate so we stop forwarding into a black hole.
        for (int i = 0; i < MAX_CANDIDATES; i++) {
            if (candidates[i].valid && candidates[i].node_id == target) {
                candidates[i].fail_count++;
                sprintf(msg, "Strike %d/%d on %s", candidates[i].fail_count, MAX_PARENT_STRIKES, node_name(target));
                LOG_WARN(msg);

                if (candidates[i].fail_count >= MAX_PARENT_STRIKES) {
                    sprintf(msg, "Max strikes — invalidating %s", node_name(target));
                    LOG_WARN(msg);
                    candidates[i].valid = false;
                    update_parent_set();
                    broadcast_beacon();
                }
                break;
            }
        }
    }

    if (pending_forwards > 0) pending_forwards--;

    // Restore outer ACK state
    waiting_for_ack = saved_waiting;
    ack_received = saved_ack_rcvd;
    ack_expected_seq = saved_ack_seq;

    radio.startReceive();

    log_separator_packet();
}

// ============================================================================
// RECEIVE AND PROCESS — NON-BLOCKING PACKET HANDLER
//
// Called every loop() iteration. Reads one packet if available and handles:
//   - BEACON: from lower-rank node -> update candidate table via process_beacon
//   - DATA:   from a child node -> validate, ACK, forward to parent
//   - ACK:    for a packet we forwarded -> signal wait_for_ack()
// ============================================================================

inline void receive_and_process() {
    if (!rxFlag) return;
    rxFlag = false;

    int len = radio.getPacketLength();
    uint8_t buf[64];
    int state = radio.readData(buf, sizeof(buf));
    int rssi = (int)radio.getRSSI();

    rxFlag = false;
    radio.startReceive();

    if (state != RADIOLIB_ERR_NONE) {
        LOG_DROP("Radio read error");
        return;
    }

    // Validation 1: Minimum length
    if (len < MESH_HEADER_SIZE) {
        LOG_DROP("Packet too short");
        return;
    }

    // Extract header fields
    uint8_t src_id  = buf[1];
    uint8_t dst_id  = buf[2];
    uint8_t flags   = buf[0];
    uint8_t seq     = buf[5];
    uint8_t ttl     = buf[4];
    uint8_t rank    = buf[6];

    // Validation 2: Ignore own packets
    if (src_id == NODE_ID) {
        return;
    }

    // Check beacon
    if (GET_PKT_TYPE(flags) == PKT_TYPE_BEACON) {
        if (rank < my_rank) {
            // Validate CRC before processing
            uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
            uint16_t computed_crc = crc16_ccitt(buf, 8);
            if (received_crc == computed_crc) {
                process_beacon(buf, len, rssi);
            } else {
                LOG_DROP("Beacon CRC mismatch");
            }
        }
        return;
    }

    // Handle ACK packets
    if (GET_PKT_TYPE(flags) == PKT_TYPE_ACK) {
        // Validate CRC
        uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
        uint16_t computed_crc = crc16_ccitt(buf, 8);
        if (received_crc != computed_crc) {
            LOG_DROP("ACK CRC mismatch");
            return;
        }
        // Check if this ACK is for a packet we forwarded
        if (dst_id == NODE_ID && waiting_for_ack && seq == ack_expected_seq) {
            char msg[64];
            sprintf(msg, "Received ACK for fwd #%d from %s", seq, node_name(src_id));
            LOG_ACK(msg);
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
        return;
    }

    // Validation 3: Destination check
    if (dst_id != NODE_ID) {
        return;
    }

    // Validation 4: Dedup check
    if (is_duplicate(src_id, seq)) {
        char msg[64];
        sprintf(msg, "Duplicate packet #%d from %s", seq, node_name(src_id));
        LOG_DROP(msg);

        // Re-ACK: the sender retried because our earlier ACK was lost
        if (flags & PKT_FLAG_ACK_REQ) {
            send_ack(buf[3], seq);  // buf[3] = prev_hop
        }
        return;
    }

    // Validation 5: CRC check
    uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
    uint16_t computed_crc = crc16_ccitt(buf, 8);
    if (received_crc != computed_crc) {
        char msg[64];
        sprintf(msg, "CRC fail on packet #%d from %s", seq, node_name(src_id));
        LOG_DROP(msg);
        return;
    }

    // Validation 6: TTL check
    if (ttl == 0) {
        char msg[64];
        sprintf(msg, "TTL expired on packet #%d from %s", seq, node_name(src_id));
        LOG_DROP(msg);
        return;
    }

    // Record in dedup table
    record_dedup(src_id, seq);

    // Validate forwarding is possible BEFORE ACKing the sender.
    // If we ACK first and then fail to forward, the sender thinks
    // delivery succeeded and never retries — silent data loss.
    uint8_t fwd_target = select_parent_for_packet();
    if (fwd_target == 0xFF) {
        LOG_DROP("No active parents in DAG set — NOT ACKing sender");
        return;  // sender will retry or find another route
    }
    if (fwd_target == buf[3]) {  // buf[3] = prev_hop
        char msg[64];
        sprintf(msg, "Loop detected: would forward #%d back to %s", seq, node_name(fwd_target));
        LOG_DROP(msg);
        return;
    }

    // ACK the sender — we have a valid forwarding path
    if (flags & PKT_FLAG_ACK_REQ) {
        send_ack(buf[3], seq);
    }

    // Recursion guard: if we're already inside forward_packet->wait_for_ack,
    // don't nest another forward (would cause unbounded stack growth).
    static bool in_forward = false;
    if (in_forward) {
        LOG_DROP("Suppressed nested forward (recursion guard)");
        return;
    }

    // Forward the packet (DAG selection with ACK + retry)
    in_forward = true;
    forward_packet(buf, len);
    in_forward = false;
}

#endif // RELAY_PACKETS_H
