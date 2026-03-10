# Firmware Reference, LoRa Mesh Network

This document describes what each firmware file does, how its internal logic works, and what to change when modifying behaviour.

For the **build guide** (step-by-step bring-up, wiring, TTN setup), see `IMPLEMENTATION_PLAN_SIMPLE.md`.

---

## System Overview

```
[Sensor 1 (0x03)] ─┐
                    ├─► [Relay (0x02)] ─► [Edge 1 (0x01)] ─► LoRaWAN ─► TTN ─► Pico W
[Sensor 2 (0x04)] ─┘         │
                              └─► [Edge 2 (0x06)] ─► LoRaWAN ─► TTN     (failover)
```

All nodes run on **LilyGo T-Beam** boards with an **SX1262** radio at **923 MHz (AS923)**. There is one physical radio per board — the edge node time-shares it between raw LoRa mesh reception and LoRaWAN uplinks.

### Node ID Assignment

| Node | ID | Role |
|------|----|------|
| Edge 1 | `0x01` | Primary mesh sink + LoRaWAN uplink |
| Relay | `0x02` | Payload-agnostic forwarder |
| Sensor 1 | `0x03` | DHT22 temperature/humidity |
| Sensor 2 | `0x04` | DHT22 temperature/humidity |
| Edge 2 | `0x06` | Standby mesh sink + LoRaWAN uplink |

### Rank Hierarchy

Rank determines routing priority. **Lower rank = closer to the internet.**

| Rank | Node type | Meaning |
|------|-----------|---------|
| 0 | Edge | Top of mesh, connected to TTN |
| 1 | Relay | Connected to an edge node |
| 2 | Sensor | Data originator |
| 255 | Any | Orphaned — lost parent, do not route through |

---

## Shared Foundation: `mesh_protocol.h`

This header is copied verbatim into `sensor_node/`, `relay_node/`, and `edge_node/`. It defines everything that must be identical across all nodes. **Never add node-specific values here** — those belong in the individual `.ino` files or `edge_node/config.h`.

### Radio Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Frequency | 923.0 MHz | AS923, Singapore |
| Spreading factor | SF7 | Fixed — fastest airtime for lab distances |
| Bandwidth | 125 kHz | Standard |
| Coding rate | 4/5 | |
| Sync word | `0x12` | Private network (not public LoRaWAN `0x34`) |
| TX power | 17 dBm | |

### Protocol Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `BEACON_INTERVAL_MS` | 10 000 ms | How often relay/edge broadcast beacons |
| `ACK_TIMEOUT_MS` | 600 ms | How long a sender waits for a hop ACK |
| `PARENT_TIMEOUT_MS` | 35 000 ms | Declare parent dead after ~3.5 missed beacons |
| `MAX_RETRIES` | 3 | TX attempts per packet before giving up |
| `DEFAULT_TTL` | 10 | Starting TTL value (max hops before drop) |
| `DEDUP_WINDOW_MS` | 30 000 ms | Ignore duplicate (src, seq) pairs within this window |
| `DEDUP_TABLE_SIZE` | 16 | Circular dedup table slots per node |
| `MAX_CANDIDATES` | 4 | Parent candidates tracked per node |
| `PARENT_SWITCH_HYSTERESIS` | 8 | Minimum score improvement required to switch parent |
| `LBT_MAX_RETRIES` | 3 | CAD attempts before forcing transmission |
| `LBT_BASE_DELAY_MS` | 100 ms | Base back-off delay (doubles each retry) |
| `MAX_AGG_RECORDS` | 7 | Max sensor readings per LoRaWAN uplink |
| `AGG_FLUSH_TIMEOUT_MS` | 240 000 ms | Flush to TTN after 4 minutes regardless |

### Packet Formats

#### MeshHeader — 10 bytes, present on every packet

| Byte(s) | Field | Description |
|---------|-------|-------------|
| 0 | `flags` | Packet type (bits 7–5) + control flags (bits 4–0) |
| 1 | `src_id` | Original creator of the packet — never changed in transit |
| 2 | `dst_id` | Intended next recipient (`0xFF` = broadcast) |
| 3 | `prev_hop` | Node ID of the last node that transmitted this packet |
| 4 | `ttl` | Decremented each hop; packet is dropped when it reaches 0 |
| 5 | `seq_num` | Sender's packet counter, used for deduplication |
| 6 | `rank` | Sender's rank at time of sending |
| 7 | `payload_len` | Byte count of the data following this header |
| 8–9 | `crc16` | CRC16-CCITT over bytes 0–7, stored big-endian |

**Packet type (bits 7–5 of `flags`):**

| Value | Constant | Meaning |
|-------|----------|---------|
| `0x00` | `PKT_TYPE_DATA` | Sensor data packet |
| `0x20` | `PKT_TYPE_BEACON` | Parent discovery broadcast |
| `0x40` | `PKT_TYPE_ACK` | Hop-level acknowledgement |

**Control flags (bits 4–0 of `flags`):**

| Bit | Constant | Set by | Meaning |
|-----|----------|--------|---------|
| 4 | `PKT_FLAG_ACK_REQ` | Sender | Requests a hop ACK from the next node |
| 3 | `PKT_FLAG_FWD` | Relay | Marks this packet as having been forwarded |

#### SensorPayload — 7 bytes, appended after the header on DATA packets

| Byte(s) | Field | Notes |
|---------|-------|-------|
| 0 | `schema_version` | Always `0x01` |
| 1 | `sensor_type` | `0x03` = DHT22 |
| 2–3 | `temp_c_x10` | Signed int16, big-endian. e.g. `253` = 25.3 °C |
| 4–5 | `humidity_x10` | Unsigned int16, big-endian. e.g. `655` = 65.5% |
| 6 | `status` | `0x00` = OK, `0x01` = sensor read error |

Integers are multiplied by 10 before packing so that one decimal place of precision is preserved without using floats in the payload.

#### BeaconPayload — 4 bytes, appended after the header on BEACON packets

| Byte | Field | Notes |
|------|-------|-------|
| 0 | `schema_version` | Always `0x01` |
| 1 | `queue_pct` | Sender's TX queue occupancy, 0–100 |
| 2 | `link_quality` | Sender's RSSI to its own parent, mapped to 0–100 |
| 3 | `parent_health` | 100 = has valid parent, 0 = orphaned |

Downstream nodes use `queue_pct` and `parent_health` when scoring this node as a potential parent.

#### BridgeAggV1 — LoRaWAN uplink payload (edge node only)

Header (3 bytes):

| Byte | Field | Value |
|------|-------|-------|
| 0 | `schema_version` | `0x02` |
| 1 | `bridge_id` | Edge node ID (`0x01` or `0x06`) |
| 2 | `record_count` | Number of sensor records following (max 7) |

Each record (14 bytes):

| Byte(s) | Field | Notes |
|---------|-------|-------|
| 0 | `mesh_src_id` | Original sensor node ID |
| 1 | `mesh_seq` | Packet sequence number from sensor |
| 2 | `sensor_type_hint` | `0x03` = DHT22 |
| 3 | `hop_estimate` | `DEFAULT_TTL − received_ttl` |
| 4–5 | `edge_uptime_s` | Edge uptime when received, big-endian uint16 |
| 6 | `opaque_len` | Payload length (always 7 for DHT22) |
| 7–13 | `opaque_payload` | Raw 7-byte SensorPayload, copied as-is |

Maximum uplink size: `3 + (7 × 14) = 101 bytes`.

### Key Inline Functions

| Function | Description |
|----------|-------------|
| `crc16_ccitt(data, len)` | CRC16-CCITT (polynomial `0x1021`, init `0xFFFF`) over first `len` bytes |
| `set_mesh_crc(hdr)` | Computes CRC over bytes 0–7 and writes it big-endian into `hdr->crc16` |
| `validate_mesh_crc(hdr)` | Returns `true` if stored CRC matches freshly computed CRC |
| `lbt_transmit(radio, data, len)` | Listen-Before-Talk wrapper — see [Listen-Before-Talk](#listen-before-talk-lbt) |

---

## Sensor Node — `sensor_node/sensor_node.ino`

**Role:** Reads temperature and humidity from a DHT22 sensor, packs the reading into the mesh packet format, and delivers it to its chosen parent (relay or edge node) with ACK confirmation.

**NODE_ID:** `0x03` (Sensor 1) or `0x04` (Sensor 2). Change `#define NODE_ID` at the top of the file before flashing.

### Boot Sequence

```
Serial init
  → candidates table initialised (all invalid)
  → PMU init (powers SX1262 via ALDO2 rail)
  → Radio init (SF7, 125 kHz, CR 4/5, sync 0x12)
  → Phase offset delay (TX_PHASE_OFFSET_MS)
  → Block-listen for first beacon
```

The **phase offset delay** staggers boot-time beacon scanning so both sensors do not scan simultaneously after a shared power-on event:

| Node | NODE_ID | `(ID % 5) × 2000` |
|------|---------|-------------------|
| Sensor 1 | `0x03` | 6 000 ms |
| Sensor 2 | `0x04` | 8 000 ms |

The sensor does **not transmit** until it has found and selected a valid parent. It blocks in `listen_for_beacons()` at the start of each loop iteration until one is heard.

### Main Loop State Machine

```
loop()
 │
 ├─ [No valid parent]
 │   └─► listen_for_beacons(TX_INTERVAL_MS)   ← up to 30 s scan
 │         └─ still no parent? → return and loop again
 │
 ├─ check_parent_timeout()
 │   └─ timed out? → invalidate parent → listen_for_beacons(TX_INTERVAL_MS) → return
 │
 └─ [Has valid parent]
     ├─ [TX due: millis() − last_tx_ms ≥ TX_INTERVAL_MS]
     │   ├─ read_sensor()              ← dummy data until DHT22 wired
     │   ├─ build_sensor_payload()     ← pack 7-byte SensorPayload
     │   ├─ build_mesh_header()        ← pack 10-byte header + CRC
     │   ├─ delay(random 0–2000 ms)    ← TX jitter
     │   ├─ send_with_ack()
     │   │   ├─ success → log TX line
     │   │   └─ all retries failed → current_parent_idx = 0xFF (force re-evaluation)
     │   └─ listen_for_beacons(2000 ms)   ← short window to refresh candidates
     │
     └─ [Between TX cycles]
         └─ listen_for_beacons(500 ms)    ← keep candidate table fresh
```

`TX_INTERVAL_MS` is `30 000` ms during development. Change it to `120 000` ms for final test runs.

### Sensor Read (`read_sensor`)

Currently returns **dummy data** (randomised within realistic ranges). The real DHT22 path is present but commented out. To switch:

1. Uncomment the `#include <DHT.h>` block and `DHT dht(...)` declaration at the top.
2. Uncomment `dht.begin()` and the 2-second delay in `setup()`.
3. Inside `read_sensor()`, comment out the dummy block and uncomment the real DHT22 read block.

The function signature (`bool read_sensor(float &temp, float &hum)`) does not change.

### Payload Construction (`build_sensor_payload`)

Packs temperature and humidity as fixed-point integers (×10) into a 7-byte array using **explicit big-endian byte packing**. Do not replace this with a struct cast — struct padding is compiler-dependent and will silently corrupt the payload on some toolchains.

### Transmission with ACK (`send_with_ack`)

1. Calls `lbt_transmit()` to send the packet (with channel-free check).
2. Switches to RX and calls `wait_for_ack()` for up to `ACK_TIMEOUT_MS` (600 ms).
3. Retries up to `MAX_RETRIES` (3) times with a small increasing back-off (`100 ms × attempt number`).
4. Returns `true` on the first successful ACK, `false` if all retries fail.

On total failure, `current_parent_idx` is reset to `0xFF`. This causes the next loop iteration to run parent re-evaluation before attempting another TX.

### ACK Validation (`wait_for_ack`)

A received packet is accepted as a valid ACK only if **all three** conditions hold simultaneously:

- `GET_PKT_TYPE(flags) == PKT_TYPE_ACK`
- `dst_id == NODE_ID` (addressed to this sensor)
- `src_id == get_parent_id()` (came from the current parent)

Any other packet arriving during the ACK window (e.g. a beacon from another node) causes the radio to be restarted immediately and the loop continues waiting, so no packets are lost.

### Configuration Points

| What to change | Where |
|----------------|-------|
| Node identity | `#define NODE_ID` at top of file |
| TX interval | `#define TX_INTERVAL_MS` at top of file |
| Switch to real DHT22 | Uncomment DHT22 includes + `dht.begin()` in `setup()` + real read code in `read_sensor()` |

---

## Relay Node — `relay_node/relay_node.ino`

**Role:** The relay is the core research artefact of this project. It forwards packets from sensor nodes to an edge node **without ever inspecting the payload** — it reads only the 10-byte MeshHeader and copies everything after byte 9 blindly. This *payload-agnostic guarantee* means the relay firmware never needs updating when sensor payload formats change.

**NODE_ID:** `0x02` (fixed, no configuration needed).

### Boot Sequence

```
Serial init
  → dedup table initialised (all invalid)
  → candidates table initialised (all invalid)
  → PMU init
  → Radio init (starts in continuous RX mode)
  → Phase offset delay (4 000 ms)
  → First beacon broadcast
  → Continuous listen
```

The relay beacons **immediately at boot**, even before it has found an edge node as its own parent. This lets sensors discover the relay as quickly as possible. `my_rank` starts at `RANK_RELAY` (1) by default, and is updated to `parent.rank + 1` once a parent edge node is confirmed.

### Main Loop

```
loop()
 ├─ [Beacon due: millis() − last_beacon_ms ≥ 10 s]
 │   └─► broadcast_beacon()
 ├─ check_parent_timeout()
 └─ receive_and_process()   ← non-blocking; returns immediately if no packet
```

The relay is **always in RX mode** between transmissions. Unlike the sensor, it never blocks — `receive_and_process()` checks `rxFlag` and returns on the spot if nothing has arrived, keeping the loop tight and responsive.

### Receive-and-Process Pipeline

Every incoming packet goes through these checks **in order**. Failure at any stage drops the packet and returns immediately.

```
receive_and_process()
 │
 ├─ 1. rxFlag set?                                      → not set: return
 ├─ 2. radio.readData() + radio.startReceive() at once  ← radio never goes deaf
 ├─ 3. readData() succeeded?                            → error: DROP + log code
 ├─ 4. len ≥ MESH_HEADER_SIZE (10 bytes)?               → too_short: DROP
 ├─ 5. src_id == NODE_ID?                               → own echo: silent DROP
 ├─ 6. PKT_TYPE_BEACON?
 │       └─ rank < my_rank? → process_beacon()
 │       └─ return (beacons are never forwarded)
 ├─ 7. PKT_TYPE_ACK?                                    → DROP (not forwarded)
 ├─ 8. is_duplicate(src_id, seq)?                       → duplicate: DROP
 ├─ 9. CRC valid over bytes 0–7?                        → crc_fail: DROP
 ├─ 10. ttl > 0?                                        → ttl_drop: DROP
 ├─ 11. record_dedup(src_id, seq)
 ├─ 12. ACK_REQ flag set? → send_ack(prev_hop, seq)     ← ACK BEFORE forward
 └─ 13. has_valid_parent? → forward_packet()            → no_parent: DROP
```

**Why ACK before forwarding (step 12)?** The upstream sender is blocking in `wait_for_ack()` with a 600 ms timeout. Sending the ACK immediately, before the potentially slower forward transmission, unblocks the sender as fast as possible.

**Why dedup before CRC (steps 8–9)?** A corrupted retransmission of a packet we already processed will be caught by the cheaper dedup check first, saving the CRC computation. Conversely, a packet with a new seq_num that happens to be corrupted passes dedup and is correctly discarded by the CRC check. Packets are only *recorded* in the dedup table after all validations pass (step 11), so a corrupt packet never poisons the table.

**Why restart RX at step 2, not after validation?** If the packet is dropped for any reason (CRC, dedup, etc.), the radio would otherwise sit in standby for the entire duration of the validation logic. Restarting immediately means at most one packet can be missed per invalid packet received — an acceptable trade-off.

### Payload-Agnostic Forwarding (`forward_packet`)

This is the defining behaviour of the relay. The function modifies **only the header fields the relay owns**, then retransmits the entire buffer verbatim:

| Step | What changes | Reason |
|------|-------------|--------|
| Set `PKT_FLAG_FWD` in `flags` | Marks packet as forwarded; edge can verify this |
| Clear `PKT_FLAG_ACK_REQ` in `flags` | The edge does not need to ACK the relay |
| `dst_id` → `get_parent_id()` | Route to our current parent edge |
| `prev_hop` → `NODE_ID` | Record that the relay was the last hop |
| `ttl` → `ttl − 1` | Count this hop |
| `crc16` → recomputed | CRC must reflect the modified header bytes |

Bytes `MESH_HEADER_SIZE` (10) onward are **never read, never modified, never inspected**. They are copied as a block. This is the invariant that must never be broken — if you need to add relay-level metadata, add a new header field; do not access the payload region.

A sanity check guards against a malformed `payload_len` field that claims more bytes than were actually received, preventing an out-of-bounds transmit.

### Beacon Broadcasting (`broadcast_beacon`)

Sent every `BEACON_INTERVAL_MS` (10 s). Key dynamic fields:

| Field | Value | When |
|-------|-------|------|
| `rank` | `1` (RANK_RELAY) | After parent found: `parent.rank + 1` |
| `rank` | `255` | After parent timeout (orphan signal) |
| `queue_pct` | `pending_forwards × 100 / 10` | Real-time forwarding load |
| `link_quality` | RSSI to parent, mapped 0–100 | 0 if no parent |
| `parent_health` | `100` / `0` | Whether parent is valid |

When `rank = 255`, downstream sensors score this relay at near-zero and will re-route around it — which is the intended failover behaviour.

### Deduplication Table

A 16-slot circular buffer of `DedupEntry` structs (`src_id`, `seq_num`, `timestamp`, `valid`). On lookup, entries older than `DEDUP_WINDOW_MS` (30 s) are expired in-place. New entries overwrite at `dedup_head` (advancing modulo 16), replacing the oldest entry when the table is full.

### Parent Selection and Failover

Same scoring algorithm as the sensor node (see [Parent Scoring Algorithm](#parent-scoring-algorithm)). The relay chooses between Edge 1 (`0x01`) and Edge 2 (`0x06`) as its parent.

Normal operation: Edge 1 wins if it has a stronger or equal signal.

Failover: If Edge 1 stops beaconing, `check_parent_timeout()` fires after 35 s. The relay:
1. Invalidates Edge 1's candidate entry.
2. Sets `my_rank = 255`.
3. Immediately calls `update_parent_selection()` — if Edge 2's entry is still fresh, it is selected instantly.
4. Subsequent beacons advertise the new rank (`edge2.rank + 1 = 1`).

Recovery: When Edge 1 resumes beaconing, it re-enters the candidate table. It will only be re-selected if its score exceeds Edge 2's score by at least `PARENT_SWITCH_HYSTERESIS` (8 points), preventing immediate flapping.

### Configuration Points

| What to change | Where |
|----------------|-------|
| Beacon interval | `BEACON_INTERVAL_MS` in `mesh_protocol.h` |
| Dedup window / table size | `DEDUP_WINDOW_MS` / `DEDUP_TABLE_SIZE` in `mesh_protocol.h` |
| Parent switch sensitivity | `PARENT_SWITCH_HYSTERESIS` in `mesh_protocol.h` |
| Parent timeout duration | `PARENT_TIMEOUT_MS` in `mesh_protocol.h` |

---

## Edge Node — `edge_node/edge_node.ino`

**Role:** The mesh sink. The edge node receives all sensor data arriving from the mesh, acknowledges it hop-by-hop, buffers it in an aggregation structure, and periodically uplinks the buffer to The Things Network via LoRaWAN. It also broadcasts rank-0 beacons so the relay and sensors know where the top of the hierarchy is.

**NODE_ID:** Configured in `edge_node/config.h` — `0x01` for Edge 1, `0x06` for Edge 2. The **same `.ino` is flashed to both edges**; only `config.h` differs between them.

### Boot Sequence

```
Serial init
  → PMU init (powers SX1262 via ALDO2)
  → Radio init (mesh LoRa mode: SF7, 125 kHz, sync 0x12)
  → [If ENABLE_LORAWAN defined]:
      LoRaWAN OTAA join attempt (blocking, up to ~60 s)
      switch_to_mesh_rx()    ← restore mesh radio config after join
  → Aggregation buffer cleared
  → Dedup table cleared
  → Timers initialised (boot_time, last_flush_time, last_beacon_time)
  → Continuous mesh listen
```

LoRaWAN is **disabled by default**. Uncomment `#define ENABLE_LORAWAN` at the top of `edge_node.ino` once real TTN credentials are filled in `config.h`. Without this, the node works as a pure mesh sink (useful for testing mesh behaviour independently of TTN).

The first beacon is scheduled using the phase offset so it fires `BEACON_PHASE_OFFSET` ms after boot rather than immediately:

```cpp
last_beacon_time = millis() - BEACON_INTERVAL_MS + BEACON_PHASE_OFFSET;
```

| Node | NODE_ID | `(ID % 5) × 2000` |
|------|---------|-------------------|
| Edge 1 | `0x01` | 2 000 ms |
| Edge 2 | `0x06` | 2 000 ms |

### Radio Time-Sharing

The T-Beam has one SX1262 radio that must serve two roles: raw LoRa mesh RX and LoRaWAN TX. The edge uses a cooperative `RadioMode` state machine:

```
Normal:        MESH_LISTEN ──────────────────────────────────────────────
                                                                         │
Flush trigger: MESH_LISTEN ─► LORAWAN_TXRX ─► MESH_LISTEN               │
                                    │                                     │
                                    ├─ lbt_transmit() LoRaWAN uplink     │
                                    ├─ Wait for RX1/RX2 windows (~5 s)  │
                                    └─ switch_to_mesh_rx()               │
```

`switch_to_mesh_rx()` explicitly reconfigures all SX1262 parameters back to mesh values. This is necessary because `LoRaWANNode` changes frequency, spreading factor, and sync word during the join/uplink sequence, and they must be restored before mesh reception can resume.

During the LoRaWAN window (~5 s), the edge is deaf to mesh packets. At 30–120 s sensor intervals, no sensor readings are lost.

### Main Loop

```
loop() [MESH_LISTEN state]
 ├─ receive_mesh_packets()      ← check rxFlag, validate, dispatch
 ├─ broadcast_beacon_if_due()  ← every 10 s
 └─ [agg_count ≥ 7  OR  elapsed ≥ 240 s]  AND  agg_count > 0  AND  lorawan_joined
     └─ radio_mode = LORAWAN_TXRX
        send_lorawan_uplink()
        switch_to_mesh_rx()
        radio_mode = MESH_LISTEN
```

### Mesh Packet Reception (`receive_mesh_packets`)

The edge uses the `MeshHeader` struct (cast from the receive buffer) rather than raw byte indexing. Validation pipeline:

```
1. rxFlag set?                             → not set: return immediately
2. radio.readData() + radio.startReceive() ← radio back in RX before any processing
3. readData() succeeded?                   → error: DROP
4. len ≥ 10 bytes?                         → too_short: DROP, packets_dropped++
5. src_id == NODE_ID?                      → own echo: silent DROP
6. is_duplicate(src_id, seq_num)?          → duplicate: DROP, packets_dropped++
7. validate_mesh_crc(hdr)?                 → crc_fail: DROP, packets_dropped++
8. hdr->ttl == 0?                          → ttl_drop: DROP, packets_dropped++
9. mark_seen(src_id, seq_num)
10. Dispatch by GET_PKT_TYPE(flags):
    PKT_TYPE_BEACON → handle_beacon()   (log only)
    PKT_TYPE_DATA   → handle_data()
    PKT_TYPE_ACK    → ignore
```

`packets_received` and `packets_dropped` counters accumulate for the lifetime of the node and can be read via serial for PDR analysis.

### Data Handling (`handle_data`)

1. Computes `hop_count = DEFAULT_TTL − hdr->ttl` (estimates hops taken from remaining TTL).
2. Sends ACK to `hdr->prev_hop` if `PKT_FLAG_ACK_REQ` is set.
3. If `agg_count < MAX_AGG_RECORDS` and `hdr->payload_len == SENSOR_PAYLOAD_SIZE`, adds a new `AggRecord` to the buffer.

Each `AggRecord` stores: `mesh_src_id`, `mesh_seq`, `sensor_type_hint`, `hop_estimate`, `edge_uptime_s`, and the raw 7-byte `opaque_payload` copied without interpretation.

### Aggregation Buffer and Flush

The buffer holds up to `MAX_AGG_RECORDS` (7) records. A flush is triggered when **either** condition is true:

| Trigger | Condition |
|---------|-----------|
| Buffer full | `agg_count >= 7` |
| Timeout | `millis() − last_flush_time >= 240 000 ms` |

On flush, `send_lorawan_uplink()` packs the records into BridgeAggV1 format and sends via `lorawan_node.sendReceive()` on FPort 1. The buffer is cleared with `memset` after every flush, regardless of whether the uplink succeeded.

### Beacon Broadcasting (`broadcast_beacon_if_due`)

Rank-0 beacons are sent every 10 s. Dynamic payload fields:

| Field | Value | Purpose |
|-------|-------|---------|
| `rank` | `0` (always) | Tells relay/sensors this is the top of the hierarchy |
| `queue_pct` | `agg_count × 100 / 7` | Relay uses this to prefer the less-loaded edge |
| `link_quality` | `100` | Edge has a perfect "link" to itself |
| `parent_health` | `lorawan_joined ? 100 : 0` | Advertises LoRaWAN status; relay can prefer the edge with a working uplink |

### Configuration Points

| What to change | Where |
|----------------|-------|
| Node identity | `#define NODE_ID` in `edge_node/config.h` (`0x01` or `0x06`) |
| TTN OTAA credentials | `DEV_EUI`, `JOIN_EUI`, `APP_KEY` in `edge_node/config.h` |
| Enable LoRaWAN | Uncomment `#define ENABLE_LORAWAN` at top of `edge_node.ino` |
| LoRaWAN FPort | `#define LORAWAN_FPORT` in `config.h` |
| Aggregation flush interval | `AGG_FLUSH_TIMEOUT_MS` in `mesh_protocol.h` |
| Max buffered records | `MAX_AGG_RECORDS` in `mesh_protocol.h` |

---

## Key Algorithms

### Parent Scoring Algorithm

Used identically by both the sensor node and the relay node. The score is a weighted sum of three factors:

```
score = (60 × rank_score  / 100)
      + (25 × rssi_score  / 100)
      + (15 × queue_score / 100)
```

**rank_score** — heavily rewards nodes closer to the edge:

| Rank | rank_score | Formula |
|------|-----------|---------|
| 0 (edge) | 100 | special case |
| 1 (relay) | 85 | `100 − 1×15` |
| 2 (sensor) | 70 | `100 − 2×15` |
| 255 (orphan) | 0 | clamped by `max(0, ...)` |

**rssi_score** — rewards stronger signals. RSSI is clamped to `[−120, −60]` dBm and mapped linearly to `[0, 100]`:

```
rssi_score = (clamp(rssi, −120, −60) + 120) × 100 / 60
```

**queue_score** — rewards less-loaded parents:

```
queue_score = 100 − queue_pct
```

**Weight rationale:** Topology (rank) dominates at 60% because routing through the correct tier matters most. Link quality contributes 25%. Congestion contributes 15%.

**Hysteresis:** A parent switch only happens when `best_score >= current_score + PARENT_SWITCH_HYSTERESIS` (8 points) AND `best_idx != current_parent_idx`. This prevents flapping when two candidates have nearly identical scores that fluctuate with RSSI variation.

**First parent:** When `current_parent_idx == 0xFF` (no parent yet), the best-scoring candidate is accepted immediately — no hysteresis threshold applies.

### Listen-Before-Talk (LBT)

Every transmission goes through `lbt_transmit()` in `mesh_protocol.h`, which wraps `radio.transmit()` with a Channel Activity Detection (CAD) scan:

```
for attempt in [0, 1, 2]:
    result = radio.scanChannel()
    if result == RADIOLIB_CHANNEL_FREE:
        return radio.transmit(data, len)       ← transmit immediately
    back_off = (100 ms << attempt) + random(0, 50 ms)
    delay(back_off)                            ← 100ms, 200ms, 400ms + jitter

# All CAD retries exhausted — transmit anyway to avoid dropping the packet
return radio.transmit(data, len)
```

The forced transmission after three failed CADs is intentional: in a small 5-node network, a persistently busy channel almost certainly indicates a hardware issue rather than genuine congestion, and dropping packets silently would be worse.

### Boot Phase Offset

All nodes apply a `delay()` at boot time to spread beacon transmissions across the 10-second beacon window:

```
offset_ms = (NODE_ID % 5) × 2000 ms
```

| Node | ID | Offset |
|------|----|--------|
| Edge 1 | `0x01` | 2 000 ms |
| Relay | `0x02` | 4 000 ms |
| Sensor 1 | `0x03` | 6 000 ms |
| Sensor 2 | `0x04` | 8 000 ms |
| Edge 2 | `0x06` | 2 000 ms |

### TX Jitter (Sensor Only)

Before each data transmission, the sensor adds `delay(random(0, 2000))`. This is independent of the boot phase offset and fires on every TX cycle, preventing two sensors from transmitting simultaneously if they happen to synchronise their 30/120-second intervals after a shared reboot.

### Parent Timeout and the Orphan State

`check_parent_timeout()` runs every loop iteration. If the current parent's last beacon was more than `PARENT_TIMEOUT_MS` (35 s) ago:

1. The parent's candidate entry is marked invalid.
2. `current_parent_idx` is reset to `0xFF`.
3. `update_parent_selection()` is called immediately — if another valid candidate exists in the table, it is selected without waiting for the next beacon scan.

**Relay-specific:** On timeout the relay also sets `my_rank = 255`. This causes the next beacon to advertise rank 255, which sensors score at essentially zero. Sensors that were routing through the relay see its score collapse and — if a direct edge link is available in their candidate table — will switch parents at their next `update_parent_selection()` call.

---

## Serial Log Reference

All nodes log to Serial at **115200 baud**. Use these prefixes to identify what is happening during a live run or when analysing a captured log.

### Sensor Node Prefixes

| Prefix | Event |
|--------|-------|
| `BOOT \|` | Startup and initialisation messages |
| `LISTEN \|` | Scanning for beacons (no parent state) |
| `BCN \|` | Beacon received and parsed into candidate table |
| `PARENT \|` | Parent selected, switched, or timed out |
| `TX \|` | Transmission attempt, jitter delay, or result |
| `ACK \|` | ACK received from parent, or CRC mismatch on received packet |
| `SENSOR \|` | Sensor read failure (skipping TX cycle) |

Example successful TX log line:
```
TX | node=3 | seq=12 | temp=25.3C | hum=65.5% | parent=2 | rssi=-68
```

### Relay Node Prefixes

| Prefix | Event |
|--------|-------|
| `BOOT \|` | Startup and initialisation messages |
| `BCN  \|` | Beacon sent (with queue/lq/health fields) or received |
| `PARENT \|` | Parent selected, switched, or timed out |
| `FWD  \|` | Packet forwarded successfully |
| `ACK  \|` | ACK sent to upstream node |
| `DROP \|` | Packet dropped — see `reason=` field |
| `LBT \|` | Channel busy back-off during `lbt_transmit()` |

**Drop reasons:**

| `reason=` | Cause |
|-----------|-------|
| `too_short` | Packet shorter than 10 bytes |
| `duplicate` | Same `(src_id, seq_num)` seen within the last 30 s |
| `crc_fail` | Header CRC does not match computed value |
| `ttl_drop` | TTL field was 0 |
| `no_parent` | No parent edge node selected yet |

Example forward log line:
```
FWD  | uid=0312 | rssi=-72 | ttl=9 | parent=1
```

### Edge Node Prefixes

| Prefix | Event |
|--------|-------|
| `[INIT]` | Startup phase |
| `[OK]` | Successful initialisation step |
| `[ERROR]` | Fatal initialisation failure (node halts) |
| `[WARN]` | Non-fatal issue (e.g. aggregation buffer full) |
| `[BCN]` | Beacon sent or received from mesh |
| `[DROP]` | Packet dropped during validation |
| `RX_MESH \|` | Valid data packet received and logged |
| `ACK \|` | ACK sent to previous hop |
| `AGG \|` | Record added to aggregation buffer |
| `[FLUSH]` | Uplink flush triggered |
| `FLUSH \|` | Uplink payload statistics |
| `UPLINK \|` | LoRaWAN uplink result |
| `[RADIO]` | Radio mode switch (mesh ↔ LoRaWAN) |

Example receive log line:
```
RX_MESH | src=0x3 | seq=12 | hops=2 | rssi=-68
```

---

## Extending the Firmware

### Adding a New Sensor Type

1. Define a new payload schema in `mesh_protocol.h` (new struct, size constant, and sensor type ID constant). Do not change `MESH_HEADER_SIZE`.
2. Update the sensor node to pack the new format in `build_sensor_payload()`.
3. **The relay requires zero changes** — it never looks beyond byte 9.
4. Update the edge node's `handle_data()` if the new payload is a different size from `SENSOR_PAYLOAD_SIZE`. The `opaque_payload` field in `AggRecord` is currently sized for 7 bytes.
5. Update the TTN payload decoder (`tools/decoder.js`) to parse the new format by inspecting the `sensor_type_hint` field.

### Adding a Second Relay

1. Assign a new NODE_ID (e.g. `0x07`).
2. Flash `relay_node.ino` with the new ID.
3. No changes to sensor or edge firmware — sensors will automatically discover and score the new relay via beacons.
4. Verify that `MAX_CANDIDATES` (currently 4) in `mesh_protocol.h` is large enough to hold all candidates a sensor might see simultaneously.

### Changing the TX Interval

Edit `TX_INTERVAL_MS` in `sensor_node.ino`:
- `30000` during development (faster feedback on the serial monitor)
- `120000` for final test runs (consistent with TTN fair-use duty cycle)

### Enabling LoRaWAN

1. Obtain OTAA credentials from TTN (DevEUI, JoinEUI, AppKey) — see `SETTINGUPTTN.md`.
2. Fill them into `edge_node/config.h`.
3. Uncomment `#define ENABLE_LORAWAN` near the top of `edge_node.ino`.
4. Flash the edge node. The join attempt happens at boot; if it fails, the node continues operating in mesh-only mode and logs a warning.
