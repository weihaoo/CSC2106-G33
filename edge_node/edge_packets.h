// ════════════════════════════════════════════════════════════════════════════
// EDGE NODE — MESH PACKET RECEPTION & HANDLING
// CSC2106 Group 33
//
// Contains:
//   - receive_mesh_packets() — DIO1 interrupt-driven packet reception
//   - handle_beacon()        — log beacons (edge doesn't select parents)
//   - handle_data()          — log, ACK, aggregate sensor data
//   - send_ack()             — build and transmit ACK via LBT
//   - is_duplicate()         — deduplication check
//   - mark_seen()            — record packet in dedup table
//   - broadcast_beacon_if_due() — periodic rank-0 beacon broadcast
//
// Included by edge_node.ino AFTER all globals are declared.
// ════════════════════════════════════════════════════════════════════════════

#ifndef EDGE_PACKETS_H
#define EDGE_PACKETS_H

// ════════════════════════════════════════════════════════════════════════════
// EXTERN DECLARATIONS (defined in edge_node.ino)
// ════════════════════════════════════════════════════════════════════════════

extern SX1262 radio;
extern volatile bool rxFlag;

extern AggRecord agg_buffer[];
extern uint8_t agg_count;
extern uint32_t boot_time;

extern DedupEntry dedup_table[];
extern uint8_t dedup_index;

extern uint32_t last_beacon_time;
extern uint32_t packets_received;
extern uint32_t packets_dropped;

extern bool lorawan_joined;
extern bool edge_ntp_synced;   // true if edge NTP sync succeeded at boot

// ════════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

inline void handle_beacon(MeshHeader *hdr, int rssi, float snr);
inline void handle_data(MeshHeader *hdr, uint8_t *payload, int rssi, float snr);
inline void send_ack(uint8_t to_node, uint8_t seq);
inline bool is_duplicate(uint8_t src_id, uint8_t seq_num);
inline void mark_seen(uint8_t src_id, uint8_t seq_num);

// ════════════════════════════════════════════════════════════════════════════
// MESH PACKET RECEPTION (PHASE 2)
// ════════════════════════════════════════════════════════════════════════════

inline void receive_mesh_packets()
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

        // Re-ACK: the sender retried because our earlier ACK was lost
        if (IS_ACK_REQUESTED(hdr->flags))
        {
            send_ack(hdr->prev_hop, hdr->seq_num);
        }
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

inline void handle_beacon(MeshHeader *hdr, int rssi, float snr)
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

// ════════════════════════════════════════════════════════════════════════════
// BEACON BROADCAST (rank-0 beacon)
// ════════════════════════════════════════════════════════════════════════════

inline void broadcast_beacon_if_due()
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
    radio.startReceive(); // Return to RX mode
}

// ════════════════════════════════════════════════════════════════════════════
// DATA PACKET HANDLING (PHASE 3)
// ════════════════════════════════════════════════════════════════════════════

inline void handle_data(MeshHeader *hdr, uint8_t *payload, int rssi, float snr)
{
    uint8_t hop_count = DEFAULT_TTL - hdr->ttl; // Estimate hops from remaining TTL

    // Capture edge receive timestamp as early as possible for best accuracy
    uint32_t recv_time_s = get_ntp_epoch_s();

    Serial.println(F("────────────────────────────────────────────────────"));
    Serial.print(F("RX_MESH | src=0x"));
    Serial.print(hdr->src_id, HEX);
    Serial.print(F(" | seq="));
    Serial.print(hdr->seq_num);
    Serial.print(F(" | hops="));
    Serial.print(hop_count);
    Serial.print(F(" | rssi="));
    Serial.println(rssi);

    // ──────────────────────────────────────────────────────────────────
    // ONE-WAY LATENCY CALCULATION
    // Reads send_timestamp_s from bytes [7-10] of the sensor payload.
    // Only shown when both edge and sensor had valid NTP sync at TX/RX time.
    // ──────────────────────────────────────────────────────────────────

    if (edge_ntp_synced && hdr->payload_len >= 11) {
        // Decode send_timestamp_s (big-endian uint32 at payload bytes 7-10)
        uint32_t send_ts = ((uint32_t)payload[7] << 24)
                         | ((uint32_t)payload[8] << 16)
                         | ((uint32_t)payload[9] <<  8)
                         |  (uint32_t)payload[10];

        if (send_ts != 0) {
            // One-way latency in seconds (NTP resolution is 1s; use ms anchor for sub-second)
            // For sub-second display we use the ms-level edge receive time vs coarse send time
            uint32_t edge_recv_ms_within_sec = (millis() - _sync_millis) % 1000;

            // Coarse one-way latency in ms:
            //   (recv whole-seconds - send whole-seconds) * 1000 + recv fractional ms
            int32_t latency_ms = (int32_t)(recv_time_s - send_ts) * 1000
                               + (int32_t)edge_recv_ms_within_sec;

            Serial.print(F("LATENCY | src=0x"));
            Serial.print(hdr->src_id, HEX);
            Serial.print(F(" | send_ts="));
            Serial.print(send_ts);
            Serial.print(F(" | recv_ts="));
            Serial.print(recv_time_s);
            Serial.print(F(" | one-way ~"));
            if (latency_ms >= 0 && latency_ms < 60000) {
                Serial.print(latency_ms);
                Serial.println(F(" ms"));
            } else {
                // Negative or unreasonably large = clock skew, show raw delta
                Serial.print((int32_t)(recv_time_s - send_ts));
                Serial.println(F(" s (coarse, NTP skew possible)"));
            }
        } else {
            Serial.println(F("LATENCY | sensor NTP not synced — no timestamp in packet"));
        }
    }
    // (If edge_ntp_synced is false, silently skip latency display)

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

inline void send_ack(uint8_t to_node, uint8_t seq)
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

    radio.startReceive(); // Return to RX mode
}

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION HELPERS
// ════════════════════════════════════════════════════════════════════════════

inline bool is_duplicate(uint8_t src_id, uint8_t seq_num)
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

inline void mark_seen(uint8_t src_id, uint8_t seq_num)
{
    dedup_table[dedup_index].src_id = src_id;
    dedup_table[dedup_index].seq_num = seq_num;
    dedup_table[dedup_index].timestamp = millis();
    dedup_table[dedup_index].valid = true;

    dedup_index = (dedup_index + 1) % DEDUP_TABLE_SIZE;
}

#endif // EDGE_PACKETS_H
