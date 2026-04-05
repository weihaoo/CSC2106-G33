# CSC2106 Group 33 — LoRa Mesh-to-LoRaWAN Gateway Network

Singapore Institute of Technology

## Abstract

This project implements a custom multi-hop LoRa mesh network designed for deep tunnel infrastructure monitoring. Sensor nodes collect DHT22 temperature and humidity data and route it through a DAG-based gradient mesh (with multi-parent load balancing and hop-by-hop ARQ) to edge nodes, which aggregate readings and uplink to The Things Network (TTN) via LoRaWAN OTAA. A Raspberry Pi dashboard subscribes to TTN over MQTT for real-time visualization. The entire system runs on LilyGo T-Beam boards with SX1262 radios in the AS923 frequency band.

**Firmware version: v1.4**

---

## 1. Network Architecture

### Topology

```
Sensor-03 ──┐                    ┌── Edge-01 (primary) ──┐
             ├── Relay-02 ───────┤                        ├── RAK WisGate ── TTN (AS923)
Sensor-04 ──┘                    └── Edge-06 (standby) ──┘                                            │
                                                                                                 RPi Dashboard
```

### Hardware

| Component | Model | Role |
|-----------|-------|------|
| LilyGo T-Beam v1.2 | SX1262 radio + AXP2101 PMU + ESP32 | All mesh nodes (sensor, relay, edge) |
| DHT22 | Temperature/humidity sensor | Sensor nodes (GPIO 4) |
| RAK WisGate D4H | LoRaWAN gateway | Bridge between mesh and TTN |
| Raspberry Pi Pico W / Pi 4B | MicroPython / Python Flask | Dashboard display |
| Laptop (Windows) | pyserial + matplotlib | Serial capture and metrics analysis |

### Radio Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Frequency | 920.8 MHz | AS923 band, offset from default to avoid interference |
| Spreading Factor | SF7 | Balance between range and airtime |
| Bandwidth | 125 kHz | Standard LoRa bandwidth |
| Coding Rate | 4/5 | Error correction |
| Sync Word | 0x33 | Group 33 unique (default 0x12 conflicts with neighboring groups) |
| TX Power | 14 dBm | Multi-building deployment level |

---

## 2. Key Technical Features

### 2.1 DAG Multi-Parent Routing

Relay nodes maintain a set of multiple active parents (not a single parent) and select a parent per-packet using weighted random selection. Parent scoring uses three weighted factors:

- **55% rank** — proximity to edge (rank 0 = edge, 1 = relay, 2 = sensor)
- **30% RSSI** — signal quality (mapped from [-120, -60] dBm to [0, 100])
- **15% queue depth** — TX queue occupancy (lower is better)

Additional mechanisms: queue penalty (80%+ occupancy reduces score), no-route penalty (parent_health=0 scores zero), hysteresis (8-point minimum improvement to switch parent), and strikes system (2 consecutive failures invalidate a parent).

### 2.2 Listen-Before-Talk (LBT)

Every mesh transmission performs a Channel Activity Detection (CAD) scan before sending. If the channel is busy, exponential backoff is applied (100ms base, doubles each retry, plus 0-50ms random jitter). After 3 failed CAD attempts, the packet is transmitted anyway to avoid permanent drops.

### 2.3 Hop-by-Hop ACK with Stop-and-Wait ARQ

Each hop acknowledges receipt with a 10-byte ACK packet. The sender waits up to 1500ms for the ACK (accounting for LBT delays) and retries up to 3 times. After 2 consecutive failure rounds, the parent is invalidated and a new parent is selected.

### 2.4 CRC-16 CCITT Integrity

All mesh headers include a CRC-16 CCITT checksum (polynomial 0x1021, initial value 0xFFFF) computed over header bytes 0-7 and stored big-endian in bytes 8-9. This ensures identical byte layout on all platforms regardless of CPU endianness.

### 2.5 Deduplication

A 16-entry circular buffer tracks recently seen packets keyed on (src_id, seq_num) with a 30-second expiry window. Duplicate packets are discarded but re-ACKed to prevent sender retry loops.

### 2.6 Payload-Agnostic Relay Forwarding

Relay nodes never read beyond byte 9 of the MeshHeader. They forward any payload type without parsing it, enabling the mesh to carry any sensor data format without relay firmware changes.

### 2.7 BridgeAggV1 Aggregation

The edge node buffers incoming sensor readings in an aggregation structure (up to 7 records per LoRaWAN frame). Flush is triggered when the buffer is full OR 30 seconds have elapsed. Each record is 14 bytes: mesh source ID, sequence number, sensor type hint, hop estimate, edge uptime, payload length, and 7 raw payload bytes.

### 2.8 Beacon System

Nodes broadcast beacons every 10 seconds with phase offsets (NODE_ID % 5 * 2000ms) to avoid collisions. Beacons carry rank, queue occupancy, link quality, and parent health. Relay nodes suppress beacons when orphaned (no active parents) to prevent attracting children into a black hole.

### 2.9 Sensor-as-Relay

Sensor nodes also function as relays — they forward DATA packets from downstream nodes while originating their own sensor readings. This enables deeper mesh topologies beyond the simple sensor-relay-edge chain.

### 2.10 Radio Time-Sharing

The edge node shares a single SX1262 radio between mesh reception (default state) and LoRaWAN transmission (temporary ~5s during uplink). The radio is reconfigured between modes (mesh uses sync word 0x33, LoRaWAN uses 0x34).

---

## 3. Packet Formats

### 3.1 MeshHeader (10 bytes)

| Byte | Field | Description |
|------|-------|-------------|
| 0 | flags | Packet type (bits 7-5) + flags (bits 4-0) |
| 1 | src_id | Original sender node ID |
| 2 | dst_id | Destination node ID (0xFF = broadcast) |
| 3 | prev_hop | Node ID of the last hop |
| 4 | ttl | Time to live, decremented each hop |
| 5 | seq_num | Sequence number from original sender |
| 6 | rank | Sender's rank (0=edge, 1=relay, 2=sensor) |
| 7 | payload_len | Bytes following this header |
| 8-9 | crc16 | CRC-16 CCITT high/low byte (big-endian) |

### 3.2 Packet Type Flags

| Type | Value | Description |
|------|-------|-------------|
| DATA | 0x00 | Sensor data packet |
| BEACON | 0x20 | Beacon packet (parent discovery) |
| ACK | 0x40 | Acknowledgment packet |
| ACK_REQ | 0x10 | Bit 4: ACK requested by sender |
| FWD | 0x08 | Bit 3: Packet has been forwarded |

### 3.3 SensorPayload (7 bytes)

| Byte | Field | Description |
|------|-------|-------------|
| 0 | schema_version | 0x01 |
| 1 | sensor_type | 0x03 = DHT22 |
| 2-3 | temp_c_x10 | Temperature x10, signed int16, big-endian |
| 4-5 | humidity_x10 | Humidity x10, unsigned int16, big-endian |
| 6 | status | 0x00 = OK, 0x01 = read error |

### 3.4 BeaconPayload (4 bytes)

| Byte | Field | Description |
|------|-------|-------------|
| 0 | schema_version | 0x01 |
| 1 | queue_pct | TX queue occupancy 0-100% |
| 2 | link_quality | RSSI-derived uplink quality 0-100 |
| 3 | parent_health | ACK success rate to parent 0-100% |

### 3.5 BridgeAggV1 Format

**Header (3 bytes):** schema_version (0x02), bridge_id, record_count

**Each record (14 bytes):** mesh_src_id, mesh_seq, sensor_type_hint, hop_estimate, edge_uptime_s (uint16 BE), opaque_len, opaque_payload[7]

---

## 4. Repository Structure

```
CSC2106-G33/
├── edge_node/
│   ├── edge_node.ino          # Edge firmware — mesh sink + LoRaWAN uplink
│   ├── config.h               # Node ID, TTN credentials, pin definitions
│   ├── edge_lorawan.h         # LoRaWAN OTAA join + uplink logic
│   └── edge_packets.h         # Mesh RX, validation, ACK, aggregation
├── relay_node/
│   ├── relay_node.ino         # Relay firmware — DAG multi-parent forwarder
│   ├── relay_packets.h        # Packet RX, validation, forward-then-ACK
│   └── relay_routing.h        # DAG parent set management, weighted selection
├── sensor_node/
│   ├── sensor_node.ino        # Sensor firmware — DHT22 reading + relay
│   ├── sensor_packets.h       # Packet RX, relay forwarding, ACK handling
│   └── sensor_routing.h       # Single-parent selection with hysteresis
├── shared/
│   ├── mesh_protocol.h        # Protocol constants, structs, CRC, LBT
│   ├── mesh_common.h          # Dedup, ACK, parent scoring, beacon processing
│   ├── mesh_radio.h           # SX1262 + AXP2101 PMU initialization
│   └── logging.h              # Structured logging with timestamps
├── dashboard/
│   └── main.py                # Pico W MicroPython: MQTT + LCD + HTTP API
├── tools/
│   ├── decoder.js             # TTN payload decoder (BridgeAggV1 to JSON)
│   └── mock_ttn.py            # Mock MQTT publisher for testing without hardware
├── .docs/
│   ├── ARCHITECTURE.md        # DAG topology and packet flow diagrams
│   ├── FIRMWARE_REFERENCE.md  # Detailed reference for all node types
│   ├── IMPLEMENTATION_PLAN_SIMPLE.md  # Full build guide with team assignments
│   ├── MESH_TEST_PLAN.md      # 23 mesh-only test cases (T-01 to T-23)
│   ├── Algo.md                # Algorithm analysis (scoring, routing)
│   ├── QUICKSTART.md          # Quick start for flashing edge node
│   ├── SETTINGUPTTN.md        # TTN and WisGate gateway setup guide
│   └── README.md              # Edge node setup guide (Person 3)
├── logs/
│   └── scenario_6_failure.log # Node failure/recovery test log
└── libraries/                 # Arduino library dependencies
```

---

## 5. Development Timeline and Attempts

### Phase 1: Initial Prototyping (14 Feb 2026)

**What was attempted:** First LoRa communication tests and initial mesh protocol design.

- Adam created the repository and README, wrote the first LoRa P2P transmitter test (`lora_transmitter_test.ino`) to verify basic radio communication between two T-Beams.
- Ming Wei uploaded an initial MeshNode prototype with `MeshPackets.h`, `NeighborTable.h`, and `NodeConfig.h` — an early mesh protocol design featuring neighbor tables for route discovery.
- Premi uploaded a separate `sketch_feb14a.ino` implementation, which immediately caused git conflicts with Ming Wei's code. The `MeshNode` folder was deleted to resolve conflicts, and conflict markers were manually removed.
- Ming Wei re-uploaded files via the GitHub web UI (two "Add files via upload" commits).
- Reynard deleted duplicate files, then spent three commits fixing a bug where nodes were transmitting multiple times before the first send completed ("Fix the issue of multiple transmissions before sending", "fix again pls work", "push for this fix").
- Premi tuned parameters: changed data interval, route stable time, minimum threshold, and spreading factor.
- Adam attempted to fix forwarding to the sink node ("Trying to fix forwarding issue to sink node").

**Problems encountered:** Git conflicts from parallel uploads by multiple team members unfamiliar with collaborative workflows. Duplicate files across different folders. Forwarding to the sink node was unreliable. The multi-transmission bug caused channel congestion.

**Outcome:** Basic P2P LoRa communication was working but the mesh was unreliable. The initial `sketch_feb14a/` and `new_folder/MeshNode/` architectures were both abandoned in favor of a cleaner structure.

**Abandoned:** The entire `sketch_feb14a/` directory and `new_folder/MeshNode/` folder were replaced during the restructuring in Phase 2.

---

### Phase 2: Project Restructuring (19-20 Feb 2026)

**What was attempted:** Clean up the codebase structure and formalize the implementation plan.

- Wei Hao restructured all files from the flat sketch format into modular `edge_node/`, `relay_node/`, and `sensor_node/` folders, each with their own `mesh_protocol.h` and `.ino` files.
- Wei Hao drafted Implementation Plan v4 (`IMPLEMENTATION_PLAN_v4.md`), then updated it with more detail.
- Ming Wei refined the strategy and decision wording in the implementation plan.
- Reynard made three small patches/changes (unspecified content in commit messages).
- `.gitignore` and `SETTINGUPTTN.md` were added.

**Outcome:** A clean, modular project structure was established. Team roles were formalized in the implementation plan. The project gained its definitive folder structure that persists to this day.

---

### Phase 3: Core Firmware Build (28 Feb 2026)

**What was attempted:** Build working firmware for all three node types in parallel.

- **Wei Hao** wrote the edge node firmware with mesh reception, aggregation buffer, and LoRaWAN uplink stub. Created `edge_node/config.h`, `edge_node/QUICKSTART.md`, and `edge_node/README.md`.
- **Premi** built the dashboard (`dashboard/main.py`), TTN payload decoder (`tools/decoder.js`), and mock MQTT publisher (`tools/mock_ttn.py`) for testing the dashboard without real hardware.
- **Ming Wei** created the sensor and relay node firmware (`sensor_node/sensor_node.ino`), then iterated to integrate with Adam's edge work ("Sensor + Relay nodes + Edge nodes for adam to integrate"). Tested from the "antigravity" lab.
- **Adam** established the canonical shared `mesh_protocol.h` header across all nodes, fixed a CRC endianness bug (CRC was platform-dependent byte order), fixed an ACK type mismatch (relay was sending the wrong packet type flag for ACKs), fixed radio standby bugs (radio not returning to RX mode after TX), and fixed a relay ACK leak (relay was ACKing packets it could not actually forward).
- The implementation plan went through several iterations — removed, re-added, and simplified into `IMPLEMENTATION_PLAN_SIMPLE.md`.
- Multiple merge commits were created due to parallel development by the entire team.

**Problems encountered:** The CRC endianness bug caused packets to fail validation on boards with different byte orders. The ACK type mismatch caused sensor nodes to not recognize ACKs from relays, triggering unnecessary retries. The radio standby bug meant nodes stopped receiving after transmitting. The relay ACK leak caused silent data loss — the relay would ACK a packet to the sender but fail to forward it to the edge.

**Outcome:** All three node types were functional. Dashboard and TTN tools existed. Critical protocol-level bugs were fixed.

---

### Phase 4: Protocol Hardening (4-5 Mar 2026)

**What was attempted:** Harden the mesh protocol and enable independent testing.

- **Adam** added `#ifdef ENABLE_LORAWAN` guards so the mesh could be tested without needing LoRaWAN infrastructure or TTN credentials. Documented the mesh algorithm analysis (`Algo.md`). Created `FIRMWARE_REFERENCE.md` (706 lines of detailed reference). Created `MESH_TEST_PLAN.md` with 23 test cases (T-01 through T-23). Applied TX phase offset at boot to prevent simultaneous transmissions.
- **Ming Wei** made sensor nodes also function as relays ("Implemented sensor node as a relay too!"), enabling deeper mesh topologies. Fixed the `queue_pct` calculation bug.
- **Wei Hao** added destination ID filtering (nodes were processing packets not addressed to them) and queue-based load balancing.

**Outcome:** The mesh could now be tested independently of LoRaWAN. Sensor-as-relay capability was added. A formal test plan with 23 test cases was established.

---

### Phase 5: DAG Routing Architecture (10 Mar 2026) — MAJOR ARCHITECTURAL PIVOT

**What was attempted:** Replace simple single-parent forwarding with DAG-based gradient routing with multi-parent support.

Wei Hao made 8 commits in a single day, completely rearchitecting the mesh routing:

1. Modified folder structure to support shared headers across nodes.
2. Added DAG configuration constants: `PARENT_THRESHOLD_SCORE`, `MAX_ACTIVE_PARENTS`, `MIN_PARENT_RSSI`, `MAX_PARENT_STRIKES`, and score weights (55/30/15).
3. Implemented multi-parent routing for relay nodes — a parent SET instead of a single parent, with weighted random selection per-packet.
4. Integrated structured logging module across all nodes (`shared/logging.h`).
5. Added shared logging module with timestamped, role-prefixed output for team-friendly debugging.
6. Documented the DAG architecture (`ARCHITECTURE.md`) with topology and packet flow diagrams.
7. Fixed mesh protocol synchronization, self-healing (stale parent eviction every 5 seconds), and reliable forwarding (validate forwarding path BEFORE ACKing sender to prevent silent data loss).
8. Changed radio frequency to 920.8 MHz (AS923 offset to avoid interference from neighboring groups) and sync word from 0x12 to 0x33 (Group 33 unique). Added buffer drain logic when LoRaWAN is disabled.

Reference materials were added: SX1262 datasheet PDF and wireless mesh lecture PDF to `.docs/`.

**Why the pivot:** The original simple single-parent forwarding had no redundancy — if the one parent died, data was lost until a new parent was found. DAG multi-parent routing provides load balancing and automatic failover. Additionally, the simple forwarding did not account for congestion — a node might forward to a parent whose aggregation buffer was already full.

**Outcome:** The mesh was upgraded from simple forwarding to gradient-based DAG routing with multi-parent support, self-healing, and reliable forwarding.

---

### Phase 6: LoRaWAN Integration (11 Mar 2026)

**What was attempted:** Complete the full data pipeline from sensor to TTN.

- **Adam** added a `#define SIMULATION_MODE` for easy swapping between real DHT22 and random simulated sensor data. Fixed LoRaWAN setup configuration.
- **Wei Hao** made the edge node payload-agnostic (it no longer assumes DHT22 — copies raw payload bytes into BridgeRecord, enabling any sensor type).
- **Premi** debugged the LoRaWAN pipeline over multiple commits:
  1. Updated `edge_node.ino` to ensure mesh initialization happens AFTER LoRaWAN connection (ordering matters because LoRaWAN join uses different radio parameters).
  2. Fixed SX1262 hardware initialization — must call `radio.begin()` before `beginOTAA()` (a RadioLib requirement).
  3. Fixed OTAA join for RadioLib 7.x — requires both `nwkKey` and `appKey` parameters (LoRaWAN 1.0.x uses the same key for both).
  4. Achieved the full pipeline: sensor reads DHT22, mesh forwards through relay, edge aggregates, LoRaWAN uplink, TTN receives data.

**Problems encountered:** Radio initialization order was critical — LoRaWAN join uses sync word 0x34 while the mesh uses 0x33. The radio had to join first, then be reconfigured for mesh. RadioLib 7.x had an API change that broke the OTAA join call (required an additional key parameter).

**Outcome:** End-to-end pipeline was working. Data was visible on the TTN console. The `feat/dynamic_node_id` branch was created here (later fully merged into main).

---

### Phase 7: ACK Reliability Fixes (20 Mar 2026) — Pull Request #1

**What was attempted:** Fix a critical reliability bug causing infinite retry loops.

- **Premi** discovered that when a node received a duplicate packet (already in the dedup table), it silently dropped it. But the sender did not know — it had not received an ACK for the original transmission, so it retried. The retry was also dropped as a duplicate. This caused endless retry loops that wasted channel time and battery.
- **Fix:** Re-ACK duplicate packets — send an ACK even though the data is discarded, so the sender knows the packet was received.
- **Premi** also fixed: the parent's `last_seen_ms` was only refreshed on beacon reception. If a parent sent ACKs but no beacons (e.g., because it was busy forwarding), it would be declared dead. **Fix:** refresh `last_seen_ms` on ACK receipt as well.
- **Premi** adjusted health and rank scoring parameters.
- **Adam** registered Edge 01 DevEUI and AppKey on TTN.
- PR #1 was merged by Adam — the only formal pull request in the project.

**Outcome:** Reliability was significantly improved. No more infinite retry loops on duplicate packets. Parent tracking became more robust.

---

### Phase 8: Intensive Bug Fixing (28 Mar 2026)

**What was attempted:** Resolve accumulated protocol-level bugs discovered during physical testing.

- **Wei Hao** fixed an `rxFlag` race condition — the ISR could set `rxFlag = true` while the main loop was in the middle of processing a previous packet, causing the flag to be cleared before the new packet was read. Also added parent_health scoring to the parent selection algorithm. Synced TX power to 14 dBm across all nodes (the sensor was using a different value). Removed duplicate default arguments from the `score_parent()` function definition.
- **Reynard** made an "adjusted fix" (unspecified content).
- **Premi** made a broad "Fix: all existing issues" commit fixing multiple accumulated bugs.
- **Adam** made a "fix: fix mesh" commit (broad debugging session).

**Outcome:** Multiple protocol-level bugs were resolved after a concentrated physical testing session.

---

### Phase 9: Codebase Modularization (29 Mar 2026)

**What was attempted:** Split monolithic `.ino` files into focused modular headers for maintainability.

- **Wei Hao** split the monolithic node files into focused headers:
  - `edge_node/edge_lorawan.h` — LoRaWAN OTAA join and uplink logic
  - `edge_node/edge_packets.h` — mesh packet reception, validation, ACK, aggregation
  - `relay_node/relay_packets.h` — packet RX, validation, forward-then-ACK pattern
  - `relay_node/relay_routing.h` — DAG parent set management, weighted random selection
  - `sensor_node/sensor_packets.h` — packet RX, relay forwarding, ACK handling
  - `sensor_node/sensor_routing.h` — single-parent selection with hysteresis
  - `shared/mesh_common.h` — dedup, ACK, parent scoring, beacon processing (shared by sensor and relay)
  - `shared/mesh_radio.h` — SX1262 + AXP2101 PMU initialization (shared by sensor and relay)
- Fixed 6 DAG routing bugs in one commit.

**Attempted but abandoned — DEMO_MODE:** Created `shared/demo_packets.h` for generating realistic serial output without physical hardware, to demonstrate the system in a presentation. Created on branch `feature/demo-mode` with 1 commit. **Never merged** — the team decided to demo with real hardware instead.

**Outcome:** Clean modular codebase. Each concern is in its own file. Demo mode was abandoned.

---

### Phase 10: Routing Refinement and Stabilization (30 Mar 2026)

**What was attempted:** Fine-tune routing quality and stabilize the main branch.

**Wei Hao** iterated rapidly on routing quality with 6 commits:

1. Adjusted RSSI values based on field testing.
2. Disabled LoRaWAN for mesh testing, fixed relay compilation order.
3. **Attempted — then immediately reverted:** Added hardcoded parent whitelists — forcing specific nodes to only accept specific parents (e.g., Sensor-03 can only use Relay-02 as parent). Lowered RSSI floor from -75 to -110 dBm for multi-building deployment. Declared firmware version v1.4.
4. **Immediately reverted:** Removed hardcoded parent whitelists in the very next commit to enable dynamic gradient routing. Commit message: "Refactor: Remove hardcoded parent whitelists to enable dynamic gradient routing." The whitelists were too rigid for production use but the concept was useful for controlled testing.
5. Fixed beacon acceptance — sensor nodes were rejecting beacons from their current parent if the parent's rank changed, causing unnecessary parent switches. Discovered during Test 3 of the test plan.
6. Reduced aggregation flush timeout from 4 minutes to 30 seconds — the old timeout caused stale queue scores (edge reported 100% queue for minutes after receiving data).

**Outcome:** Main branch stabilized at commit `6f5876c`. This is the current HEAD of main. Firmware v1.4 declared.

---

### Phase 11: Metrics — Three Competing Approaches (2-3 Apr 2026)

The team needed metrics (latency, PDR, throughput, hop count) for the project report. Three independent approaches were attempted in parallel:

**Approach A — `feature/mesh-testing-metrics` branch (Wei Hao, 1 commit):**
- Created `shared/mesh_metrics.h` with firmware-level instrumentation.
- Tracked PDR, orphan events, and protocol events directly in the firmware.
- Created `TESTING.md` and `mesh_logs.md`.
- **Abandoned:** Firmware-level metrics added code bloat and could not be reprocessed after the fact. Also risked affecting timing-sensitive radio operations.

**Approach B — `feature/metrics-latency-throughput-pdr-loss-hopcount` branch (Reynard + Ming Wei, 3 commits):**
- Ming Wei added NTP time synchronization to sensor and edge nodes (`shared/ntp_time.h`).
- Reynard added `edge_node/edge_metrics.h` with runtime metrics counters.
- Reynard modified `tools/decoder.js` to surface metrics in the TTN dashboard.
- **Abandoned:** Metrics were tied to the TTN uplink path, which required working LoRaWAN. Could not collect metrics during mesh-only testing.

**Approach C — `latency` branch (Adam + Ming Wei, 19 commits) — ADOPTED:**
- Built external Python tools: `tools/serial_capture.py` (192 lines) for automated log collection via USB serial, `tools/metrics_analysis.py` (998 lines) for offline analysis generating charts and CSVs.
- Kept NTP timestamps from Approach B (sensor embeds send timestamp, edge computes one-way latency).
- Created comprehensive testing scenarios documentation (`.docs/TESTING_SCENARIOS_METRICS.md`, 560 lines).
- Rewrote dashboard from MicroPython (Pico W) to Python/Flask (Raspberry Pi), adding real-time metrics display, packet history dropdowns, and seamless JavaScript live updates.
- **Why it won:** Did not bloat firmware, could reprocess logs offline, generated publication-quality matplotlib charts, worked with or without LoRaWAN.

---

### Phase 12: Testing and Results Collection (3-5 Apr 2026)

**What was attempted:** Execute planned test scenarios and collect quantitative results.

- Ran Scenario 1 (baseline) and Scenario 2 (TX interval stress test) on the `latency` branch.
- Collected 4 log files via `serial_capture.py` from edge node COM16 at 115200 baud.
- Generated charts and CSVs in the `report/` directory.
- Fixed metrics calculation bugs: warmup period for throughput (initial spike), corrected packet loss formula, added per-hop comparison analysis.
- Added parent whitelist feature back on the `latency` branch (for controlled topology testing only).
- 3 git stashes on the branch indicate ongoing work-in-progress (node ID and whitelist tweaks between test runs).

**Scenarios not yet completed:**
- Scenario 3: Multi-hop chain (requires `PARENT_WHITELIST` to force topology)
- Scenario 4: Distance/signal stress (marked SKIP — requires outdoor space)
- Scenario 5: Burst traffic (marked OPTIONAL — requires firmware modification)
- Scenario 6: Node failure/recovery (log file exists on main branch, not yet analyzed)
- Scenario 7: All nodes active (load and scalability test)

---

## 6. Test Results and Analysis

### 6.1 Scenario 1 — Baseline Performance

**Configuration:** 5-second TX interval, approximately 5 minutes duration, 2 sensor nodes (0x03 and 0x04), direct to edge (no relay hop).

| Metric | Value |
|--------|-------|
| Total packets received | 104 |
| Packets dropped | 3 |
| Unique source nodes | 2 (0x03, 0x04) |
| Overall PDR | 88.0% |
| Latency min | 839 ms |
| Latency max | 6,961 ms |
| Latency mean | 2,794.7 ms |
| Latency median | 2,565.0 ms |
| Latency std dev | 1,422.8 ms |
| Hop count | All 0 (direct to edge) |

**Analysis:** In ideal conditions with low traffic, the mesh achieves 88% PDR with sub-3-second average latency. All packets arrived via direct path (0 hops through relay), indicating the sensor nodes were within range of the edge node. The standard deviation of 1,423 ms suggests moderate variability, likely caused by LBT backoffs and occasional ACK retries. The maximum latency of 6.96 seconds is approximately 2.5x the mean, indicating occasional congestion events.

### 6.2 Scenario 2 — TX Interval Stress Test

**Configuration:** 2 sensor nodes, three sub-tests at decreasing TX intervals.

| Metric | 2s interval | 1s interval | 500ms interval |
|--------|-------------|-------------|----------------|
| Total packets | 161 | 161 | 112 |
| Drops | 3 | 12 | 9 |
| Overall PDR | 78.9% | 78.9% | 81.7% |
| Latency min (ms) | 746 | 162 | 622 |
| Latency max (ms) | 6,207 | 48,865 | 35,010 |
| Latency mean (ms) | 2,522.7 | 4,680.5 | 4,941.6 |
| Latency median (ms) | 2,408.0 | 2,394.0 | 2,700.5 |
| Latency std dev (ms) | 950.3 | 7,295.4 | 5,096.7 |
| Hop count range | All 0 | 0-1 (mean 0.4) | 0-1 (mean 0.4) |

**Analysis:** As the TX interval decreases from 2 seconds to 500 milliseconds, latency variance explodes dramatically — the standard deviation increases from 950 ms at 2s to 7,295 ms at 1s interval. Extreme tail latencies appear: 48.9 seconds at 1s interval and 35.0 seconds at 500ms interval. This indicates channel congestion: LBT backoffs cascade, ACK timeouts trigger retries, and retries compound the congestion further.

PDR drops to approximately 79% under stress, suggesting the network saturates below 2-second TX intervals. Notably, the median latency remains relatively stable (2,408-2,701 ms across all three tests), meaning most packets still get through quickly — but a significant minority experience severe delays. This bimodal distribution is characteristic of contention-based MAC protocols under load.

At 1s and 500ms intervals, some packets began routing through the relay (hop count 0-1, mean 0.4), suggesting that direct links to the edge became congested, causing the DAG routing to favor the relay path for some packets.

---

## 7. Known Issues and Limitations

1. **LoRaWAN disabled on main branch** — `#define ENABLE_LORAWAN` is commented out in `edge_node.ino:40`. Edge 1 (0x01) TTN credentials are placeholder values in `config.h:31-38` and must be registered on TTN before production use.

2. **Documentation parameter discrepancies** — at least 11 values in `.docs/` files do not match actual code values: frequency (docs say 923.0 MHz, code uses 920.8 MHz), sync word (docs say 0x12, code uses 0x33), TX power (docs say 17 dBm, code uses 14 dBm), ACK timeout (docs say 600 ms, code uses 1500 ms), aggregation flush timeout (docs say 240,000 ms, code uses 30,000 ms), score weights (docs say 60/25/15, code uses 55/30/15), Edge 2 NODE_ID (docs say 0x05, code uses 0x06), TTN cluster (docs say as1, code uses au1), TTN App ID (docs say csc2106-g33-mesh, code uses sit-csc2106-g33), DHT pin (docs say GPIO 13, code uses GPIO 4), TX interval (docs say 30s dev / 120s final, code uses 5s).

3. **TX interval in testing mode** — currently set to 5 seconds in `sensor_node.ino:33`. The comment indicates this should be 30s during development and 120s for final deployment.

4. **Incomplete test coverage** — only 2 of 7 planned test scenarios have been completed. No multi-hop chain, distance/signal, burst traffic, or full-load tests have been executed.

5. **Hardcoded node identifiers** — NODE_ID, NODE_NAME, and TTN credentials must be manually changed in source code before flashing each board. No runtime configuration mechanism exists.

6. **No outdoor or distance testing** — Scenario 4 was marked SKIP because it requires outdoor space. All testing has been conducted indoors.

7. **`latency` branch not merged to main** — 19 commits of metrics tools, dashboard rewrite, and test results exist only on the `latency` branch and have not been integrated into the main branch.

8. **Single relay node** — only one relay (0x02) was tested. Multi-relay mesh topologies were not validated.

---

## 8. Team Contributions

Based on git commit history (82 commits on main, approximately 100 total across all branches):

| Member | Commits | Primary Contributions |
|--------|---------|----------------------|
| **Wei Hao** | ~30 | Project architecture and restructuring. DAG multi-parent routing design and implementation. Codebase modularization into header files. Radio configuration (frequency, sync word, TX power). Bug fixes: rxFlag race condition, parent health scoring, beacon acceptance, stale queue scores. 6 DAG routing bug fixes in one commit. Implementation plan. |
| **Adam (boopydoo)** | ~34 | Protocol-level bug fixes (CRC endianness, ACK type mismatch, radio standby, relay ACK leak). Canonical `mesh_protocol.h` establishment. Documentation (FIRMWARE_REFERENCE, MESH_TEST_PLAN, ARCHITECTURE, Algo analysis). Metrics analysis pipeline (`metrics_analysis.py`, `serial_capture.py`). Dashboard rewrite (Flask/Raspberry Pi). LoRaWAN setup and TTN device registration. |
| **Kwong Ming Wei** | ~21 | Initial sensor and relay node firmware. Sensor-as-relay capability. DHT22 integration. NTP time synchronization for latency measurement. Physical hardware testing. Test log collection. Test scenario execution. |
| **Premi (Premierlin)** | ~15 | LoRaWAN OTAA pipeline — SX1262 init ordering, RadioLib 7.x compatibility, full sensor-to-TTN pipeline. ACK reliability (re-ACK duplicates, parent last_seen refresh). Dashboard prototype. TTN decoder. Parameter tuning. |
| **Reynard (ReynardTSW)** | ~10 | Early transmission bug fixes (multiple TX before send). Edge metrics header. TTN decoder updates for metrics. |

---

## 9. How to Build and Flash

### Requirements

- **Hardware:** LilyGo T-Beam v1.2 boards (one per node), DHT22 sensor (for sensor nodes), LoRa antenna (CRITICAL — transmitting without an antenna will damage the SX1262 radio)
- **Software:** Arduino IDE or PlatformIO
- **Libraries:** RadioLib, XPowersLib, DHT (install via Arduino Library Manager)

### Flashing Instructions

1. **Before flashing each board,** edit the appropriate `.ino` file to set the correct `NODE_ID` and `NODE_NAME`:
   - Sensor 1: `NODE_ID = 0x03`, `NODE_NAME = "SENSOR-03"`
   - Sensor 2: `NODE_ID = 0x04`, `NODE_NAME = "SENSOR-04"`
   - Relay: `NODE_ID = 0x02`, `NODE_NAME = "RELAY-02"`
   - Edge 1: `NODE_ID = 0x01`, `NODE_NAME = "EDGE-01"` (in `config.h`)
   - Edge 2: `NODE_ID = 0x06`, `NODE_NAME = "EDGE-06"` (in `config.h`)

2. **For edge nodes,** edit `edge_node/config.h` to set valid TTN OTAA credentials (DevEUI, JoinEUI, AppKey) if LoRaWAN uplink is required.

3. **To enable LoRaWAN,** uncomment `#define ENABLE_LORAWAN` in `edge_node.ino:40`.

4. Flash the appropriate `.ino` file to each board using Arduino IDE or PlatformIO.

5. Open Serial Monitor at 115200 baud to verify boot messages.

### TTN Setup

See `.docs/SETTINGUPTTN.md` for detailed TTN and WisGate gateway setup instructions.

### Testing Without Hardware

Run `python tools/mock_ttn.py` to publish fake TTN payloads to `test.mosquitto.org` for testing the dashboard without real hardware.
