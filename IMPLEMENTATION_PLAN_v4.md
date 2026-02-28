# Detailed Implementation Spec v7: T-Beam Sensor/Relay -> Dual-Role Edge Bridge -> WisGate -> TTN -> Pico W (Singapore AS923)

> **Revision notes (v7):** Topology is now fixed to **5 nodes, all LILYGO T-Beam Meshtastic + 923MHz antenna**: 2 sensor nodes, 1 relay node, and 2 edge bridge nodes. Both edge bridges are **dual-role single-board nodes** (mesh sink + LoRaWAN end device). The previous two-board UART edge split is removed. Single-radio arbitration policy is added so each edge alternates between mesh listen and bounded LoRaWAN TX/RX windows.

---

## Brief Summary
 
 > **SIMULATION DISCLAIMER:** This entire implementation is designed for an **above-ground lab simulation** of the underground scenario. All "underground" references (tunnels, shafts) refer to the *target* use case, but the actual code and hardware will be tested in a classroom/lab environment. All radio testing is done in free space or through building walls, not actual earth.
 
 **Core design principle — payload-agnostic relay:** Relay nodes forward mesh packets without inspecting or decoding sensor payloads. This means the relay firmware never changes regardless of what sensor type is attached to a sensor node. A sensor node could be replaced with a completely different sensor (e.g., CO₂, PM2.5, soil moisture) and relays would continue forwarding correctly without any firmware update. This is the primary architectural goal of the project.

This implementation builds a two-tier underground LoRa mesh using **5 physical T-Beam boards**:

| Role | Count | Hardware | Purpose |
|------|-------|----------|---------|
<<<<<<< Updated upstream
| **Sensor node** | 2 | LilyGO T-BEAM-AXP2101-V1.2 (LoRA 923MHz) + DHT22 | Generate telemetry, route upstream |
| **Relay node** | 1 | LilyGO T-BEAM-AXP2101-V1.2 (LoRA 923MHz) | Forward packets opaquely (payload-agnostic) |
| **Edge bridge** | 2 | LilyGO T-BEAM-AXP2101-V1.2 (LoRA 923MHz) | Mesh sink + LoRaWAN end device uplinking to TTN |
=======
| **Sensor node** | 2 | LILYGO T-Beam Meshtastic (923MHz) + DHT22 | Generate telemetry, route upstream |
| **Relay node** | 1 | LILYGO T-Beam Meshtastic (923MHz) | Forward packets opaquely (payload-agnostic) |
| **Edge bridge node** | 2 | LILYGO T-Beam Meshtastic (923MHz) | Dual-role: mesh rank-0 sink + LoRaWAN OTAA uplink |
>>>>>>> Stashed changes

1. Two sensor nodes read DHT22 (temperature + humidity) and route upstream through mesh parents.
2. One relay node forwards packets **opaquely** — it never reads sensor payloads. This is the core payload-agnostic design.
3. Two edge bridge nodes aggregate mesh telemetry and uplink to TTN via LoRaWAN (AS923). Both are registered as separate LoRaWAN end devices.
4. Each edge bridge is single-radio, so firmware time-slices between `MESH_LISTEN` and bounded `LORAWAN_TXRX` windows.
5. TTN acts as the LoRaWAN network server. A single Pico W runs the application server (Phew! + MicroPython), consuming data from TTN via MQTT.
6. All nodes are mains-powered (5V USB supply). No battery power budget constraints.
7. The final deliverable is successful real end-to-end uplink (not simulated), plus measured latency/PDR/throughput and a minimal monitoring dashboard.

---

## System Architecture Overview

### LoRa Mesh Network Topology (5 nodes total)

```
                   ╔════════════════════════════════════════════════════════╗
                   ║        UNDERGROUND LoRa MESH NETWORK            ║
                   ║      5 nodes — 2 sensor, 1 relay, 2 edge bridge    ║
                   ║     Core principle: payload-agnostic relay        ║
                   ╠════════════════════════════════════════════════════════╣
                   ║                                                        ║
                   ║  [Sensor 1]         [Sensor 2]                      ║
                   ║   (DHT22)            (DHT22)                        ║
                   ║      │                  │                           ║
                   ║      │ LoRa             │ LoRa                      ║
                   ║      ▼                  ▼                           ║
                   ║   [Relay 1]      [Edge Bridge 2]                    ║
                   ║  forwards          (dual-role)                      ║
                   ║  opaquely               │                            ║
                   ║      │                  │                            ║
                   ║      ▼                  │                            ║
                   ║ [Edge Bridge 1]         │                            ║
                   ║   (dual-role)           │                            ║
                   ╚══════════════╪═══════════════════════╪═══════════════╝
                                  │                       │
                     LoRaWAN AS923│(OTAA, AES-128) LoRaWAN AS923│
                                  ▼                       ▼
                   ┌─────────────────────────────────────────────┐
                   │            RAK WisGate ×2                    │
                   │       (8-ch LoRaWAN gateway)                 │
                   └─────────────────────┬───────────────────────┘
                                         │ Ethernet / WiFi backhaul
                                         ▼
                   ┌─────────────────────────────────────────────┐
                   │         TTN Network Server (cloud)           │
                   │     OTAA join, MIC, payload decode           │
                   │     decoder.js payload formatter             │
                   └─────────────────────┬───────────────────────┘
                                         │ MQTT
                                         ▼
                   ┌─────────────────────────────────────────────┐
                   │       Pico W — Phew! App Server              │
                   │        Dashboard + telemetry display         │
                   └─────────────────────────────────────────────┘
```

### Mesh Routing Summary

```
  Rank 2              Rank 1                    Rank 0
  (Leaf)              (Relay)                   (Sink → LoRaWAN)
  ──────              ──────                   ──────

  Sensor 1 ─────► Relay 1 ───────────► EDGE BRIDGE 1 ──► LoRaWAN ──► TTN
  Sensor 2 ──────────────────────────► EDGE BRIDGE 2 ──► LoRaWAN ──► TTN

 • Sensor 1 routes through Relay 1 by default (fallback: EDGE BRIDGE 2)
 • Sensor 2 routes to EDGE BRIDGE 2 by default (fallback: Relay 1 -> EDGE BRIDGE 1)
 • Score-based parent selection determines actual routing at runtime

 KEY DESIGN PRINCIPLE — PAYLOAD-AGNOSTIC RELAY:
 • Relay 1 NEVER inspects sensor payloads
 • It forwards the opaque byte blob as-is (no decode, no parse)
 • If Sensor 1’s DHT22 was swapped for a CO₂ sensor tomorrow,
   Relay 1 would continue forwarding without any firmware change
 • Only edge bridges and TTN decoder need to know payload schema
```

**Role of each component:**
- **WisGate:** LoRaWAN gateway only. Forwards uplink frames to TTN. No application logic.
- **TTN:** Network server. Handles OTAA join, MIC verification, frame counter, payload decoding via formatter. Pushes decoded data to Pico W.
- **Pico W:** Application server only. Receives TTN data via MQTT subscription or webhook. Runs a Phew!-based dashboard. No LoRaWAN stack.

---

## Hardware Specification

### Node Hardware (All LoRa Nodes)

| Component | Model | Specifications |
|-----------|-------|----------------|
| **Main board** | LILYGO T-Beam Meshtastic (923MHz) | ESP32, SX1276 LoRa transceiver, USB power input |
| **LoRa RF** | Included 923MHz LoRa antenna | Must match AS923 deployment |
| **Temperature & Humidity Sensor** | DHT22 (AM2302) | Digital output, ±0.5°C temp accuracy, ±2% RH accuracy |

**Confirmed sensor assignment:** Each sensor node carries a **DHT22** sensor providing both temperature and humidity readings.

**Power:** All nodes are connected to **mains power** (5V USB adapter or regulated 5V supply) at all times. No battery operation, no sleep mode required.

**T-Beam hardware validation gate (mandatory before firmware work):**
- Confirm board revision from silk-screen and boot logs.
- Bench-confirm LoRa pin mapping before first mesh packet.
- Baseline T-Beam (legacy ESP32 + SX1276) profile to verify first:
  - `LORA_SS=18`, `LORA_RST=23`, `LORA_DIO0=26`, `LORA_DIO1=33`, `LORA_DIO2=32`
  - `LORA_SCK=5`, `LORA_MISO=19`, `LORA_MOSI=27`
- If your board revision differs, update constants in `config.h` before integration testing.

### Gateway Hardware

| Component | Model | Specifications |
|-----------|-------|----------------|
| **LoRaWAN Gateway** | RAK WisGate Edge Lite 2 (RAK7268) ×2 | 8-channel LoRa concentrator, Ethernet/WiFi backhaul, AS923 support |

### Edge Bridge Hardware (Single-Board Dual-Role)

**Requirement:** One T-Beam per edge bridge running both roles
- **Mesh role:** rank-0 sink + aggregation (`Arduino-LoRa` path).
- **LoRaWAN role:** OTAA end device uplinking `BridgeAggV1` (`MCCI LMIC` path).
- **Constraint:** one SX1276 radio per board, so firmware must arbitrate radio ownership.

**Design note:** edge bridge firmware uses bounded LoRaWAN windows and immediately returns to mesh listen mode to preserve downstream responsiveness.

### Application Server

| Component | Model | Role |
|-----------|-------|------|
| **Pico W** | Raspberry Pi Pico W | Application server (MicroPython + Phew!). Subscribes to TTN MQTT or receives TTN webhook. Serves dashboard. No LoRaWAN stack. |

---

## Software Libraries

### Mesh Firmware (Sensor / Relay / Edge Bridge in mesh mode)

| Library | Purpose | Used By |
|---------|---------|---------|
| **Arduino-LoRa** (Sandeep Mistry) | Raw LoRa mesh radio driver | Sensor, relay, edge bridge |
| **SPI.h** | SPI bus for SX1276 | All mesh nodes |
| **DHT sensor library** (Adafruit) | DHT22 temperature + humidity driver | Sensor nodes |
| **Preferences** (ESP32 NVS) | Persistent node config/counters | All mesh nodes |
| **mesh_protocol.h** | Custom mesh protocol definitions | All mesh nodes |

### Edge Bridge (T-Beam, LoRaWAN mode)

| Library | Purpose |
|---------|---------|
| **Arduino-LMIC** (MCCI) | LoRaWAN stack (OTAA, MIC, AES, duty-cycle behavior) |
| **SPI.h** | SPI bus for SX1276 |
| **FreeRTOS** | Mesh/LoRaWAN worker scheduling + radio ownership arbitration |
| **Preferences** (ESP32 NVS) | Persistent counters/config |

**Critical:** LMIC runs on **edge bridge nodes only** (during LoRaWAN windows). Sensor and relay nodes run raw mesh firmware only.

### Application Server (Pico W)

| Library | Purpose |
|---------|---------|
| **MicroPython** | Runtime |
| **umqtt.simple** | MQTT client to subscribe to TTN application MQTT broker |
| **Phew!** (or equivalent MicroPython non-blocking HTTP server) | Lightweight HTTP server for dashboard |

---

## TTN Airtime Budget Analysis (Critical — Read Before Proceeding)

This analysis must be resolved before committing to the bridged uplink architecture. TTN fair use policy allows **30 seconds of airtime per LoRaWAN device per day**. Each edge bridge node is one LoRaWAN device.

### Airtime per frame at each SF (reference only: 20-byte LoRaWAN payload, BW 125 kHz, CR 4/5)

| SF  | Approx. airtime/frame | Max frames/day (30s budget) |
|-----|-----------------------|-----------------------------|
| SF7 | ~56 ms                | ~535                        |
| SF8 | ~103 ms               | ~291                        |
| SF9 | ~185 ms               | ~162                        |
| SF10| ~370 ms               | ~81                         |

The table above is **not** the actual `BridgeAggV1` airtime; it is a 20-byte reference point only.

### Actual payload sizing for `BridgeAggV1`

For `record_count = N`:
- FRMPayload bytes = `3 + (14 × N)`
- LoRaWAN PHYPayload bytes used for airtime approximation = `FRMPayload + 13`
  (MHDR 1 + FHDR 7 + FPort 1 + MIC 4)

At SF7/BW125/CR4/5:
- `N=2`: FRMPayload 31B → PHYPayload ~44B → airtime ~92.4 ms
- `N=4`: FRMPayload 59B → PHYPayload ~72B → airtime ~133.4 ms
- `N=6`: FRMPayload 87B → PHYPayload ~100B → airtime ~174.3 ms
- `N=7`: FRMPayload 101B → PHYPayload ~114B → airtime ~194.8 ms

### Budget calculation example (current low-latency defaults)

Assume:
- Sensor periodic interval = 120s
- Flush interval = 240s or 7 records
- Baseline load split: bridge 1 gets ~1 sensor, bridge 2 gets ~1 sensor

In steady state, the **time trigger dominates**:
- **Bridge 1:** ~2 records/uplink, 360 uplinks/day → 360 × 92.4 ms ≈ **~33.3 s/day**
- **Bridge 2:** ~2 records/uplink, 360 uplinks/day → 360 × 92.4 ms ≈ **~33.3 s/day**

Result: both bridges exceed TTN 30s/day fair use under current low-latency defaults.

Failover note: if one bridge temporarily carries both sensors, that bridge rises to roughly ~4 records/uplink (~48.0 s/day at SF7).

### Operating profiles

1. **Low-latency lab profile (default in this plan):**
   - `sensor_interval_s = 120`
   - `flush_interval_s = 240`
   - Suitable for latency/convergence experiments, but **not TTN fair-use compliant** for 24h continuous operation.
2. **TTN fair-use profile (optional):**
   - `sensor_interval_s = 180`
   - `flush_interval_s = 450`
   - Expected balanced case at SF7: ~2-3 records/uplink, 192 uplinks/day → **~17.7-21.7 s/day** per bridge
   - Expected busy bridge at SF7 (temporary failover): ~5 records/uplink, 192 uplinks/day → 192 × 153.9 ms ≈ **~29.5 s/day**
   - Requirement: keep edge uplink at SF7; prolonged SF9 operation will exceed 30s/day.

**Decision:** Keep the low-latency profile for bench performance experiments. Use TTN fair-use profile when running long-duration tests on public TTN.

---

## End-to-End Data Path (What Happens Per Packet)

1. Sensor node samples DHT22, packs temperature and humidity into `SensorPayload`.
2. Sensor adds `MeshHeader` and sends `PKT_TYPE_DATA` to current parent.
3. Relay receives frame, validates CRC, deduplicates, ACKs previous hop, decrements TTL, forwards payload **unchanged** to its parent.
4. Edge bridge receives final mesh frame, ACKs previous hop, and stores the record in aggregation buffer.
5. On flush trigger (240s elapsed or 7 records buffered): edge bridge switches radio ownership from mesh to LoRaWAN, transmits `BridgeAggV1` uplink via OTAA, completes RX windows, then returns to mesh listen mode.
6. WisGate forwards raw frame to TTN network server.
7. TTN decodes payload using `decoder.js` formatter and exposes structured data.
8. TTN pushes decoded data to Pico W via MQTT broker or webhook.
9. Pico W application server serves dashboard and telemetry display.

---

## Public APIs / Interfaces / Types (Decision-Complete)

### 1. `MeshHeader` (mandatory on all mesh packets)

**LoRa PHY frame vs mesh protocol — what goes where:**

```
LoRa PHY frame (transmitted over the air by SX1276):
┌──────────┬──────┬──────────┬──────────────────────────────────────┬─────┐
│ Preamble │ PHDR │ PHDR_CRC │            PHYPayload                │ CRC │
│  (auto)  │(auto)│  (auto)  │       ◄── YOUR BYTES GO HERE ──►     │(auto)│
└──────────┴──────┴──────────┴──────────────────────────────────────┴─────┘
                                          │
                              ┌───────────┘
                              ▼
              ┌───────────────────────┬──────────────────────┐
              │   MeshHeader (10B)    │   Payload (N bytes)   │
              │  bytes 0–9            │   bytes 10–N          │
              │  (relay reads this)   │   (relay copies       │
              │                       │    blindly)            │
              └───────────────────────┴──────────────────────┘
                                              │
                              ┌───────────────┘
                              ▼
                  Data packet:  SensorPayload (7B for DHT22)
                  Beacon:       BeaconPayloadV1 (4B)

  Preamble, PHDR, PHDR_CRC, CRC → handled by SX1276 hardware (Arduino-LoRa).
  PHYPayload → written by firmware via LoRa.write(). This is MeshHeader + payload.
```

**Concrete byte layout (10 bytes total):**

| Offset | Field        | Size   | Notes |
|--------|-------------|--------|-------|
| 0      | flags        | 1 byte | PKT_TYPE (4b), ACK_REQ (1b), FWD (1b), reserved (2b) |
| 1      | src_id       | 1 byte | Originating node ID |
| 2      | dst_id       | 1 byte | Next-hop target (0xFF = broadcast) |
| 3      | prev_hop     | 1 byte | Last forwarder ID |
| 4      | ttl          | 1 byte | Decremented each hop |
| 5      | seq_num      | 1 byte | Per-source sequence counter |
| 6      | rank         | 1 byte | Hop distance from edge (0 = edge) |
| 7      | payload_len  | 1 byte | Byte length of following payload |
| 8–9    | crc16        | 2 bytes | CRC16 over bytes 0–7 |

Total overhead: 10 bytes. Relays inspect bytes 0–9 only. Bytes 10–N are the opaque sensor payload.

### 2. `BeaconPayloadV1` (beacon frames only)

**Byte layout (4 bytes):**

| Offset | Field          | Size   | Notes |
|--------|---------------|--------|-------|
| 0      | schema_version | 1 byte | Always 0x01 |
| 1      | queue_pct      | 1 byte | TX queue occupancy 0–100 |
| 2      | link_quality   | 1 byte | RSSI-derived upstream LQ 0–100 |
| 3      | parent_health  | 1 byte | ACK success rate to parent 0–100 |

### 3. `SensorPayload` (originating sensor node — carried opaquely by relays)

All sensor nodes carry a **DHT22** sensor providing temperature and humidity. The relay never reads past the `MeshHeader`.

**DHT22 node payload (7 bytes):**

| Offset | Field          | Size   | Notes |
|--------|---------------|--------|-------|
| 0      | schema_version | 1 byte | Always 0x01 |
| 1      | sensor_type    | 1 byte | 0x03 = DHT22 |
| 2      | temp_c_x10     | 2 bytes | Temperature in °C × 10, signed big-endian (e.g. 253 = 25.3°C) |
| 4      | humidity_x10   | 2 bytes | Relative humidity in % × 10, unsigned big-endian (e.g. 655 = 65.5%) |
| 6      | status         | 1 byte  | 0x00 = OK, 0x01 = read error (discard reading) |

### 3a. `BridgeAggV1` (edge aggregate telemetry uplink — schema_version 0x02)

Used when the edge bridge node flushes its aggregation buffer. All sensor data is DHT22 telemetry.

**Header (3 bytes):**

| Offset | Field          | Size   | Notes |
|--------|---------------|--------|-------|
| 0      | schema_version | 1 byte | Always 0x02 |
| 1      | bridge_id      | 1 byte | Edge bridge node ID |
| 2      | record_count   | 1 byte | Number of records that follow (max 7) |

**Per-record layout (14 bytes each):**

| Offset | Field            | Size    | Notes |
|--------|-----------------|---------|-------|
| 0      | mesh_src_id      | 1 byte  | Originating sensor node ID |
| 1      | mesh_seq         | 1 byte  | Sensor's sequence number |
| 2      | sensor_type_hint | 1 byte  | Always 0x03 = DHT22 |
| 3      | hop_estimate     | 1 byte  | TTL_default − received TTL |
| 4–5    | edge_uptime_s    | 2 bytes | Edge uptime when record was received |
| 6      | opaque_len       | 1 byte  | Byte length of sensor payload (always 7 for DHT22) |
| 7–13   | opaque_payload   | 7 bytes | DHT22 SensorPayload bytes |

**Maximum aggregate size at SF9:** 3 + (7 × 14) = 101 bytes. SF9 max payload = 115 bytes. Safe. Maximum record_count = 7 at SF9, 8 at SF7/SF8.

### 4. `PacketUID`

Format: `mesh_src_id (8-bit) | seq_num (8-bit)` → 16-bit key. Log as hex string `{src:02X}{seq:02X}`. Dedup window: 30 seconds. Handle 8-bit rollover with modular arithmetic.

### 5. Config API (unified across all node types)

Fields: `role`, `node_id`, `region`, `frequency`, `sf_default`, `sf_fallback`, `feature_flags`, `gateway_target`, `score_weights[3]`, `flush_interval_s`.

`score_weights[3]`: runtime-configurable weights for `[rank, rssi, queue]`, integers summing to 100. Default `[60, 25, 15]`. Stored in ESP32 NVS. Adjustable via serial command without reflash.

---

## ESP32 Resource Budget Gate (Mandatory)

T-Beam nodes have significantly more headroom than minimal-MCU designs, but deterministic resource use is still required.

| Gate | Threshold | Action |
|------|-----------|--------|
| Free heap at boot | `>= 120 KB` | Fail boot self-check and log `HEAP_LOW_BOOT` if below |
| Runtime free heap floor | `>= 80 KB` | Raise warning `HEAP_LOW_RUNTIME` and shed non-critical logs |
| Sketch flash usage | `< 85%` partition usage | Freeze feature additions and optimize if exceeded |
| Edge aggregate queue usage | `< 80%` sustained | Trigger queue overflow policy + log |

**Action items:**
1. Log heap watermark (`esp_get_free_heap_size`) at boot and every 60s.
2. Keep bounded static queue sizes for mesh RX/forwarding and edge aggregation/uplink staging.
3. Avoid unbounded dynamic allocation in hot paths.
4. Record build sizes for sensor/relay/edge firmware in test artifacts.

---

## Node ID Provisioning (Not Compile-Time)

Node IDs are **not** compile-time constants. Provisioning mechanism:

1. Factory default: node boots with `node_id = 0xFF` (unprovisioned sentinel).
2. Operator connects USB serial and runs `provision.py --node-id 0x01 --role sensor`.
3. Script sends `SETID:<id>` command. Node stores ID in ESP32 NVS and reboots.
4. On boot: if stored ID is `0xFF`, node halts and blinks error. If 0x01–0xFE, proceeds.
5. `provision.py` maintains `nodes.csv` mapping node IDs to physical locations and flash dates.
6. Duplicate ID detection: relays log a `CONFLICT` event if two sources share the same ID.

**NVS key layout (`namespace = mesh_cfg`):**

| Key | Type | Notes |
|-----|------|-------|
| `node_id` | `u8` | `0xFF` = unprovisioned |
| `role` | `u8` | 0x01=sensor, 0x02=relay, 0x03=edge_bridge |
| `feature_flags` | `u16` | Reserved |
| `sf_default` | `u8` | Spreading factor 7-12 |
| `sf_fallback` | `u8` | Fallback SF on link degradation |
| `w_rank`, `w_rssi`, `w_queue` | `u8` | Score weights summing to 100 |
| `flush_interval_s` | `u16` | Supports values >255 (e.g. 450s) |
| `overflow_drop_count` | `u32` | Persistent queue overflow counter |
| `wdt_reset_count` | `u16` | Incremented on each WDT reboot |
| `crc_fail_count` | `u32` | Persistent CRC failure counter (relay/edge bridge mesh path) |

---

## Watchdog Timer Policy

All node types must implement hardware watchdog. Non-negotiable for unattended underground deployment.

1. **Timeout:** 30 seconds via ESP32 Task Watchdog (TWDT).
2. **Kick locations:** main loop/task loop top, post-TX callback, post-RX callback, post-mode-switch completion on edge bridges.
3. **Do NOT kick inside:** retry loops or blocking waits that can mask deadlock.
4. **On watchdog reset:** read `esp_reset_reason()`. If WDT-related, increment NVS `wdt_reset_count` before normal boot and log `WDT_RESET`.
5. **Edge bridge policy:** mesh worker and LoRaWAN worker each register with TWDT; radio arbiter loop must also remain non-blocking.
6. **Safe mode threshold:** if `wdt_reset_count > 5` since last manual clear, node enters safe mode (health beacon only) and waits for operator action.

---

## Power Supply

All nodes (sensor, relay, edge) are connected to **mains power** (5V USB adapter or regulated 5V supply) at all times. No battery operation. No sleep mode. No power budget constraint. This simplifies firmware significantly — no duty cycling, no deep sleep, no wake-on-interrupt logic.

---

## ACK Timeout Table (Keyed to SF)

| SF  | Frame airtime (20B) | ACK timeout | Max retry interval |
|-----|---------------------|-------------|-------------------|
| SF7 | ~56 ms              | 600 ms      | 2.4 s (3 retries) |
| SF8 | ~103 ms             | 800 ms      | 3.2 s |
| SF9 | ~185 ms             | 1200 ms     | 4.8 s |
| SF10| ~370 ms             | 1800 ms     | 7.2 s |
| SF11| ~660 ms             | 2500 ms     | 10.0 s |
| SF12| ~1319 ms            | 3500 ms     | 14.0 s |

Store as `PROGMEM` table indexed by `sf - 7`. Recalculate immediately on SF change.

Parent timeout: `parent_timeout = 3 × (ACK_timeout × max_retries) + beacon_interval`. Use 35s as ceiling.

---

## SF Selection Criteria

1. Begin with SF7 after parent acquisition.
2. If `link_quality` drops below threshold for 3 consecutive beacons → promote to SF9. Log `SF_FALLBACK`.
3. If on SF9 and `link_quality` recovers for 5 consecutive beacons → return to SF7. Log `SF_RECOVER`.
4. SF11/SF12 reserved for emergency rescue beacon only.
5. ADR disabled on all mesh nodes.
6. Edge bridge LoRaWAN path (via LMIC): disable ADR for deterministic airtime budgeting. After OTAA join on each edge bridge, call `LMIC_setAdrMode(0)` and keep fixed DR/SF7 for TTN fair-use profile runs. Re-enable ADR only for explicit ADR experiments.

| SF transition | RSSI threshold |
|--------------|---------------|
| SF7 → SF9   | < −105 dBm    |
| SF9 → SF7   | > −95 dBm     |

---

## Orphan State Queue Policy

When a relay or sensor loses its parent with no backup:

1. Stop accepting new packets into TX queue.
2. Hold existing TX queue contents up to `orphan_hold_duration = 60s`.
3. Continue RX and dedup — still accept inbound frames and issue local ACKs.
4. On parent acquisition within 60s: flush held queue, resume normal operation.
5. On expiry without parent: drop held queue, increment NVS `overflow_drop_count`, log `ORPHAN_DROP`. Enter beacon-listen-only mode.
6. Held packets retain `PacketUID`. If re-delivered after recovery, next-hop dedup will catch duplicates — acceptable behaviour.

---

## Congestion Control Policy (Decision-Complete)

This policy is applied to **mesh paths on all nodes** (sensor + relay + edge bridge). It is designed to reduce collisions during burst traffic while staying within T-Beam timing/resource limits.

### Constants (shared)

```c
CONGEST_GREEN  = 0
CONGEST_YELLOW = 1
CONGEST_RED    = 2

ACK_FAIL_WINDOW = 20          // attempts
QUEUE_PCT_YELLOW = 60
QUEUE_PCT_RED    = 80
ACK_FAIL_YELLOW_PCT = 20
ACK_FAIL_RED_PCT    = 40

TX_JITTER_MIN_MS = 0
TX_JITTER_MAX_MS = 2000
SENSE_MAX_ATTEMPTS = 2
SENSE_BACKOFF_MIN_MS = 50
SENSE_BACKOFF_MAX_MS = 200
CHANNEL_BUSY_RSSI_DBM = -90

INTERVAL_MULT_X10[3] = {10, 15, 20}   // GREEN 1.0x, YELLOW 1.5x, RED 2.0x
RETRIES_BY_STATE[3]  = {3, 2, 1}      // GREEN, YELLOW, RED
```

### State derivation

1. Compute `ack_fail_pct` over last 20 TX attempts.
2. Define `queue_pct` source per role:
   - Sensor node: use selected parent's advertised `BeaconPayloadV1.queue_pct` (latest valid beacon).
   - Relay node: use local TX queue occupancy percentage.
   - Edge bridge node (mesh path): use local aggregation/uplink queue occupancy percentage.
3. Use `queue_pct` and `ack_fail_pct`:
   - `RED` if `queue_pct >= 80` OR `ack_fail_pct >= 40`
   - `YELLOW` if `queue_pct >= 60` OR `ack_fail_pct >= 20`
   - `GREEN` otherwise
4. Evaluate once per beacon period (10s) and log transitions (`CONGEST_GREEN`, `CONGEST_YELLOW`, `CONGEST_RED`).

### TX discipline

1. Add pre-TX jitter: random delay `0..2000 ms` on originated telemetry packets.
2. Perform RSSI-based channel sensing before non-ACK transmissions:
   - sample radio RSSI in RX/standby mode and compare with `CHANNEL_BUSY_RSSI_DBM` (default `-90 dBm`). Do not use `LoRa.rssi()` as it returns the last packet RSSI. Instead, implement a `read_channel_rssi()` helper in `shared/mesh_protocol.h` to put the radio in RX mode, read register `0x1A` (RegRssiValue), and apply the HF port formula (-157 + RegRssiValue).
   - if channel busy, random backoff `50..200 ms`, retry sensing up to 2 times.
   - if still busy after final attempt, transmit after final backoff (no additional retry loop).
3. Optional enhancement: if hardware exposes CAD-complete IRQ wiring and firmware support, CAD may be enabled in addition to RSSI sensing.
4. Effective telemetry interval:
   - `effective_interval_s = base_interval_s * INTERVAL_MULT_X10[state] / 10`
5. Effective retries:
   - `effective_retries = RETRIES_BY_STATE[state]`
6. Beacon staggering (all mesh-participating nodes):
   - `beacon_offset_ms = (node_id % 5) * 2000`
   - apply once at boot so 5 nodes spread across a 10s beacon window.

### Feasibility note

Worst-case sensing+backoff wait here is < 500 ms, well below the 30s ESP32 TWDT timeout. No extra WDT kick points are required.

---

## Sensor Node Implementation (How + Why)

### Responsibilities
1. Sense environment (DHT22 — temperature + humidity).
2. Join and maintain mesh parent.
3. Send periodic telemetry.
4. Retry with SF-keyed ACK timeout policy.

### Implementation Steps

1. **Sensor driver:**
   - **DHT22:** Use Adafruit DHT library. Read temperature and humidity each cycle. If read fails, set `status = 0x01` and skip transmission for that cycle. Compute `temp_c_x10 = (int)(temp_c * 10)` and `humidity_x10 = (int)(rh * 10)`.
2. **Mesh radio pin init (mandatory before `LoRa.begin()`):**
   - Set explicit T-Beam pin mapping instead of relying on library defaults:
      - `LoRa.setPins(MESH_SS_PIN, MESH_RST_PIN, MESH_DIO0_PIN);`
   - Baseline T-Beam 923 profile:
      - `MESH_SS_PIN=18`, `MESH_RST_PIN=23`, `MESH_DIO0_PIN=26`
      - Optional IRQ pins if used: `MESH_DIO1_PIN=33`, `MESH_DIO2_PIN=32`
   - If your board revision differs, update constants from board silk-screen/wiring check before firmware testing.

3. **Build `SensorPayload`** per byte layout in section 3. Pack as byte array — never as C struct (struct padding is compiler-dependent and will corrupt the decoder).

4. **Keep payload opaque** — relay never reads sensor bytes.

5. **Parent selection:**
   - Parse incoming `BeaconPayloadV1`.
   - Use shared normalization helpers from `shared/mesh_protocol.h`:
     - `MAX_RANK = 4`
     - `rank_clamped = min(neighbor.rank, MAX_RANK)`
     - `rank_score = ((MAX_RANK - rank_clamped) * 100) / MAX_RANK` (rank 0→100, rank 1→75, rank 2→50, rank 3→25, rank >=4→0)
     - `rssi_norm = rssi_to_norm(rssi_dbm)`
     - `rssi_to_norm()` must be defined in `shared/mesh_protocol.h`:

```cpp
static inline uint8_t clamp_u8(int v, int lo, int hi) {
  if (v < lo) return (uint8_t)lo;
  if (v > hi) return (uint8_t)hi;
  return (uint8_t)v;
}

static inline uint8_t rssi_to_norm(int16_t rssi_dbm) {
  long mapped = map((long)rssi_dbm, -120, -60, 0, 100);  // map() does not clamp
  return clamp_u8((int)mapped, 0, 100);
}
```
   - Bench-check this helper at close range (RSSI above `-60 dBm`) to confirm clamping remains `100` and does not overflow scoring.

   - `score = w_rank * rank_score + w_rssi * rssi_norm + w_queue * (100 - queue_pct)`
   - Weights from NVS (`w_rank`, `w_rssi`, `w_queue`). Adjustable via serial without reflash.
   - Switch only if `score_improvement >= 8` and cooldown of 20s has elapsed.

6. **Transmission policy:**
   - Base interval: 120s (or profile override).
   - Maintain local rolling ACK-failure window: `ack_fail_window = 20` TX attempts (`ack_fail_pct = failures/20 * 100`).
   - Apply congestion control policy above:
     - pre-TX jitter `0..2000 ms`
     - RSSI-based channel sensing + bounded backoff
     - `effective_interval_s` by congestion state (1.0x / 1.5x / 2.0x)
     - `effective_retries` by congestion state (3 / 2 / 1)
   - ACK timeout from SF lookup table.

7. **TX buffer overflow policy:**
   - Sensor node has a single TX buffer (one packet). If a new sensor reading is ready while the previous packet is still pending ACK/retry, **overwrite the TX buffer with the newer reading**. The stale reading is discarded.
   - Rationale: for temperature/humidity telemetry, the latest reading is always more valuable than a stale one, and bounded buffering keeps behavior deterministic.
   - Log `TX_OVERWRITE` with the discarded `PacketUID` for post-analysis.
   - This should rarely trigger at 120s intervals (3 retries × 600ms ACK timeout = 1.8s worst case at SF7), but protects against sustained link failure scenarios.

8. **Reliability:** Dedup by `(src_id, seq_num)` with 30s window, 16-entry table. TTL default 10.

9. **Self-healing:** Parent timeout 35s. Promote best backup from neighbor table. Orphan policy as above.

10. **Watchdog:** Kick at main loop top and post-TX using ESP32 TWDT (30s timeout).

---

## Relay Node Implementation (How + Why)

### Responsibilities
1. Forward packets opaquely — no inspection of sensor payload bytes.
2. Maintain neighbor table and upstream parent.
3. Send hop ACK quickly.
4. Advertise congestion via beacon metadata.

### Implementation Steps

1. **Mesh radio pin init (mandatory before `LoRa.begin()`):**
   - Set explicit T-Beam pin mapping:
      - `LoRa.setPins(MESH_SS_PIN, MESH_RST_PIN, MESH_DIO0_PIN);`
   - Use the same T-Beam baseline pin profile as sensor nodes unless bench wiring confirms otherwise.

2. **Ingress pipeline:**
   - Validate minimum length (10 bytes).
   - Drop self-originated frames.
   - Dedup check.
   - CRC16 validation over bytes 0–7. Drop on mismatch, increment NVS `crc_fail_count`, log `CRC_FAIL`.
   - ACK previous hop if `ACK_REQ` set.

3. **Forwarding pipeline:**
   - Drop if TTL == 0. Log `TTL_DROP` with `PacketUID`.
   - Drop if no parent → enter orphan policy.
   - Set `prev_hop = MY_NODE_ID`, set `FWD` flag.
   - Forward bytes 0–(payload_len+9) unchanged. Do not inspect bytes 10–N.

4. **Queue-aware beacon:** Compute `queue_pct`, `link_quality` (RSSI EWMA), `parent_health` (ACK success rate over last 20 attempts). Emit in `BeaconPayloadV1` every 10s.
   - Apply startup beacon phase offset: `beacon_offset_ms = (node_id % 5) * 2000`.
   - Relay congestion state uses this node's own forwarding ACK-failure window (`ack_fail_window = 20`) plus local `queue_pct` as defined in the shared congestion policy.

5. **Parent algorithm:** Use the same shared normalization as sensor nodes (`MAX_RANK=4`, `rank_score` mapping above, `rssi_norm` in 0–100), then:
   - `score = w_rank * rank_score + w_rssi * rssi_norm + w_queue * (100 - queue_pct)`
   - Switch with hysteresis + 20s dwell.

6. **Failure handling:** ACK failures ≥ 5 consecutive → re-parent. No backup → orphan state.

7. **Logging:** Per-packet events with `PacketUID`, RSSI, forward decision, drop reason. Persist drop counters in NVS.

8. **Watchdog:** ESP32 TWDT 30s. Kick at loop top and post-TX.

---

## Dual-Role Edge Bridge Node Implementation (How + Why)

### Responsibilities
1. Act as rank-0 mesh sink.
2. Aggregate mesh telemetry into `BridgeAggV1`.
3. Act as LoRaWAN OTAA end device uplinking to TTN.
4. Arbitrate a single SX1276 radio between mesh and LoRaWAN operation.

### Hardware Model
- **Board:** one T-Beam 923 per edge bridge.
- **Radio:** one SX1276 shared by mesh and LoRaWAN stacks.
- **Power:** mains USB.

### Firmware Structure

1. **`mesh_worker` path:**
   - Keep radio in mesh listen mode by default.
   - Receive mesh frames, ACK previous hop, dedup, append to aggregation buffer.
   - Trigger uplink request when `flush_interval_s` expires or `record_count == 7`.

2. **`lorawan_worker` path:**
   - Maintain OTAA session with exponential backoff on failures.
   - On uplink request, obtain radio ownership, send unconfirmed `BridgeAggV1` via `LMIC_setTxData2`, wait for RX windows, release ownership.
   - Optional TTN airtime guard (TTN fair-use profile):
     - Track `daily_airtime_ms` and `airtime_window_start_s` (uptime-based 86400s window).
     - Estimate airtime before each uplink from current SF + payload bytes.
     - If `daily_airtime_ms + est_airtime_ms > 28000`, defer send for 60s (`AIRTIME_DEFER`) and keep current frame pending.
     - Reset `daily_airtime_ms` when uptime window reaches 86400s.
   - On 3 consecutive uplink failures: log `UPLINK_FAIL` with `PacketUID` list, drop frame, continue.

3. **Single-radio arbitration contract (mandatory):**
   - Radio ownership states: `MESH_LISTEN` and `LORAWAN_TXRX`.
   - Only one stack may own the radio at a time.
   - Enter LoRaWAN window only when an aggregate frame is pending.
   - Bound each LoRaWAN window (TX + RX1/RX2 + margin), then force return to mesh listen.
   - Apply per-edge phase offset on flush timer so both edge bridges do not leave mesh simultaneously.

**Reference implementation sketch (Arduino/ESP32):**

```cpp
enum RadioMode : uint8_t { MESH_LISTEN = 0, LORAWAN_TXRX = 1 };

static volatile RadioMode radio_mode = MESH_LISTEN;
static volatile bool uplink_pending = false;
static uint32_t lora_window_started_ms = 0;

static const uint32_t ARBITER_TICK_MS = 2;
static const uint32_t LORAWAN_WINDOW_BUDGET_MS = 4500;  // TX + RX1/RX2 + guard
static const uint32_t EDGE_FLUSH_PHASE_MS = (MY_NODE_ID % 2) * 1200;  // de-sync two edges

BridgeAggFrame pending_frame;

void mesh_worker_tick() {
  if (radio_mode != MESH_LISTEN) return;

  check_mesh_receive_and_ack();
  bool flush_due = agg_flush_due_with_phase(EDGE_FLUSH_PHASE_MS);
  bool count_full = (agg_record_count() >= 7);

  if (!uplink_pending && (flush_due || count_full)) {
    pending_frame = build_bridgeagg_v1();
    uplink_pending = true;
  }
}

void lorawan_worker_tick() {
  if (radio_mode != LORAWAN_TXRX) return;

  os_runloop_once();  // LMIC timing service

  if (uplink_pending && lmic_ready_for_tx()) {
    LMIC_setTxData2(/*fport=*/1, pending_frame.bytes, pending_frame.len, /*confirmed=*/0);
    uplink_pending = false;
  }
}

void arbiter_tick() {
  uint32_t now = millis();

  if (radio_mode == MESH_LISTEN && uplink_pending) {
    mesh_radio_pause();      // disable mesh RX callbacks/IRQ handling
    lorawan_radio_resume();  // LMIC owns radio now
    radio_mode = LORAWAN_TXRX;
    lora_window_started_ms = now;
    return;
  }

  if (radio_mode == LORAWAN_TXRX) {
    bool txrx_done = lmic_txrx_idle();
    bool overtime = (now - lora_window_started_ms) > LORAWAN_WINDOW_BUDGET_MS;
    if (txrx_done || overtime) {
      lorawan_radio_pause();
      mesh_radio_resume_rx();
      radio_mode = MESH_LISTEN;
    }
  }
}

void loop() {
  mesh_worker_tick();
  lorawan_worker_tick();
  arbiter_tick();
  esp_task_wdt_reset();
  delay(ARBITER_TICK_MS);
}
```

Use this as a structure template; concrete helper names (`lmic_ready_for_tx()`, `mesh_radio_pause()`, etc.) are implementation-specific.

4. **Queue policy:**
   - Edge aggregate queue depth: 32 frames.
   - On full queue, drop oldest aggregate, log `QUEUE_OVERFLOW` with dropped packet UIDs, increment NVS `overflow_drop_count`.

5. **LMIC thread-safety rule:** only `lorawan_worker` may call LMIC APIs (`os_runloop_once`, `LMIC_setTxData2`, join handlers).

6. **Observability:** log mode transitions (`MESH_LISTEN` <-> `LORAWAN_TXRX`), join transitions, every uplink attempt result, window duration, and overflow events with `PacketUID` list + `edge_uptime_s`.

---

## Reference Encoder/Decoder Workflow

Write and validate the `BridgeAggV1` codec **before writing any edge firmware**.

1. Write `tools/codec.py`: `encode_agg_v1()`, `decode_agg_v1()`. Include `roundtrip_test()` asserting encode → decode produces identical fields.
2. Write `tools/decoder.js`: TTN payload formatter implementing same logic. Validate both produce identical output for the same input hex strings.
3. Commit both before touching edge firmware.
4. During integration: compare edge-produced bytes against `codec.py` on bench before first TTN uplink.

---

## WisGate + TTN Setup and Configuration

> Reference: Professor's lab guide — [`SETTINGUPTTN.md`](SETTINGUPTTN.md)
>
> **Scope note:** In this project, LoRaWAN setup applies to **the two dual-role edge bridge T-Beam nodes only**. Sensor and relay nodes use raw LoRa mesh (Arduino-LoRa) and must not run LMIC.

### Step 1 — WisGate Hardware Setup

1. **Attach LoRa antenna** to the RP-SMA connector on the back panel of the RAK7268/C WisGate Edge Lite 2.
   - ⚠️ **Do not power on without antenna attached** — this can damage the radio frontend.
2. **Power on** using the 12V DC adapter (included) or PoE injector via the Ethernet port.

### Step 2 — Access the WisGate Web UI

Two access methods (use whichever is available):

| Method | Details |
|--------|---------|
| **Wi-Fi AP** | Connect to SSID `RAK7268_XXXX` (no password). Browse to `192.168.230.1`. |
| **Ethernet (WAN)** | Connect cable to ETH port. Default IP: `169.254.X.X` (last 4 hex of MAC → decimal). Set your PC to same subnet. |

- **Login credentials:** Username: `root`, Password: `admin12345678!`

### Step 3 — Connect WisGate to Internet

1. In WisGate Web UI, navigate to **Network → Wi-Fi → Settings**.
2. Set Mode to **DHCP Client**.
3. Scan and select your Wi-Fi SSID, enter encryption type and key.
4. After connection, note the new DHCP-assigned IP for future access.

### Step 4 — Configure WisGate for TTN

1. In WisGate Web UI, navigate to **LoRa Network → Network Settings**.
2. Set **Work mode** to `Packet forwarder`.
3. Set **Server URL** to `as1.cloud.thethings.network` (Singapore AS1 cluster).
   - ⚠️ Professor's guide uses `au1` (Australia). Our project uses **AS923 Singapore → `as1`**.
   - ⚠️ Treat all `au1` / `AU915` references in legacy lab material as not applicable for this project.
4. Note the **Gateway EUI** from the WisGate (also printed on the bottom label). You'll need this for TTN registration.

### Step 5 — TTN Account and Gateway Registration

1. Go to [https://www.thethingsnetwork.org](https://www.thethingsnetwork.org) and log in (or create an account).
2. Open the **Console** and select the **`as1`** (Singapore) cluster.
3. Register **WisGate #1**:
   - Click **Gateways → Register gateway**.
   - Enter the **Gateway EUI** from WisGate #1 bottom label.
   - Set **Frequency plan** to `Asia 920-923 MHz (AS923 Group 1)`.
   - Click **Register gateway** and verify status is **Connected**.
4. Register **WisGate #2** by repeating the same steps with the second unit's bottom-label Gateway EUI.
5. Confirm both gateways appear as connected in TTN console.

### Step 6 — TTN Application Setup

1. In TTN Console, click **Applications → Create application**.
2. Enter an **Application ID** (lowercase, e.g. `csc2106-g33-mesh`). This uniquely identifies your application.
3. Click **Create application**.

### Step 7 — Register Edge Bridges as End Devices

Repeat for **both** edge bridge nodes (`edge-bridge-01` and `edge-bridge-02`):

1. In the application, click **End devices → Register end device**.
2. Configure:
   - **Frequency plan:** `Asia 920-923 MHz (AS923 Group 1)`
   - **LoRaWAN version:** `LoRaWAN Specification 1.0.2` (MCCI LMIC library compatible)
   - **Regional Parameters version:** `RP001 Regional Parameters 1.0.2 revision B`
   - **Activation mode:** `Over The Air Activation (OTAA)`
   - **Additional LoRaWAN class capabilities:** `None (Class A only)`
3. Set provisioning:
   - **End Device ID:** `edge-bridge-01` (then repeat with `edge-bridge-02`)
   - **JoinEUI (AppEUI in older docs):** `00 00 00 00 00 00 00 00`
   - **DevEUI:** Auto-generate (each edge bridge gets a unique DevEUI)
   - **AppKey:** Auto-generate (each edge bridge gets a unique AppKey)
4. **Record the DevEUI, JoinEUI, and AppKey for each edge bridge.** These go into `edge_node/config.h`.
   - ⚠️ **Byte order matters:** DevEUI and JoinEUI must be stored in **LSB** order in firmware. AppKey in **MSB** order. Use [Mobilefish EUI converter](https://www.mobilefish.com/download/lora/eui_key_converter.html) to convert.
   - Example DevEUI conversion: TTN shows `70 B3 D5 7E D0 05 A1 2B` → firmware array (LSB) `{ 0x2B, 0xA1, 0x05, 0xD0, 0x7E, 0xD5, 0xB3, 0x70 }`.
5. Click **Register end device**. Repeat for the second edge bridge.

### Step 7A — LMIC Region + Pinmap (Edge Bridge Nodes Only)

1. Edit MCCI LMIC `lmic_project_config.h` and ensure AS923 is selected:
   - Comment out `#define CFG_us915 1`
   - Comment out `#define CFG_au915 1`
   - Uncomment `#define CFG_as923 1`
   - Typical locations (confirm on each lab PC):
     - Windows: `%USERPROFILE%\\Documents\\Arduino\\libraries\\MCCI_LoRaWAN_LMIC_library\\project_config\\lmic_project_config.h`
     - macOS/Linux: `~/Documents/Arduino/libraries/MCCI_LoRaWAN_LMIC_library/project_config/lmic_project_config.h`
2. In `edge_node/config.h`, define the LMIC pin map for the T-Beam LoRa radio (NSS, RST, DIO).
3. Minimum required fields:

```cpp
// Baseline T-Beam 923 profile (validate on your board revision):
#define RADIO_NSS   18
#define RADIO_RST   23
#define RADIO_DIO0  26
#define RADIO_DIO1  33
#define RADIO_DIO2  32

const lmic_pinmap lmic_pins = {
  .nss = RADIO_NSS,
  .rxtx = LMIC_UNUSED_PIN,
  .rst = RADIO_RST,
  .dio = { RADIO_DIO0, RADIO_DIO1, RADIO_DIO2 },
};
```

4. Keep SPI assignment aligned with T-Beam board support package defaults for LoRa (`SCK=5`, `MISO=19`, `MOSI=27`, `NSS=18`) unless your board variant differs.
5. Bench validation gate (mandatory): before edge firmware development, verify `RADIO_RST`, `RADIO_DIO0`, and `RADIO_DIO1` against physical wiring continuity and first-join logs. If board wiring differs from baseline profile, update these constants in `edge_node/config.h` before continuing.
6. Do not apply this LMIC setup to sensor/relay nodes.

### Step 8 — Deploy Payload Formatter

1. In TTN Console, select the application → **Payload formatters → Uplink**.
2. Set **Formatter type** to `JavaScript`.
3. Paste the contents of `tools/decoder.js` into the formatter editor.
4. Save. This decodes `BridgeAggV1` frames into readable JSON fields.

### Step 9 — Enable MQTT Integration

1. In TTN Console, select the application → **Integrations → MQTT**.
2. Note the connection details:
   - **Server:** `as1.cloud.thethings.network`
   - **Port:** `1883` (Use port 1883. TLS (8883) requires ussl wrapping and is out of scope for this implementation.)
   - **Username:** `{app-id}@ttn`
   - **Password:** Generate an API key with "Read application traffic" rights
3. **Topic for Pico W subscription:** `v3/{app-id}@ttn/devices/+/up`

### Step 10 — Validation Checklist

| Check | Expected Result |
|-------|-----------------|
| Gateway status in TTN Console | Shows **Connected** |
| OTAA join for both edge bridges | Join-accept events visible in TTN device live data for `edge-bridge-01` and `edge-bridge-02` |
| First uplink after join | Decoded JSON fields match `codec.py` reference output |
| `opaque_payload` bytes | Match original sensor TX log bytes (byte-for-byte) |
| `edge_uptime_s` | Increments correctly across successive uplinks |
| Payload formatter output | All `BridgeAggV1` fields decoded correctly (record count, sensor data) |

---

## Pico W Application Server (MicroPython + Phew!)

### Role
The Pico W is the **application layer only**. It does not handle any LoRaWAN protocol, network server functions, or gateway functions. Its sole job is to consume decoded telemetry from TTN and present it to the user.

### Data ingestion
- Subscribe to TTN MQTT broker using `umqtt.simple`.
- Topic: `v3/{app-id}@ttn/devices/+/up`
- On message received: parse JSON, extract `decoded_payload` fields, store in in-memory dict keyed by `mesh_src_id`.

### Concurrency model (decision-complete)
- Pico W runs a **single-thread cooperative loop** (no blocking dual-loop architecture).
- MQTT path uses `client.check_msg()` with a short socket timeout (<= 200 ms).
- HTTP path uses a non-blocking socket poll function (`http_poll_once()`), called every loop iteration.
- MQTT reconnect policy is mandatory:
  - wrap `mqtt_check_once()` in `try/except OSError`
  - on exception, reconnect with bounded retry delay and log `MQTT_RECONNECT` on success
- `main.py` loop shape:

```python
while True:
    try:
        mqtt_check_once()   # non-blocking
    except OSError:
        mqtt_reconnect_with_backoff()
    http_poll_once()    # non-blocking
    sleep_ms(50)
```

- Do not use blocking calls like `client.wait_msg()` or a blocking HTTP `serve_forever()` loop.

### Dashboard (served from Pico W via Phew!)

Serve a single-page HTML dashboard at `http://{pico-ip}/`. Refresh every 10 seconds via JavaScript `setInterval`.

**Minimum dashboard views:**

| View | Contents |
|------|----------|
| **Topology** | Table of active nodes: node ID, last seen, hop count, parent ID, RSSI |
| **Latest readings** | Per-node: DHT22 temperature in °C and humidity in %RH |
| **Network health** | PDR estimate (uplinks received vs expected), edge bridge join status, WisGate online indicator |

**Implementation stack:**
- MicroPython on Pico W.
- `umqtt.simple` with non-blocking `check_msg()`.
- Phew! (or equivalent MicroPython HTTP server) with non-blocking poll loop (socket timeout <= 200 ms).
- Dashboard HTML embedded as a Python string — no external files needed.
- All state kept in memory (no file system required for MVP).

**Work package (`dashboard/`):**
- **Note:** Install Phew! by copying the `phew/` folder from the Pimoroni GitHub release to the Pico W filesystem before running `main.py`.
- `main.py` — boot, WiFi connect, cooperative scheduler loop (`mqtt_check_once()` + `http_poll_once()`).
- `dashboard.py` — HTML template string with placeholder substitution.
- `state.py` — in-memory node state dict with update and query functions.

---

## Routing, Congestion, and Timing Parameters (Defaults)

| Parameter                  | Value                        | Notes |
|---------------------------|------------------------------|-------|
| Region / frequency         | AS923, 923 MHz               | |
| Mesh LoRa BW/CR/TX         | 125 kHz, 4/5, 17 dBm         | |
| Default SF                 | SF7                          | With SF9 fallback |
| ACK timeout                | Per SF lookup table          | |
| Beacon interval            | 10s                          | |
| Sensor periodic interval   | 120s                         | Low-latency lab default |
| Sensor periodic interval (TTN fair-use profile) | 180s | Use for long runs on public TTN |
| TX jitter (originated telemetry) | 0–2000 ms            | Random per TX |
| Channel-sense method       | RSSI threshold pre-TX         | CAD optional enhancement |
| Channel-sense attempts     | 2                            | Non-ACK packets |
| Channel-sense backoff      | 50–200 ms                    | Random per attempt |
| Busy threshold             | RSSI > -90 dBm               | Tune in field tests |
| Max retries                | 3                            | |
| Retries by congestion state| GREEN=3, YELLOW=2, RED=1    | Mesh nodes |
| Interval multiplier by state | GREEN=1.0x, YELLOW=1.5x, RED=2.0x | Mesh nodes |
| Congestion thresholds      | YELLOW: queue≥60 or ACK fail≥20%; RED: queue≥80 or ACK fail≥40% | 20-attempt window |
| Parent timeout             | 35s                          | |
| Dedup window               | 30s                          | |
| Score weights              | rank 60%, RSSI 25%, queue 15% | Runtime-configurable |
| Switch hysteresis          | score improvement ≥ 8        | |
| Parent switch cooldown     | 20s                          | |
| Orphan hold duration       | 60s                          | |
| Beacon phase offset        | `(node_id % 5) * 2000 ms`    | Applied once at boot |
| Aggregation flush interval | 240s or 7 records            | Low-latency lab default |
| Aggregation flush interval (TTN fair-use profile) | 450s or 7 records | Use with 180s sensor interval |
| RSSI SF7→SF9 threshold     | < −105 dBm                   | Tune in field |
| RSSI SF9→SF7 threshold     | > −95 dBm                    | |
| TWDT timeout (ESP32)       | 30s                          | All T-Beam roles |

---

## Security and Robustness Choices
1. Mesh CRC16 over header — catches corruption and malformed frames. Not cryptographic.
2. LoRaWAN AES-128 session keys enforced at edge bridge uplink — where it matters.
3. `PacketUID` dedup prevents retry storms.
4. TTL prevents routing loops.
5. Node ID uniqueness enforced by `provision.py` + `nodes.csv`, not compile-time.
6. Payload version fields allow schema evolution without relay firmware updates.
7. NVS persistent counters (WDT resets, queue drops, CRC failures) enable post-deployment health analysis without live logging access.

---

## Project File Structure

```
CSC2106-G33/
│
├── shared/                              # Shared protocol definitions (all node types)
│   └── mesh_protocol.h                  #   MeshHeader, BeaconPayloadV1, BridgeAggV1,
│                                        #   ACK timeout PROGMEM table, config struct,
│                                        #   NVS key constants, CRC16,
│                                        #   rank normalization constants/helpers
│
├── sensor_node/                         # Sensor node firmware (T-Beam 923)
│   └── sensor_node.ino                  #   DHT22 driver, SensorPayload packing,
│                                        #   parent selection, TX with retry, WDT
│
├── relay_node/                          # Relay node firmware (T-Beam 923)
│   └── relay_node.ino                   #   CRC16 validation, opaque forwarding,
│                                        #   beacon emission, parent selection, WDT
│
├── edge_node/                           # Edge bridge firmware (T-Beam 923, dual-role)
│   ├── edge_node.ino                    #   Main entry, worker startup, config load
│   ├── mesh_worker.cpp                  #   Mesh sink RX/ACK/dedup/aggregation
│   ├── lorawan_worker.cpp               #   OTAA join + LMIC uplink path
│   ├── radio_arbiter.cpp                #   Single-radio ownership state machine
│   └── config.h                         #   Shared LoRa pinmap + DevEUI/JoinEUI/AppKey
│
├── dashboard/                           # Pico W application server (MicroPython)
│   ├── main.py                          #   Boot, WiFi connect, MQTT subscribe, HTTP serve
│   ├── dashboard.py                     #   HTML template string with placeholder substitution
│   └── state.py                         #   In-memory node state dict (update + query)
│
├── tools/                               # Host-side tooling (Python + JS)
│   ├── codec.py                         #   encode_agg_v1(), decode_agg_v1(), roundtrip_test()
│   ├── decoder.js                       #   TTN uplink payload formatter (deployed to TTN)
│   ├── provision.py                     #   Node ID provisioning via USB serial → NVS
│   ├── collect_ttn.py                   #   Subscribe to TTN MQTT, write ttn_rx.csv
│   ├── collect_serial.py                #   Read sensor USB serial, write sensor_N_tx.csv
│   ├── compute_pdr.py                   #   Merge tx/rx logs, compute PDR per node + E2E
│   ├── check_integrity.py               #   Compare opaque_payload bytes, report corruption
│   ├── compute_latency.py               #   Extract telemetry latency distributions
│   └── plot_results.py                  #   Generate PDR/latency/drop-reason charts
│
├── logs/                                # Experiment data (gitignored, generated at runtime)
│   └── scenario1_baseline_YYYYMMDD_HHMMSS_sensor01_tx.csv
│
├── nodes.csv                            # Node ID registry (maintained by provision.py)
│
└── IMPLEMENTATION_PLAN_v4.md            # This document
```

---

## Implementation Work Packages (Ordered by Dependency)

1. **Shared protocol header:** `shared/mesh_protocol.h` — `MeshHeader`, `BeaconPayloadV1`, `BridgeAggV1`, ACK timeout PROGMEM table, config struct, NVS key constants.

2. **Reference codec:** `tools/codec.py` and `tools/decoder.js`. Roundtrip tests for `BridgeAggV1`. Commit before touching firmware.

3. **Provisioning tooling:** `tools/provision.py`. Establish `nodes.csv`. Commission all nodes before field deployment.

4. **Sensor node firmware** `sensor_node/sensor_node.ino`:
   - DHT22 driver with Adafruit DHT library.
   - `SensorPayload` byte packing per section 3.
   - `BeaconPayloadV1` consume and score logic.
   - SF-keyed ACK timeout lookup.
   - Watchdog + WDT reset logging.
   - Orphan hold queue behaviour.

5. **Relay node firmware** `relay_node/relay_node.ino`:
   - CRC16 ingress validation.
   - Opaque forwarding pipeline.
   - `BeaconPayloadV1` emission with queue/LQ/health.
   - Score-based parent selection with runtime weights.
   - SF-keyed ACK timeout lookup.
   - NVS drop counters.
   - Watchdog + WDT reset logging.
   - Orphan hold queue behaviour.

6. **Edge bridge firmware** `edge_node/`:
   - Rank-0 sink behavior and aggregation buffer.
   - Single-radio arbitration (`MESH_LISTEN` / `LORAWAN_TXRX`).
   - Real OTAA join on T-Beam LoRa radio.
   - LMIC uplink path with airtime guard and retry/drop policy.
   - Queue overflow logging + NVS counters + `edge_uptime_s`.

7. **TTN decoder:** Deploy `tools/decoder.js` as payload formatter. Validate against `codec.py` test vectors.

8. **Pico W application server** `dashboard/`:
   - MQTT subscriber to TTN.
   - Phew!-based dashboard serving topology, readings, and network health.
   - In-memory state management.

9. **Experiment scripts** `tools/`: `collect_ttn.py`, `collect_serial.py`, `compute_pdr.py`, `check_integrity.py`, `compute_latency.py`, `plot_results.py`. Run after E2E path is validated.

---

## PDR Measurement Methodology

PDR (Packet Delivery Ratio) = packets received at TTN / packets attempted by sender. Primary validation metric for the payload-agnostic relay pipeline.

### Ground truth logging — sender side (lab/bench only)

TX logs are captured via USB serial during **lab and bench experiments only**. Field tunnel experiments are not instrumented with USB serial due to physical cable constraints — all tunnel validation relies on TTN-side logs and NVS drop counters retrieved post-run.

Each sensor node logs: `TX | uid={PacketUID} | seq={seq_num} | ts={millis}` to USB serial. Capture with `tools/collect_serial.py` during bench tests. Save to `logs/sensor_{node_id}_tx.csv`.

### Received log — TTN side

`tools/collect_ttn.py` subscribes to TTN MQTT and writes `logs/ttn_rx.csv` in real time during all experiment runs (both bench and tunnel).

Each record: `mesh_src_id`, `mesh_seq`, `edge_uptime_s`, `sensor_type_hint`, `opaque_payload`, `received_at`.

### PDR computation (`tools/compute_pdr.py`)

```
for each sensor node N:
    tx_set = set of seq_nums from sensor_N_tx.csv  (bench) or estimated from seq continuity (tunnel)
    rx_set = set of mesh_seq values in ttn_rx.csv where mesh_src_id == N
    PDR(N) = |rx_set ∩ tx_set| / |tx_set|

end_to_end_PDR = sum(|rx_set ∩ tx_set| for all N) / sum(|tx_set| for all N)
```

Handle 8-bit `seq_num` rollover with modular arithmetic. For tunnel runs without TX logs, estimate `tx_set` from the expected transmit interval and run duration (e.g. 120s interval × 30 min = 15 expected packets per node).

### Payload integrity check (`tools/check_integrity.py`)

For every `PacketUID` in both tx and rx logs: compare original `SensorPayload` hex bytes (from sensor TX log) against `opaque_payload` bytes decoded at TTN. Any mismatch → `CORRUPT | uid={PacketUID}`. Expected result: 0. Any corruption event means the relay is modifying bytes — firmware bug.

### Latency computation

Cannot use wall-clock sync across nodes. Use `edge_uptime_s` (ingress at edge) and TTN `received_at` (egress) to compute edge-to-TTN latency. Estimate mesh-to-edge latency from hop_estimate × per-hop airtime at configured SF.

### Stress test scenarios

**Scenario 1 — Baseline (bench + tunnel):**
All sensor nodes transmit at 120s interval for 30 minutes. No failures. Expected: PDR ≥ 95%, integrity 100%, telemetry latency ≤ 10s.

**Scenario 2 — Burst load (bench):**
All nodes transmit at 30s interval for 10 minutes. Stresses relay queues and dedup. Observe which failure mode dominates via NVS drop counters.

**Scenario 3 — Failure and recovery (bench):**
120s interval. After 10 minutes, power off relay node. Wait 90 seconds. Restore. Measure orphan detection time, re-parent convergence time, PDR during and after failure window.

### Pass/fail thresholds

| Metric | Pass threshold | Rationale |
|--------|---------------|-----------|
| End-to-end PDR (baseline) | ≥ 95% | Industry standard |
| End-to-end PDR (burst load) | ≥ 80% | Degradation acceptable under stress |
| Payload integrity (all scenarios) | 100% | Any corruption = relay is not opaque |
| Telemetry latency (baseline) | ≤ 10s median | Reasonable for 120s interval telemetry |
| PDR recovery after failure | ≥ 95% within 2 beacons | Self-healing must restore baseline |

### Experiment script work package (`tools/`)

| Script | Purpose |
|--------|---------|
| `collect_ttn.py` | Subscribe to TTN MQTT, write ttn_rx.csv |
| `collect_serial.py` | Read sensor USB serial, write sensor_N_tx.csv (bench only) |
| `compute_pdr.py` | Merge tx/rx logs, compute PDR per node and end-to-end |
| `check_integrity.py` | Compare opaque_payload bytes, report corruption events |
| `compute_latency.py` | Extract telemetry latency distributions |
| `plot_results.py` | Generate PDR/latency/drop-reason charts for report |

Log filenames include scenario label and timestamp: `logs/scenario1_baseline_20250601_143000_sensor01_tx.csv`.

---

## Test Cases and Scenarios

1. **Sensor payload integrity:** Relay forwards unchanged DHT22 payload bytes across 2+ hops. Verified by byte-for-byte comparison of TTN-decoded `opaque_payload` against sensor TX log.
2. **DHT22 reading correctness:** Compare `temp_c_x10` and `humidity_x10` fields against reference instruments. Verify ±0.5°C temp and ±2% RH accuracy.
3. **DHT22 read error handling:** Simulate DHT22 read failure. Verify `status = 0x01` and no transmission occurs for that cycle.
4. **Parent selection stability:** No rapid oscillation under RSSI fluctuations < 8 score difference. Verified over 30-minute stable period.
5. **Score weight runtime update:** Change weights via serial, verify reflected in next parent decision without reflash.
6. **SF fallback and recovery:** Attenuate link until SF9 triggers. Verify ACK timeout switches to 1200ms. Remove attenuation, verify SF7 recovery.
7. **Congestion-aware routing:** Under burst load, nodes prefer lower-queue-pct parents when alternatives exist.
8. **Orphan hold and recovery:** Kill parent relay. Verify orphan hold activates. Restore within 60s. Verify queue flushes.
9. **Orphan drop:** Kill parent for > 60s. Verify `ORPHAN_DROP` logged, NVS counter incremented, node recovers after parent restoration.
10. **TTL and dedup:** No packet loop amplification. Duplicate delivery rate = 0 under stable conditions.
11. **WDT reset logging:** Inject artificial hang in test build. Verify NVS `wdt_reset_count` increments and `WDT_RESET` logs on next boot.
12. **TTN end-to-end + airtime profile check:** Both edge bridges join and uplink through WisGate. In low-latency profile (120s/240s), verify projected airtime exceeds 30s/day and document this as public-TTN non-compliant. In TTN fair-use profile (180s/450s, SF7), verify airtime < 30s/day per edge bridge.
13. **Aggregation correctness (steady state + burst):** With default 240s flush and balanced routing, nominal topology should produce ~2 records/uplink on each edge bridge. Under burst load, verify 7-record flush path (`record_count = 7`) decodes correctly.
14. **Queue overflow logging:** Fill uplink queue past capacity. Verify `QUEUE_OVERFLOW` logged with packet UIDs, NVS counter incremented.
15. **Pico W dashboard:** Node topology, latest readings (DHT22 temperature + humidity), and network health display correctly. Values match TTN decoded data.
16. **Payload-agnostic relay verification:** Replace one DHT22 sensor node with a different payload pattern (e.g., random bytes). Relay and edge bridges must forward and aggregate it without error. Only the TTN payload formatter output will differ — the mesh pipeline itself must be unaffected.
17. **TX jitter and channel-sense behavior:** Under synchronized start conditions, verify TX timestamps spread over jitter window and busy-sense events trigger bounded backoff (RSSI method; CAD optional if wired/supported).
18. **Congestion state transitions:** Induce queue/ACK stress and verify nodes transition GREEN→YELLOW→RED and recover back with correct logs.
19. **Adaptive retries:** In RED state, verify effective retries reduce to 1; in GREEN return to 3.
20. **Beacon staggering:** On boot, verify beacon transmissions from all 5 mesh-participating nodes are phase-offset across ~10s window.
21. **Edge airtime guard:** In TTN fair-use profile, force projected daily airtime above threshold and verify `AIRTIME_DEFER` behavior and 86400s window reset.
22. **ESP32 resource gate:** Record flash size and heap watermark for sensor/relay/edge builds. Verify runtime heap and flash thresholds in the ESP32 resource gate are met.

---

## Acceptance Criteria

1. Real `sensor → (relay optional) → edge bridge → WisGate → TTN → Pico W` pipeline works end-to-end for both edge bridges.
2. Airtime budget is explicitly validated against active profile: low-latency profile is documented as TTN fair-use non-compliant; TTN fair-use profile is measured/projection-checked to stay < 30s/day per edge bridge at SF7.
3. **Relay is payload-agnostic:** zero corruption events across all test scenarios. Relay firmware must work unchanged with any sensor payload variant.
4. Structured dataset across all three stress test scenarios with PDR per node, payload integrity, telemetry latency distribution, and NVS drop-reason breakdown. All results evaluated against pass/fail thresholds.
5. Self-healing demonstrated: orphan recovery time and re-parent convergence time measured.
6. WDT behaviour verified on bench before field deployment.
7. Pico W dashboard shows topology, node health, latest readings (temperature + humidity).
8. Report includes deployment guidance: node spacing, SF selection for tunnel geometry, score weight tuning, congestion handling.
9. Congestion-control policy (jitter/channel-sense/state-based interval+retry) is validated with test cases 17–20.
10. TTN fair-use profile edge airtime guard is validated (test case 21) for long-duration public TTN operation.
11. ESP32 flash/heap usage is measured and recorded for sensor/relay/edge builds (test case 22), with mitigation actions documented if thresholds are exceeded.

---

## Assumptions and Constraints

1. Operator has WisGate admin access and TTN application/device credentials.
2. Edge hardware confirmed as **two single-board dual-role T-Beam edge bridges**, both with 923MHz SX1276 radios.
3. All nodes are mains-powered (5V USB supply). No battery constraints.
4. AS923 is mandatory.
5. TTN fair use (30s/day/device) applies on public TTN. Low-latency defaults (120s/240s) exceed this budget; use TTN fair-use profile (180s/450s, SF7) or private network server for long-duration runs.
6. Simulation-only uplink does not count as acceptance evidence.
7. Score weights `[60, 25, 15]` are starting defaults — expect field tuning given tunnel RF variability.
8. Pico W connects to the same WiFi/LAN as the TTN MQTT endpoint. Pico W does not need a public IP — TTN MQTT broker is cloud-hosted.
9. TX log capture via USB serial is feasible only for bench/lab experiments. Tunnel PDR estimation uses TTN-side logs and sequence continuity analysis.

---

## Open Questions (Resolved)
 
1. **Pico W WiFi connectivity:** Confirmed as **above ground** (lab simulation). No tunnel RF concerns. Pico W connects to standard lab WiFi.
2. **Single-radio limitation on T-Beam:** **Decision: Use single-board dual-role edge bridges with time-sliced arbitration.**
   - Default state: `MESH_LISTEN` for downstream parent/sink behavior
   - Flush state: `LORAWAN_TXRX` window for one aggregate uplink + RX windows
   - Return immediately to `MESH_LISTEN` after each LoRaWAN window
   - **Rationale:** Preserves required dual-role behavior while controlling mesh blackout duration.
3. **WisGate mode:** **Decision: Legacy Packet Forwarder.**
   - **Rationale:** Simplest configuration for lab/educational use. "It just works" without certificate management.
