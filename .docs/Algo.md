# Mesh Network Algorithm Analysis
**CSC2106 Group 33 | LoRa Mesh for Deep Tunnel Infrastructure**

---

## Architecture Overview

The system is a custom 2-tier LoRa mesh with a flat routing hierarchy:

```
Sensor (rank=2) → Relay (rank=1) → Edge (rank=0) → LoRaWAN → TTN
```

All nodes share the same LoRa parameters (923 MHz AS923, SF7, 125 kHz BW, CR 4/5, sync word `0x12`) defined in `mesh_protocol.h`. Every packet begins with a common 10-byte `MeshHeader`.

---

## 1. Node Discovery — Beacon-Based Parent Announcement

**Where:** `edge_node.ino:broadcast_beacon_if_due()`, `relay_node.ino:broadcast_beacon()`

All infrastructure nodes (edge and relay) **proactively broadcast beacons every 10 seconds** (`BEACON_INTERVAL_MS = 10000ms`). This is not a standard protocol like RPL or OLSR — it is a custom, stateless neighbor announcement. Sensors only listen; they never broadcast beacons.

Each beacon is a full `MeshHeader` (10 bytes) followed by a 4-byte `BeaconPayload`:

| Byte | Field | Meaning |
|------|-------|---------|
| 0 | `schema_version` | `0x01` |
| 1 | `queue_pct` | TX queue occupancy 0–100% |
| 2 | `link_quality` | RSSI-derived uplink quality to this node's own parent (0–100) |
| 3 | `parent_health` | ACK success rate to parent (0–100) |

The `rank` field in the `MeshHeader` tells receivers how far the beaconing node is from the TTN sink (edge = 0, relay = 1). Sensors use this to make routing decisions.

### Beacon Phase Offset

To prevent all nodes from beaconing simultaneously (and colliding on the air), each node applies a staggered boot delay and timing offset:

```cpp
#define TX_PHASE_OFFSET_MS  ((NODE_ID % 5) * 2000UL)
// Node 0x01 → 2000ms, Node 0x02 → 4000ms, Node 0x03 → 6000ms, etc.
```

This spreads beacons across a 10-second window. It is a simple deterministic TDMA-style stagger, not a true CSMA/CA mechanism — there is no channel sensing before transmission.

---

## 2. Parent Selection — Weighted Multi-Criteria Scoring

**Where:** `sensor_node.ino:score_parent()`, `relay_node.ino:score_parent()`

When a node receives a beacon, it scores the sender as a potential parent using:

```
score = (60 × rank_score / 100)
      + (25 × rssi_score / 100)
      + (15 × queue_score / 100)
```

| Component | Weight | How it is computed |
|-----------|--------|--------------------|
| `rank_score` | 60% | `rank=0` → 100 pts, `rank=1` → 85 pts, `rank=2` → 70 pts, ... formula: `100 - (rank × 15)`, floored at 0 |
| `rssi_score` | 25% | RSSI clamped to [-120, -60] dBm, linearly mapped to [0, 100] |
| `queue_score` | 15% | `100 - queue_pct` — lower queue occupancy is better |

**Candidate table:** Each sensor and relay tracks up to `MAX_CANDIDATES = 4` potential parents. On every beacon received, the table is updated (or a slot is evicted if full).

**Hysteresis:** A node only switches parents if the new candidate's score exceeds the current parent's score by at least `PARENT_SWITCH_HYSTERESIS = 8` points. This prevents "flapping" between two similarly-scored candidates.

**Parent timeout:** If no beacon is heard from the current parent within `PARENT_TIMEOUT_MS = 35000ms` (~3.5 missed beacon intervals), the parent is declared dead, its candidate entry is invalidated, and re-selection begins immediately from remaining valid candidates. This is the self-healing mechanism.

---

## 3. Data Transmission — Stop-and-Wait ARQ

**Where:** `sensor_node.ino:send_with_ack()`, `sensor_node.ino:wait_for_ack()`

Sensor nodes use a **stop-and-wait ARQ (Automatic Repeat reQuest)** scheme for reliable delivery to the next hop:

1. Transmit packet with `PKT_FLAG_ACK_REQ` set in the flags byte
2. Switch radio to RX mode, wait up to `ACK_TIMEOUT_MS = 600ms` for an ACK
3. If no ACK received, retry — up to `MAX_RETRIES = 3` attempts
4. Each retry adds a back-off delay: `100ms × attempt_number`
5. If all retries fail, invalidate the current parent and trigger re-selection

ACKs are **hop-by-hop** (point-to-point between adjacent nodes), not end-to-end. The relay ACKs the sensor; the edge ACKs whoever sent the packet to it (`prev_hop`). There is no end-to-end acknowledgement from TTN back to the sensor.

**TX jitter:** Before every transmission the sensor applies a random delay of 0–2000ms (`random(0, 2000)`) to reduce the probability of two sensors transmitting simultaneously after a shared event (e.g., power-on).

---

## 4. Packet Forwarding — Payload-Agnostic Store-and-Forward

**Where:** `relay_node.ino:forward_packet()`

The relay's core function is **payload-agnostic forwarding** — the central academic claim of the project. The relay:

1. Reads only the 10-byte `MeshHeader` (bytes 0–9)
2. Mutates exactly four header fields it owns:
   - Sets `prev_hop = NODE_ID` (own ID)
   - Sets `PKT_FLAG_FWD` bit in flags
   - Clears `PKT_FLAG_ACK_REQ` (edge does not need to ACK the relay)
   - Decrements `ttl` by 1
3. Recomputes CRC16-CCITT over the modified bytes 0–7
4. Re-transmits the full packet (header + payload) **without ever reading or modifying bytes 10 onward**

```
Relay rule: bytes MESH_HEADER_SIZE (10) onward are NEVER inspected.
They are copied as an opaque block.
```

This design means adding a new sensor type (e.g., CO₂ sensor with a different payload format) requires **no relay firmware update** — the relay will forward it correctly regardless of payload content.

---

## 5. Packet Validation Pipeline

**Where:** `relay_node.ino:receive_and_process()`, `edge_node.ino:receive_mesh_packets()`

Both relay and edge run every incoming packet through an identical 5-stage validation pipeline before processing:

| Stage | Check | Action on failure |
|-------|-------|-------------------|
| 1 | Minimum length ≥ 10 bytes | Drop, log `too_short` |
| 2 | `src_id ≠ own NODE_ID` | Drop silently (own TX echo) |
| 3 | Deduplication check | Drop, log `duplicate` |
| 4 | CRC16-CCITT over bytes 0–7 | Drop, log `crc_fail` |
| 5 | `ttl > 0` | Drop, log `ttl_drop` |

Only packets passing all five stages are ACKed and processed/forwarded.

---

## 6. Duplicate Suppression — Time-Windowed Circular Buffer

**Where:** `relay_node.ino:is_duplicate()` / `record_dedup()`, `edge_node.ino:is_duplicate()` / `mark_seen()`

Each relay and edge node maintains a dedup table of `DEDUP_TABLE_SIZE = 16` entries. Each entry stores `(src_id, seq_num, timestamp)`. A packet is considered a duplicate if `(src_id, seq_num)` was seen within the last `DEDUP_WINDOW_MS = 30000ms`.

The relay uses a **circular buffer** with a head pointer that advances modulo 16 on every new entry. The edge uses a simple linear scan with timestamp expiry. Both achieve the same effect: suppress forwarded or re-stored duplicates within a 30-second window.

---

## 7. Integrity Check — CRC16-CCITT

**Where:** `mesh_protocol.h:crc16_ccitt()`, `set_mesh_crc()`, `validate_mesh_crc()`

Every packet header is protected by **CRC16-CCITT** (polynomial `0x1021`, initial value `0xFFFF`), computed over the first 8 bytes of the `MeshHeader` and stored big-endian in bytes 8–9. This detects single and burst bit errors introduced by the LoRa channel.

This is an **integrity check only**, not a cryptographic MAC. It cannot detect deliberate tampering. Security at the network layer is provided by LoRaWAN's AES-128 encryption on the edge-to-TTN uplink.

---

## 8. Data Aggregation at Edge — Batch LoRaWAN Uplink

**Where:** `edge_node.ino:handle_data()`, `edge_node.ino:send_lorawan_uplink()`

The edge node does not forward each mesh packet individually to TTN — it **batches** sensor readings into a `BridgeAggV1` structure and sends a single LoRaWAN uplink. This is necessary because LoRaWAN (TTN fair-use policy) limits each device to roughly 30 seconds of airtime per day, so sending one uplink per sensor reading would exhaust the budget quickly.

The edge flushes its aggregation buffer to TTN when either condition is met:
- Buffer contains `MAX_AGG_RECORDS = 7` readings, **or**
- `AGG_FLUSH_TIMEOUT_MS = 240000ms` (4 minutes) have elapsed since the last uplink

The `BridgeAggV1` format sent over LoRaWAN:
- **3-byte header:** schema version, bridge (edge) ID, record count
- **14 bytes per record:** src node ID, sequence number, sensor type hint, hop estimate, edge uptime, opaque payload length, raw 7-byte sensor payload (copied as-is — payload-agnostic even at the aggregation layer)

---

## 9. Radio Time-Sharing at Edge

**Where:** `edge_node.ino:loop()`, `edge_node.ino:switch_to_mesh_rx()`

The edge node has a **single SX1262 radio** that must serve two roles: raw LoRa mesh receiver and LoRaWAN transmitter. These use different radio configurations (LoRaWAN uses its own frequency plan, preamble, and sync word managed by RadioLib).

A cooperative state machine handles this:

```
Normal state (MESH_LISTEN):
    radio listens for mesh packets on 923 MHz, SF7, sync 0x12

Flush trigger (buffer full OR 240s elapsed):
    MESH_LISTEN → LORAWAN_TXRX
        reconfigure radio for LoRaWAN
        send BridgeAggV1 uplink (blocking, ~5s for RX1/RX2 windows)
        reconfigure radio back to mesh parameters
    LORAWAN_TXRX → MESH_LISTEN
```

During the LoRaWAN window (~5 seconds), the edge is **deaf to mesh packets**. At 30–120 second sensor TX intervals this is acceptable — no sensor packet will be missed.

---

## 10. Edge Failover — Relay Re-Parenting

**Where:** `relay_node.ino:check_parent_timeout()`, `relay_node.ino:update_parent_selection()`

The system supports a standby Edge 2 (`NODE_ID = 0x05`) for redundancy. Both Edge 1 and Edge 2 broadcast beacons continuously. The relay maintains both in its candidate table and scores them identically by formula.

Under normal operation the relay selects whichever edge has the better score (typically Edge 1 as primary). If Edge 1 goes offline:

1. Relay stops hearing Edge 1's beacons
2. After `PARENT_TIMEOUT_MS = 35s`, relay invalidates Edge 1 and calls `update_parent_selection()`
3. Edge 2 is already in the candidate table with a valid recent beacon — relay selects it immediately
4. All subsequent sensor packets are forwarded to Edge 2
5. TTN uplinks now originate from Edge 2's DevEUI

When Edge 1 recovers, it starts beaconing again. The relay will **not** switch back immediately — hysteresis (`PARENT_SWITCH_HYSTERESIS = 8`) requires Edge 1 to score at least 8 points higher than the current parent (Edge 2) before a switch occurs. Since both are rank=0 with similar RSSI in a lab environment, the relay typically stays on Edge 2 until Edge 2 itself times out.

---

## Key Parameters Reference

| Parameter | Value | Notes |
|-----------|-------|-------|
| LoRa frequency | 923.0 MHz | AS923, Singapore |
| Spreading factor | SF7 (fixed) | Fast airtime, sufficient lab range |
| Bandwidth | 125 kHz | Standard |
| Coding rate | 4/5 | |
| TX power | 17 dBm | |
| Sync word | `0x12` | Private network |
| Beacon interval | 10s | Edge and relay |
| Beacon phase offset | `(NODE_ID % 5) × 2000ms` | Prevents simultaneous beacons |
| TX jitter (sensor) | 0–2000ms random | Before each sensor transmission |
| ACK timeout | 600ms | How long sensor waits for hop ACK |
| Max retries | 3 | Per packet, with back-off |
| TTL default | 10 | Max hops before drop |
| Dedup window | 30s | Duplicate suppression window |
| Dedup table size | 16 entries | Per node, circular buffer |
| Parent switch hysteresis | 8 points | Min score improvement to switch |
| Parent timeout | 35s | ~3.5 missed beacon intervals |
| Score weights | rank=60%, rssi=25%, queue=15% | Sum to 100% |
| Aggregation flush | 240s or 7 records | Edge → TTN uplink trigger |
| LoRaWAN window | ~5s max | Edge deaf to mesh during uplink |

---

## Algorithm Summary Table

| Algorithm / Mechanism | Role in the system |
|-----------------------|--------------------|
| Beacon broadcast (proactive, periodic) | Node discovery — infrastructure announces itself every 10s |
| Weighted scoring (rank + RSSI + queue) | Parent selection — picks best next hop toward the edge |
| Phase offset staggering | Collision avoidance for simultaneous beacons at boot |
| Stop-and-Wait ARQ | Reliable hop-by-hop delivery with retry and back-off |
| TX jitter (0–2000ms random) | Reduces simultaneous-TX collision probability |
| Payload-agnostic forwarding | Relay copies payload bytes blindly — core research claim |
| CRC16-CCITT (polynomial 0x1021) | Header integrity check over the air |
| Time-windowed dedup table | Drops retransmitted duplicates within 30s window |
| Batch aggregation + flush trigger | Efficient LoRaWAN uplink within TTN airtime budget |
| Cooperative radio time-sharing | One radio serves both mesh RX and LoRaWAN TX at edge |
| Parent timeout + re-parenting | Self-healing — automatic failover on node loss |
| Hysteresis on parent switch | Prevents flapping between similarly-scored candidates |
