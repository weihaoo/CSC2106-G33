# Mesh-Only Test Plan — CSC2106 Group 33

**Hardware:** LilyGo T-Beam (SX1262, 923 MHz AS923)  
**Firmware commit tested against:** see `git log`  
**LoRaWAN:** DISABLED for all tests (`#define ENABLE_LORAWAN` must be commented out in `edge_node.ino`)  
**Serial baud rate:** 115200  
**TX_INTERVAL_MS:** 30000 (30 s) — confirm in `sensor_node.ino` before testing  
**Tools needed:** Arduino IDE Serial Monitor (or any 115200-baud terminal), one USB cable per board  

---

## Node Summary

| Node | Role | NODE_ID | File | Boot offset |
|------|------|---------|------|-------------|
| Edge 1 | Mesh sink (rank 0) | `0x01` | `edge_node.ino` + `config.h` | 2 s (`0x01 % 5 × 2000`) |
| Relay | Forwarder (rank 1) | `0x02` | `relay_node.ino` | 4 s (`0x02 % 5 × 2000`) |
| Sensor 1 | Data originator | `0x03` | `sensor_node.ino` (NODE_ID=0x03) | 6 s |
| Sensor 2 | Data originator | `0x04` | `sensor_node.ino` (NODE_ID=0x04) | 8 s |
| Edge 2 | Mesh sink (rank 0) | `0x06` | `edge_node.ino` + `config.h` | 2 s (`0x06 % 5 × 2000`) |

---

## How to Read the Serial Logs

Each firmware writes structured one-line log entries. The key prefixes are:

| Prefix | Node(s) | What it means |
|--------|---------|---------------|
| `BOOT \|` | all | Startup message; also shows phase-offset delay |
| `PMU OK` | all | AXP2101 and SX1262 powered correctly |
| `RadioLib OK` | all | Radio initialised at 923 MHz SF7 |
| `BCN \|` | relay, edge | Beacon transmitted or received |
| `LISTEN \|` | sensor | Scanning for beacons (no parent yet) |
| `PARENT \| Selected` | sensor, relay | First parent chosen |
| `PARENT \| Switching` | sensor, relay | Switched to a better parent |
| `PARENT \| Timeout` | sensor, relay | Parent went silent; re-evaluating |
| `TX \| Attempt` | sensor | Data packet TX attempt (1–3) |
| `TX \| node=` | sensor | TX succeeded (ACK received) |
| `TX \| All retries failed` | sensor | All 3 TX attempts timed out |
| `ACK \| Received` | sensor | Hop-ACK received from parent |
| `ACK  \|` | relay, edge | ACK sent to sender |
| `FWD  \|` | relay | Packet forwarded to edge |
| `DROP \|` | relay, edge | Packet discarded (reason shown) |
| `RX_MESH \|` | edge | Data packet received |
| `AGG \|` | edge | Record added to aggregation buffer |
| `[BCN]` | edge | Beacon TX/RX logged |

---

## Section 1 — Individual Node Bring-Up

These tests power one node at a time. No other nodes are on-air.

---

### T-01: Edge Node Boot

**Purpose:** Confirm the edge node initialises correctly and starts beaconing.

**Setup:** Flash `edge_node.ino` with `NODE_ID = 0x01` in `config.h`. Ensure `#define ENABLE_LORAWAN` is commented out.

**Procedure:**
1. Connect USB. Open Serial Monitor at 115200.
2. Power-cycle the board (press reset or unplug/replug USB).
3. Wait 15 seconds.

**Expected serial output (order may vary slightly):**

```
═══════════════════════════
     EDGE NODE - CSC2106 G33 (Person 3)
═══════════════════════════
Node ID: 0x1
Rank: 0
...
[OK] PMU initialized — LoRa radio powered via ALDO2
[OK] RadioLib mesh config:
     Frequency: 923.00 MHz (AS923)
     SF: 7, BW: 125.00 kHz
[INIT] LoRaWAN DISABLED (mesh-only testing mode)
Edge node ready! Listening for mesh packets...
```

Then every 10 seconds:

```
[BCN] TX | rank=0 | queue=0%
```

**Pass criteria:**
- [ ] PMU OK line appears (no `[ERROR] PMU init failed`)
- [ ] RadioLib OK with `923.00 MHz`
- [ ] "LoRaWAN DISABLED" line appears (not a LoRaWAN join attempt)
- [ ] Beacon TX appears within ~12 s of the "ready" line and repeats every ~10 s
- [ ] No error lines after boot

**Notes:**
- If PMU fails, check I2C wiring and that the battery connector is seated.
- Repeat with `NODE_ID = 0x06` to verify Edge 2 boots identically.

---

### T-02: Relay Node Boot

**Purpose:** Confirm the relay initialises and immediately starts beaconing at rank 255 (no parent yet).

**Setup:** Flash `relay_node.ino`. No other nodes on-air.

**Procedure:**
1. Open Serial Monitor at 115200.
2. Power-cycle the board.
3. Wait 10 seconds.

**Expected serial output:**

```
=== CSC2106 G33 | Relay Node 0x02 ===
PMU OK — LoRa radio powered on via ALDO2
BOOT | Beacon phase offset: 4000 ms
RadioLib OK — SX1262 @ 923 MHz, SF7
BCN  | queue=0% | lq=0 | health=0
BOOT | Relay ready. Listening...
```

Then every 10 seconds:

```
BCN  | queue=0% | lq=0 | health=0
```

**Pass criteria:**
- [ ] PMU OK and RadioLib OK present
- [ ] Phase offset printed as `4000 ms`
- [ ] First beacon appears within ~5 s of the "ready" line
- [ ] `lq=0` and `health=0` (no parent yet, expected)
- [ ] Beacon repeats every ~10 s

**Note:** `rank` in the beacon is `my_rank`, which starts at `RANK_RELAY = 1` but is broadcast as 255 (`my_rank` is updated to `parent.rank + 1` after a parent is found — before that it stays at 1 in the code, but `health=0` correctly shows no parent).

---

### T-03: Sensor Node Boot (No Parent)

**Purpose:** Confirm a sensor node boots, applies its phase offset, and then blocks waiting for a beacon.

**Setup:** Flash `sensor_node.ino` with `NODE_ID = 0x03`. No other nodes on-air.

**Procedure:**
1. Open Serial Monitor at 115200.
2. Power-cycle the board.
3. Wait 45 seconds.

**Expected serial output:**

```
=== CSC2106 G33 | Sensor Node 0x3 ===
PMU OK — LoRa radio powered on via ALDO2
BOOT | Beacon phase offset: 6000 ms
RadioLib OK — SX1262 @ 923 MHz, SF7
BOOT | Waiting for beacon before first TX...
LISTEN | No parent yet, scanning for beacons...
LISTEN | No beacon heard. Continuing to wait...
LISTEN | No parent yet, scanning for beacons...
...
```

The "No beacon heard" / "scanning" pair repeats indefinitely.

**Pass criteria:**
- [ ] PMU OK and RadioLib OK present
- [ ] Phase offset printed as `6000 ms` (for NODE_ID=0x03: `3 % 5 × 2000 = 6000`)
- [ ] Sensor never attempts a TX (no `TX | Attempt` lines)
- [ ] Sensor loops on "No beacon heard" indefinitely

**Note:** Repeat with `NODE_ID = 0x04`. Expected offset = `8000 ms`.

---

## Section 2 — Beacon Propagation and Parent Selection

---

### T-04: Sensor Discovers Edge Node (Direct)

**Purpose:** Sensor selects an edge node as its parent when in direct range, with no relay present.

**Setup:** Edge node (0x01) already running from T-01. Now power on Sensor 1 (0x03) in the same room.

**Procedure:**
1. Power on Sensor 1. Open Serial Monitor.
2. Wait up to 45 seconds for the sensor to hear a beacon.

**Expected on Sensor 1 serial:**

```
LISTEN | No parent yet, scanning for beacons...
BCN | src=0x1 | rank=0 | rssi=<value> | queue=0%
PARENT | Selected 0x1 (score=<value>)
```

Then 30 seconds later, a data TX:

```
TX | Attempt 1/3 → parent=0x1
ACK | Received from 0x1
TX | node=3 | seq=0 | temp=XX.XC | hum=XX.X% | parent=1 | rssi=<value>
```

**Expected on Edge 1 serial:**

```
RX_MESH | src=0x3 | seq=0 | hops=1 | rssi=<value>
ACK | to=0x3 | seq=0
AGG | Added to buffer | count=1/7
```

**Pass criteria:**
- [ ] Sensor prints `BCN | src=0x1 | rank=0`
- [ ] Sensor prints `PARENT | Selected 0x1`
- [ ] Sensor prints `ACK | Received from 0x1` on first TX attempt
- [ ] Edge prints `RX_MESH | src=0x3`
- [ ] Edge prints `ACK | to=0x3`
- [ ] Edge prints `AGG | Added to buffer`
- [ ] No `DROP` lines on the edge for this packet

---

### T-05: Relay Discovers Edge Node

**Purpose:** Relay selects Edge 1 as its parent after hearing its beacon.

**Setup:** Edge 1 already running. Power on relay (0x02).

**Procedure:**
1. Power on relay. Open Serial Monitor.
2. Wait up to 20 seconds.

**Expected on Relay serial:**

```
BCN  | src=0x1 | rank=0 | rssi=<value> | queue=0%
PARENT | Selected 0x1 (score=<value>)
BCN  | queue=0% | lq=<value> | health=100
```

**Expected on Edge 1 serial:**

```
[BCN] RX from 0x2 | rank=1 | rssi=<value>
```

**Pass criteria:**
- [ ] Relay prints `PARENT | Selected 0x1`
- [ ] Relay's subsequent beacons show `health=100` (parent is valid)
- [ ] Edge logs the relay's beacon (informational only)

---

### T-06: Sensor Discovers Relay (Intermediate Parent)

**Purpose:** Sensor selects the relay as its parent when the relay is the strongest signal.

**Setup:** All three nodes running: Edge 1 (0x01), Relay (0x02), Sensor 1 (0x03). Place the sensor physically closer to the relay than to the edge node, or confirm the relay is already beaconing.

**Procedure:**
1. Wait for the relay to finish T-05 (has parent, `health=100`).
2. Restart Sensor 1 and watch its parent selection.

**Expected on Sensor 1 serial:**

```
BCN | src=0x2 | rank=1 | rssi=<relay_rssi> | queue=0%
BCN | src=0x1 | rank=0 | rssi=<edge_rssi> | queue=0%
PARENT | Selected 0x<whichever scored higher> (score=<value>)
```

**Scoring formula** (from `sensor_node.ino:score_parent()`):  
`score = (60 × rank_score/100) + (25 × rssi_score/100) + (15 × queue_score/100)`  
where `rank_score(0)=100`, `rank_score(1)=85`, `rssi_score` maps `[-120,-60]→[0,100]`.

The edge (rank 0) scores 60 points from rank alone vs 51 for the relay (rank 1).  
The relay can only win if its RSSI is significantly better.

**Pass criteria:**
- [ ] Sensor hears beacons from both 0x01 and 0x02
- [ ] Sensor selects one parent (log shows `PARENT | Selected`)
- [ ] The selection matches the scoring formula (verify manually with the printed RSSI values)
- [ ] Data TX succeeds with ACK

---

### T-07: Parent Selection Hysteresis

**Purpose:** Confirm the sensor does NOT switch parents unless the score improvement is ≥ 8 points (`PARENT_SWITCH_HYSTERESIS`).

**Setup:** Sensor 1 has selected a parent from T-06.

**Procedure:**
1. Note the current parent and its score from the serial log.
2. Without changing any physical positions, let the sensor run through 3 TX cycles.
3. Observe whether it switches parents despite both beacons being present.

**Expected:**  
No `PARENT | Switching` lines unless a genuine improvement of ≥ 8 score points is measured.

**Pass criteria:**
- [ ] No spurious parent switching between consecutive beacon scans at stable signal levels

---

## Section 3 — Data Forwarding (Two-Hop Path)

---

### T-08: Sensor → Relay → Edge (Full Forwarding Chain)

**Purpose:** Verify the complete two-hop data path works end-to-end.

**Setup:** All three nodes running: Edge 1 (0x01), Relay (0x02), Sensor 1 (0x03).  
Sensor 1 must have the relay (0x02) as its current parent (confirm via serial log).

**Procedure:**
1. Wait for a TX cycle on Sensor 1 (~30 s after boot or parent selection).
2. Observe all three serial monitors simultaneously (or check logs in order).

**Expected sequence of events:**

**Sensor 1 serial:**
```
TX | Attempt 1/3 → parent=0x2
ACK | Received from 0x2
TX | node=3 | seq=N | temp=XX.XC | hum=XX.X% | parent=2 | rssi=<value>
```

**Relay serial (within ~1 s of sensor TX):**
```
ACK  | to=3 | for_seq=N
FWD  | uid=3N | rssi=<value> | ttl=9 | parent=1
```

**Edge 1 serial (within ~1 s of relay forward):**
```
RX_MESH | src=0x3 | seq=N | hops=1 | rssi=<value>
ACK | to=0x2 | seq=N
AGG | Added to buffer | count=<n>/7
```

**Pass criteria:**
- [ ] Sensor gets an ACK from the relay (not directly from edge)
- [ ] Relay log shows `FWD` with `ttl=9` (DEFAULT_TTL=10 minus one hop)
- [ ] Edge log shows `src=0x3` (original sensor, not relay — relay does not change src_id)
- [ ] Edge shows `hops=1` (DEFAULT_TTL - received_ttl = 10 - 9 = 1)
- [ ] Edge ACKs back to `0x2` (relay's prev_hop address, not the sensor)
- [ ] AGG buffer increments

---

### T-09: Payload Integrity (Temperature and Humidity Values)

**Purpose:** Confirm the 7-byte sensor payload survives the relay hop intact.

**Setup:** Same as T-08.

**Procedure:**
1. Note the `temp=` and `hum=` values printed by Sensor 1 on a successful TX.
2. Check the Edge 1 AGG buffer. Since LoRaWAN is disabled, the values sit in memory — you will need to add a temporary `Serial.print` in `handle_data()` to decode `opaque_payload`, OR simply add the following debug print inside the `if (agg_count < MAX_AGG_RECORDS ...)` block in `edge_node.ino`:

```cpp
// Temporary debug: decode sensor payload
int16_t t = ((int16_t)rec->opaque_payload[2] << 8) | rec->opaque_payload[3];
uint16_t h = ((uint16_t)rec->opaque_payload[4] << 8) | rec->opaque_payload[5];
Serial.print(F("AGG | temp="));
Serial.print(t / 10.0, 1);
Serial.print(F("C | hum="));
Serial.print(h / 10.0, 1);
Serial.println(F("%"));
```

3. Compare the decoded values at the edge against what Sensor 1 printed.

**Pass criteria:**
- [ ] Temperature value at edge matches sensor within ±0.1°C (encoding precision)
- [ ] Humidity value at edge matches sensor within ±0.1%
- [ ] `status` byte = `0x00` (OK)
- [ ] `schema_version` = `0x01`
- [ ] `sensor_type` = `0x03` (DHT22)

---

### T-10: Both Sensors, Both Paths

**Purpose:** Confirm that Sensor 1 (0x03) and Sensor 2 (0x04) can both deliver readings to the edge concurrently.

**Setup:** Edge 1 (0x01), Relay (0x02), Sensor 1 (0x03), Sensor 2 (0x04) all running.

**Procedure:**
1. Let the network run for 3 full TX cycles (~90 s).
2. Check the edge's AGG buffer count.

**Expected on Edge 1:**
- At least 3 `AGG | Added to buffer` entries with `src=0x3`
- At least 3 `AGG | Added to buffer` entries with `src=0x4`
- No entries with `src=0x2` (relay does not generate sensor data)

**Pass criteria:**
- [ ] Both 0x03 and 0x04 appear as `src` in `RX_MESH` logs on the edge
- [ ] AGG count grows with readings from both sensors
- [ ] No consistent `DROP | reason=duplicate` for unique (src, seq) pairs

---

## Section 4 — ACK and Retry Behaviour

---

### T-11: Retry on No ACK (Simulated)

**Purpose:** Confirm the sensor retries up to `MAX_RETRIES = 3` times when no ACK is received.

**Setup:** Sensor 1 running with a valid parent (relay or edge). The "parent" board will be temporarily powered off.

**Procedure:**
1. Wait for the relay (or edge) to be seen as the parent by Sensor 1 (confirm via `PARENT | Selected`).
2. Power off the parent board.
3. Wait for the next TX cycle on Sensor 1.

**Expected on Sensor 1:**
```
TX | Attempt 1/3 → parent=0xN
TX | No ACK received, retrying...
TX | Attempt 2/3 → parent=0xN
TX | No ACK received, retrying...
TX | Attempt 3/3 → parent=0xN
TX | No ACK received, retrying...
TX | All retries failed. Triggering parent re-evaluation.
```

Then:
```
PARENT | Timeout on 0xN (Xs since last beacon). Re-evaluating...
```
or immediately:
```
PARENT | Parent timed out. Scanning for new parent...
```

**Pass criteria:**
- [ ] Exactly 3 `TX | Attempt` lines appear
- [ ] `TX | All retries failed` appears after the 3rd attempt
- [ ] `current_parent_idx` is reset (next loop scans for a new parent)
- [ ] No crash or freeze

**Note:** After `MAX_RETRIES` fail, the sensor resets `current_parent_idx = 0xFF` and triggers a beacon scan on the next loop iteration.

---

### T-12: ACK is Hop-by-Hop, Not End-to-End

**Purpose:** Verify that the relay ACKs the sensor directly, and the edge ACKs the relay directly. The sensor never receives an ACK from the edge.

**Setup:** Full two-hop path (Sensor → Relay → Edge) as in T-08.

**Procedure:**
1. Observe the `ACK | Received from 0x` line on Sensor 1.
2. Observe the `ACK  | to=` line on the relay.

**Expected:**
- Sensor 1 (0x03) prints: `ACK | Received from 0x2` (the relay, not 0x1 the edge)
- Relay (0x02) prints: `ACK  | to=3 | for_seq=N` (to the sensor)
- Edge (0x01) prints: `ACK | to=0x2 | seq=N` (to the relay's prev_hop)

**Pass criteria:**
- [ ] Sensor ACK src is 0x2 (relay), not 0x1 (edge)
- [ ] Edge ACK dst is 0x2 (relay), not 0x3 (sensor)

---

## Section 5 — Deduplication

---

### T-13: Relay Drops Duplicate Packets

**Purpose:** Confirm the relay's dedup table prevents forwarding the same (src_id, seq_num) pair twice within `DEDUP_WINDOW_MS = 30 s`.

**Setup:** This test requires two edge nodes (or you can simulate by having the sensor in range of both the relay and an edge directly, then use the relay's log).

**Simplified single-relay procedure:**
1. Run full system (Edge 1, Relay, Sensor 1).
2. At the relay, you will see `FWD | uid=3N` once per sensor TX cycle.
3. If you can artificially retransmit the same packet (e.g., by temporarily wiring two sensors with the same NODE_ID — **test environment only**), the second packet should produce:

```
DROP | uid=3N | reason=duplicate
```

**Alternative observation (normal operation):**
- Confirm that for every sensor TX, the relay logs exactly ONE `FWD` and ONE `ACK` for each unique `uid`.
- No `DROP | reason=duplicate` should appear for legitimately unique packets.

**Pass criteria:**
- [ ] Each unique (src, seq) pair is forwarded exactly once per 30-second window
- [ ] `DROP | reason=duplicate` appears if the same (src, seq) is received again within 30 s
- [ ] After 30 s, the dedup entry expires and the same seq would be accepted again (wrap-around test, optional)

---

### T-14: Edge Drops Duplicate Packets

**Purpose:** Confirm the edge node's dedup table also blocks duplicates independently.

**Setup:** Same as T-13, but observe the edge serial output.

**Expected:**
- Normal: `RX_MESH | src=0x3 | seq=N` once per TX cycle
- If duplicate arrives: `[DROP] Duplicate | src=0x3 seq=N`

**Pass criteria:**
- [ ] No duplicate `AGG` entries from the same (src, seq) pair
- [ ] `[DROP] Duplicate` appears when the same packet is received a second time

---

## Section 6 — CRC and TTL Validation

---

### T-15: TTL Decrement and Drop

**Purpose:** Confirm TTL is decremented at the relay and a packet with TTL=0 is dropped.

**Observation (passive):**
1. Run the full chain (Sensor → Relay → Edge).
2. On the Relay serial, observe `FWD | ... | ttl=9` (TTL decremented from 10 to 9).
3. On the Edge serial, observe `RX_MESH | ... | hops=1` (= DEFAULT_TTL − received_ttl = 10 − 9).

**TTL=0 drop test (requires manual packet injection or very deep chain — optional):**  
Set `DEFAULT_TTL = 1` temporarily in `mesh_protocol.h`, flash sensor and relay, and observe:

```
DROP | uid=3N | reason=ttl_drop
```

on the relay (TTL arrives as 1, relay would decrement to 0 before forwarding — actually drops because `ttl == 0` check happens on received TTL, not after decrement).

> **Important:** TTL is checked on the *received* value. If TTL=0 arrives, it's dropped. The relay decrements TTL *before* forwarding. So a packet with TTL=1 arriving at the relay gets forwarded with TTL=0, which the edge will then drop. Set TTL=2 to allow exactly one relay hop.

**Pass criteria (passive):**
- [ ] Relay logs show `ttl=9` on forwarded packets (started at 10)
- [ ] Edge `hops=1` matches `DEFAULT_TTL - forwarded_ttl = 10 - 9 = 1`

**Pass criteria (active TTL=0 drop):**
- [ ] Relay drops packet with `reason=ttl_drop` when TTL=0 is received

---

### T-16: CRC Failure Drop

**Purpose:** Confirm corrupted packets are discarded.

**Observation (passive):**
- In normal operation, no `DROP | reason=crc_fail` or `[DROP] CRC_FAIL` should appear.
- Their absence over 5 TX cycles confirms the CRC check is not falsely triggering.

**Active test (optional):**
- Temporarily change `LORA_SYNC_WORD` to `0x13` in one node's `mesh_protocol.h` so its packets have a different sync word, causing RadioLib to not even deliver them. Or temporarily corrupt byte 8 (CRC high byte) by XORing it with `0xFF` in `build_mesh_header()`.

**Pass criteria (passive):**
- [ ] Zero `crc_fail` drops appear during 5 clean TX cycles
- [ ] If CRC is deliberately broken: `DROP | reason=crc_fail` appears

---

## Section 7 — Parent Timeout and Recovery

---

### T-17: Sensor Recovers After Parent Disappears

**Purpose:** Confirm a sensor correctly detects a parent timeout and re-selects a new parent.

**Setup:** Sensor 1 with relay as current parent. Edge 1 also running.

**Procedure:**
1. Confirm Sensor 1 has selected relay (0x02) as parent.
2. Power off the relay.
3. Wait `PARENT_TIMEOUT_MS = 35 s` (3+ missed beacon intervals).
4. Observe Sensor 1 serial.

**Expected on Sensor 1:**
```
PARENT | Timeout on 0x2 (35s since last beacon). Re-evaluating...
LISTEN | No parent yet, scanning for beacons...
BCN | src=0x1 | rank=0 | rssi=<value> | queue=0%
PARENT | Selected 0x1 (score=<value>)
```

Then at the next TX cycle:
```
TX | Attempt 1/3 → parent=0x1
ACK | Received from 0x1
TX | node=3 | seq=N | temp=...
```

**Pass criteria:**
- [ ] `PARENT | Timeout` appears within ~35 s of powering off relay
- [ ] Sensor scans and finds edge as fallback
- [ ] Sensor successfully delivers the next TX to Edge 1
- [ ] No crash or indefinite hang

---

### T-18: Relay Recovers After Edge Disappears

**Purpose:** Confirm the relay detects edge timeout and beacons with degraded health.

**Setup:** Relay with Edge 1 as parent.

**Procedure:**
1. Confirm relay has `PARENT | Selected 0x1`.
2. Power off Edge 1.
3. Wait 35 s.

**Expected on Relay:**
```
PARENT | Timeout on 0x1 (35s since last beacon). Re-evaluating...
BCN  | queue=0% | lq=0 | health=0
```

The relay continues beaconing but with `health=0`. If Edge 2 (0x06) is running, it will select that instead:
```
BCN  | src=0x6 | rank=0 | rssi=<value> | queue=0%
PARENT | Selected 0x6 (score=<value>)
BCN  | queue=0% | lq=<value> | health=100
```

**Pass criteria:**
- [ ] `PARENT | Timeout on 0x1` appears within 35 s
- [ ] Relay beacons continue (network stays partially alive)
- [ ] If Edge 2 is present: relay selects it and `health` returns to 100

---

### T-19: Relay Switches to Better Edge (Dual-Edge Failover)

**Purpose:** Confirm the relay selects Edge 2 when Edge 1 is absent, then switches back when Edge 1 returns.

**Setup:** Relay (0x02), Edge 1 (0x01), Edge 2 (0x06) all running.

**Procedure:**
1. Let the relay select a parent (whichever edge scores higher).
2. Power off the currently-selected edge.
3. Wait 35 s. Confirm relay switches to the other edge.
4. Power the first edge back on.
5. Wait 35 s. Confirm the relay only switches back if the score improvement is ≥ 8 (`PARENT_SWITCH_HYSTERESIS`).

**Pass criteria:**
- [ ] After edge goes offline: relay prints `PARENT | Timeout` then `PARENT | Selected 0x<other>`
- [ ] On restoration: relay only prints `PARENT | Switching` if score difference ≥ 8, otherwise stays put

---

## Section 8 — Beacon Content Verification

---

### T-20: Relay Beacon Reflects Queue Load

**Purpose:** Confirm `queue_pct` in the relay beacon increases when packets are being forwarded.

**Setup:** Full system running at 30-second TX interval.

**Procedure:**
1. At rest (no sensors transmitting), observe relay beacon: `queue=0%`.
2. Reduce `TX_INTERVAL_MS` to `5000` in `sensor_node.ino` and reflash both sensors.
3. Observe relay beacon: queue% should rise briefly as packets are being processed.

**Pass criteria:**
- [ ] `queue=0%` when no activity
- [ ] `queue>0%` during burst traffic (hard to observe at 30 s intervals; easier with 5 s interval)

---

### T-21: Edge Beacon Carries Correct Rank and Queue

**Purpose:** Verify edge node beacons always carry rank=0 and queue reflects AGG buffer state.

**Procedure:**
1. Observe Edge 1 serial log over 3 beacon cycles.
2. After several data packets fill the AGG buffer, check if `queue` percentage rises.

**Expected:**
```
[BCN] TX | rank=0 | queue=0%     ← before any data
[BCN] TX | rank=0 | queue=14%    ← after 1 record (1/7 = 14%)
[BCN] TX | rank=0 | queue=28%    ← after 2 records
```

**Pass criteria:**
- [ ] `rank=0` in all edge beacons
- [ ] `queue_pct` = `(agg_count / MAX_AGG_RECORDS) × 100` matches the number of received readings

---

## Section 9 — Edge Aggregation Buffer

---

### T-22: Aggregation Buffer Fills and Resets

**Purpose:** Confirm the edge stores up to 7 records and resets the buffer after LoRaWAN flush (or manually).

**Setup:** LoRaWAN disabled. Edge 1, Relay, both Sensors running.

**Procedure:**
1. Run 4 TX cycles from each sensor (= 8 total readings, more than `MAX_AGG_RECORDS = 7`).
2. Observe edge serial for the "buffer full" warning.

**Expected:**
```
AGG | Added to buffer | count=7/7
[WARN] Aggregation buffer full or invalid payload length
```

The 8th and subsequent readings are logged with the warning but not stored until the buffer is flushed.

> **Note:** With LoRaWAN disabled, the buffer is only flushed when `buffer_flush` or `timeout_flush` conditions are met inside the `#ifdef ENABLE_LORAWAN` block — which won't execute. The buffer will stay full indefinitely. This is expected in mesh-only test mode. To clear it, reset the edge node.

**Pass criteria:**
- [ ] AGG count reaches 7/7 and stops incrementing
- [ ] `[WARN] Aggregation buffer full` appears for records beyond 7
- [ ] Edge continues receiving and ACKing packets even when buffer is full

---

## Section 10 — Packet Format Spot-Check

---

### T-23: Verify Forwarded Packet Header Fields

**Purpose:** Quick sanity check on the exact bytes the relay modifies.

**Observation (via relay serial log `FWD | uid=...`):**

When the relay receives a data packet from sensor 0x03 and forwards it, the header fields must be:

| Field | Before (at sensor) | After (at relay) |
|-------|--------------------|-----------------|
| `flags[0]` | `0x10` (DATA+ACK_REQ) | `0x08` (DATA+FWD, ACK_REQ cleared) |
| `src_id[1]` | `0x03` | `0x03` (unchanged) |
| `dst_id[2]` | `0x02` (relay) | `0x01` (edge) |
| `prev_hop[3]` | `0x03` | `0x02` (relay) |
| `ttl[4]` | `10` | `9` |
| `seq_num[5]` | N | N (unchanged) |
| `rank[6]` | `2` | `2` (unchanged) |
| `payload_len[7]` | `7` | `7` (unchanged) |
| `crc16[8-9]` | CRC of original | Recomputed CRC of modified header |
| Payload bytes | sensor data | sensor data (untouched) |

To verify these without a packet sniffer, add a temporary debug print in `relay_node.ino` inside `forward_packet()` after the header modification:

```cpp
Serial.print("FWD_HDR | flags=0x"); Serial.print(buf[0], HEX);
Serial.print(" src=0x"); Serial.print(buf[1], HEX);
Serial.print(" dst=0x"); Serial.print(buf[2], HEX);
Serial.print(" prev=0x"); Serial.print(buf[3], HEX);
Serial.print(" ttl="); Serial.println(buf[4]);
```

**Pass criteria:**
- [ ] `flags` = `0x08` (FWD set, ACK_REQ cleared)
- [ ] `src_id` unchanged from sensor original
- [ ] `dst_id` = edge node ID
- [ ] `prev_hop` = relay node ID (0x02)
- [ ] TTL decremented by 1
- [ ] Payload bytes beyond offset 9 are identical to what the sensor sent

---

## Test Execution Log

Use this table to record results as you run each test.

| Test | Description | Tester | Date | Result | Notes |
|------|-------------|--------|------|--------|-------|
| T-01 | Edge node boot | | | PASS / FAIL / SKIP | |
| T-02 | Relay node boot | | | | |
| T-03 | Sensor boot, no parent | | | | |
| T-04 | Sensor discovers edge directly | | | | |
| T-05 | Relay discovers edge | | | | |
| T-06 | Sensor discovers relay | | | | |
| T-07 | Parent hysteresis | | | | |
| T-08 | Full two-hop chain | | | | |
| T-09 | Payload integrity | | | | |
| T-10 | Both sensors concurrent | | | | |
| T-11 | Retry on no ACK | | | | |
| T-12 | ACK is hop-by-hop | | | | |
| T-13 | Relay dedup | | | | |
| T-14 | Edge dedup | | | | |
| T-15 | TTL decrement and drop | | | | |
| T-16 | CRC failure drop | | | | |
| T-17 | Sensor recovers after parent loss | | | | |
| T-18 | Relay recovers after edge loss | | | | |
| T-19 | Dual-edge failover | | | | |
| T-20 | Relay beacon queue_pct | | | | |
| T-21 | Edge beacon rank and queue | | | | |
| T-22 | AGG buffer fills and resets | | | | |
| T-23 | Forwarded packet header fields | | | | |

---

## Recommended Test Order

For a first-time bring-up, run tests in this order:

1. **T-01, T-02, T-03** — Bring up each node individually. Fix any hardware/init issues first.
2. **T-04, T-05** — Verify beacon reception and parent selection basics.
3. **T-08** — Full chain. This is the most important end-to-end test.
4. **T-09** — Payload integrity. Quick to run after T-08.
5. **T-10** — Dual-sensor test.
6. **T-11, T-12** — ACK/retry behaviour.
7. **T-13, T-14** — Deduplication.
8. **T-17, T-18** — Recovery. Only run these once the happy path is solid.
9. Remaining tests as time allows.

---

## Key Timing Reference

| Constant | Value | Where defined |
|----------|-------|---------------|
| `BEACON_INTERVAL_MS` | 10 000 ms | `mesh_protocol.h` |
| `PARENT_TIMEOUT_MS` | 35 000 ms | `mesh_protocol.h` |
| `TX_INTERVAL_MS` | 30 000 ms | `sensor_node.ino` |
| `ACK_TIMEOUT_MS` | 600 ms | `mesh_protocol.h` |
| `MAX_RETRIES` | 3 | `mesh_protocol.h` |
| `DEDUP_WINDOW_MS` | 30 000 ms | `mesh_protocol.h` |
| `PARENT_SWITCH_HYSTERESIS` | 8 points | `mesh_protocol.h` |
| `MAX_AGG_RECORDS` | 7 | `mesh_protocol.h` |
| Boot phase offset formula | `(NODE_ID % 5) × 2000 ms` | each `.ino` / `config.h` |
