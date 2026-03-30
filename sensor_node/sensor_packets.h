// ============================================================================
// SENSOR PACKETS -- Packet handling for sensor nodes
// CSC2106 Group 33 -- LoRa Mesh Network
//
// Sensor-specific packet receive/process, forwarding, and send-with-ACK.
// Uses shared dedup, send_ack, wait_for_ack from mesh_common.h.
// ============================================================================

#ifndef SENSOR_PACKETS_H
#define SENSOR_PACKETS_H

#include <Arduino.h>
#include "../shared/mesh_protocol.h"
#include "../shared/mesh_common.h"
#include "sensor_routing.h"

// ============================================================================
// SEND WITH ACK + RETRY
// Returns true if ACK received within ACK_TIMEOUT_MS on any attempt.
// Returns false if all MAX_RETRIES attempts fail.
//
// During the ACK wait, we use receive_and_process() to handle incoming packets.
// This lets us forward other nodes' data even while waiting for our own ACK.
// ============================================================================

inline bool send_with_ack(uint8_t *packet, uint8_t total_len) {
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
        // NOTE: Do NOT clear rxFlag here -- if an ACK arrived during TX,
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

// ============================================================================
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
// ============================================================================

inline void forward_packet(uint8_t *buf, int len) {
    // Save outer ACK state -- this function uses the same globals for its own
    // ACK wait. If we're called from inside wait_for_ack() (nested forwarding),
    // we must restore these afterwards so the caller's send_with_ack() retry
    // loop isn't corrupted.
    bool     saved_waiting  = waiting_for_ack;
    bool     saved_ack_rcvd = ack_received;
    uint8_t  saved_ack_seq  = ack_expected_seq;

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
        waiting_for_ack  = saved_waiting;
        ack_received     = saved_ack_rcvd;
        ack_expected_seq = saved_ack_seq;
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

    // Restore outer ACK state so the caller's send_with_ack() isn't corrupted
    waiting_for_ack  = saved_waiting;
    ack_received     = saved_ack_rcvd;
    ack_expected_seq = saved_ack_seq;

    // Return to receive mode
    radio.startReceive();
}

// ============================================================================
// RECEIVE AND PROCESS -- NON-BLOCKING PACKET HANDLER
//
// Called every loop() iteration. Reads one packet if available and handles:
//   - BEACON: from lower-rank node -> update candidate table
//   - DATA:   from another node -> validate, ACK, forward to parent
//   - ACK:    addressed to us -> signal send_with_ack() if waiting
//
// This is the core function that enables mesh relay capability.
// ============================================================================

inline void receive_and_process() {
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
    uint8_t flags    = buf[0];
    uint8_t src_id   = buf[1];
    uint8_t dst_id   = buf[2];
    uint8_t prev_hop = buf[3];
    uint8_t ttl      = buf[4];
    uint8_t seq      = buf[5];
    uint8_t rank     = buf[6];
    uint8_t pay_len  = buf[7];

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
        //   1. Lower rank (always -- closer to edge), OR
        //   2. Same rank with a valid parent, when we have no parent
        //      (allows sensor-to-sensor relay without creating loops), OR
        //   3. Current parent (always -- so we learn rank/health changes)
        bool is_current_parent = has_valid_parent() && (src_id == get_parent_id());
        bool accept = (rank < my_rank) ||
                      (rank == my_rank && beacon_health > 0 && !has_valid_parent()) ||
                      is_current_parent;

        if (accept) {
            // Validate CRC before processing
            uint16_t received_crc = ((uint16_t)buf[8] << 8) | buf[9];
            uint16_t computed_crc = crc16_ccitt(buf, 8);
            if (received_crc == computed_crc) {
                process_beacon(buf, len, rssi);
            }
        } else {
            // Log that we heard the beacon but filtered it -- helps debug connectivity
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

    // ---- DATA -- only process DATA packets beyond this point ----

    // --- Validation 3: Destination check (prevents cross-path forwarding) ---
    if (dst_id != NODE_ID) {
        return;  // Not addressed to us -- ignore
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

    // --- Validate forwarding is possible BEFORE ACKing ---
    // If we ACK first and then can't forward, the sender thinks
    // delivery succeeded and never retries -- silent data loss.
    if (!has_valid_parent()) {
        Serial.print("DROP | uid=");
        Serial.print(src_id, HEX);
        Serial.print(seq, HEX);
        Serial.println(" | reason=no_parent (NOT ACKing)");
        return;
    }

    if (get_parent_id() == prev_hop) {
        Serial.print("DROP | uid=");
        Serial.print(src_id, HEX);
        Serial.print(seq, HEX);
        Serial.println(" | reason=loop_detected");
        return;
    }

    // --- ACK the sender -- we have a valid forwarding path ---
    if (flags & PKT_FLAG_ACK_REQ) {
        send_ack(prev_hop, seq);
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

#endif // SENSOR_PACKETS_H
