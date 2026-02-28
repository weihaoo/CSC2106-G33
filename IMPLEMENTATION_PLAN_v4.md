# Detailed Implementation Spec v5: Sensor Node -> Relay Node -> Dual-Role Edge Node -> WisGate -> TTN -> Pico W (Singapore AS923)

> **Revision notes (v2):** Addresses six critical issues and seven warnings from v1 review: TTN airtime budget analysis, ATmega328P SRAM memory map, ACK timeout table keyed to SF, node ID provisioning mechanism, orphan queue policy, and watchdog timer policy. Additional improvements cover score weight runtime configurability, SF selection criteria, uptime tick in bridge payload, reference encoder/decoder workflow, queue overflow logging, and frame count for E2E latency.
>
> **Revision notes (v3):** Adds full PDR Measurement Methodology section covering: definition and rationale, ground truth logging on sender and TTN sides, PDR/integrity/latency computation scripts, three structured stress test scenarios (baseline, burst load, failure/recovery), and explicit pass/fail thresholds for each metric.
>
> **Revision notes (v4):** Resolves all previously missing definitions: `SensorPayload` full byte layout for MQ-135 + DS18B20, `BridgeAggV1` concrete byte layout with `sensor_type_hint` per record, alert threshold EEPROM placeholder with field calibration instructions, MQ-135 60s warm-up guard, EEPROM table corrected to include WDT reset counter at 0x14 and alert threshold at 0x1A–0x1B, missing Test Cases section heading restored, power budget section added (battery + mains split), dashboard implementation spec updated for Pico W-compatible HTTP server, PDR methodology updated to lab-only serial logging, hardware spec updated to confirmed sensors (MQ-135 + DS18B20), library table corrected (DHT removed). **Architecture change:** dual Pico W replaced with single Pico W acting as application server only — WisGate forwards to TTN (network server), TTN pushes via MQTT/webhook to Pico W application server.
>
> **Revision notes (v5):** Sensor simplified to DHT22 only (MQ-135 gas sensor and DS18B20 removed). Alert uplink path (`BridgeUplinkV1`) removed — all data is telemetry-only, sent via aggregated `BridgeAggV1`. Sensor interval changed from 15s to 120s. Network sized to **6 mesh nodes: 3 sensor, 1 relay, 2 edge bridges** (both edge bridges are LoRaWAN end devices registered on TTN). All nodes are mains-powered (USB/5V supply) — battery power budget section removed. SensorPayload redesigned for DHT22 (temperature + humidity). EEPROM alert threshold fields removed. Payload-agnostic relay design emphasised as the core architectural goal.

---

## Brief Summary
 
 > **SIMULATION DISCLAIMER:** This entire implementation is designed for an **above-ground lab simulation** of the underground scenario. All "underground" references (tunnels, shafts) refer to the *target* use case, but the actual code and hardware will be tested in a classroom/lab environment. All radio testing is done in free space or through building walls, not actual earth.
 
 **Core design principle — payload-agnostic relay:** Relay nodes forward mesh packets without inspecting or decoding sensor payloads. This means the relay firmware never changes regardless of what sensor type is attached to a sensor node. A sensor node could be replaced with a completely different sensor (e.g., CO₂, PM2.5, soil moisture) and relays would continue forwarding correctly without any firmware update. This is the primary architectural goal of the project.

This implementation builds a two-tier underground LoRa mesh using **6 nodes**:

| Role | Count | Hardware | Purpose |
|------|-------|----------|---------|
| **Sensor node** | 2 | LilyGO T-BEAM-AXP2101-V1.2 (LoRA 923MHz) + DHT22 | Generate telemetry, route upstream |
| **Relay node** | 1 | LilyGO T-BEAM-AXP2101-V1.2 (LoRA 923MHz) | Forward packets opaquely (payload-agnostic) |
| **Edge bridge** | 2 | LilyGO T-BEAM-AXP2101-V1.2 (LoRA 923MHz) | Mesh sink + LoRaWAN end device uplinking to TTN |

1. Three sensor nodes read DHT22 (temperature + humidity) and route upstream through mesh parents.
2. One relay node forwards packets **opaquely** — it never reads sensor payloads. This is the core payload-agnostic design.
3. Two edge bridge nodes aggregate mesh telemetry and uplink to TTN via LoRaWAN (AS923). Both are registered as separate LoRaWAN end devices.
4. TTN acts as the LoRaWAN network server. A single Pico W runs the application server (Phew! + MicroPython), consuming data from TTN via MQTT.
5. All nodes are mains-powered (5V USB supply). No battery power budget constraints.
6. The final deliverable is successful real end-to-end uplink (not simulated), plus measured latency/PDR/throughput and a minimal monitoring dashboard.

---

## System Architecture Overview

### LoRa Mesh Network Topology (6 nodes)

```
                   ╔════════════════════════════════════════════════════════╗
                   ║        UNDERGROUND LoRa MESH NETWORK            ║
                   ║     6 nodes — 3 sensor, 1 relay, 2 edge bridge   ║
                   ║     Core principle: payload-agnostic relay        ║
                   ╠════════════════════════════════════════════════════════╣
                   ║                                                        ║
                   ║  [Sensor 1]   [Sensor 2]   [Sensor 3]                  ║
                   ║   (DHT22)      (DHT22)      (DHT22)                    ║
                   ║     │            │            │                        ║
                   ║     │ LoRa       │ LoRa       │ LoRa                   ║
                   ║     └───────────┤            │                        ║
                   ║               │            │                        ║
                   ║               ▼            │                        ║
                   ║            [Relay 1]       │                        ║
                   ║            forwards        │                        ║
                   ║            opaquely        │                        ║
                   ║               │            │                        ║
                   ║          ┌────┴────┐       │                        ║
                   ║          │         │       │                        ║
                   ║          ▼         ▼       ▼                        ║
                   ║  ┌──────────────┐  ┌──────────────┐        ║
                   ║  │ EDGE BRIDGE 1│  │ EDGE BRIDGE 2│        ║
                   ║  │ (ESP32)      │  │ (ESP32)      │        ║
                   ║  │ Radio A:mesh │◄─LoRa mesh─►│ Radio A:mesh │        ║
                   ║  │ Radio B:WAN  │  │ Radio B:WAN  │        ║
                   ║  └──────┬───────┘  └──────┬───────┘        ║
                   ╚═════════╪══════════════════════════════╪═══════════════╝
                             │                              │
                LoRaWAN AS923│(OTAA, AES-128)  LoRaWAN AS923│
                             ▼                              ▼
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

 Sensor 1 ──┬────► Relay 1 ──────┬──────► EDGE BRIDGE 1 ──► LoRaWAN ──► TTN
                │                     │
 Sensor 2 ──┘                     │
                                      │
 Sensor 3 ──────────────────────┘──► EDGE BRIDGE 2 ──► LoRaWAN ──► TTN

 • Sensor 1 & 2 route through Relay 1 (default path)
 • Sensor 3 routes directly to Edge Bridge 2 (or via Relay 1 as backup)
 • Score-based parent selection determines actual routing at runtime

 KEY DESIGN PRINCIPLE — PAYLOAD-AGNOSTIC RELAY:
 • Relay 1 NEVER inspects sensor payloads
 • It forwards the opaque byte blob as-is (no decode, no parse)
 • If Sensor 1’s DHT22 was swapped for a CO₂ sensor tomorrow,
   Relay 1 would continue forwarding without any firmware change
 • Only the edge bridges and TTN decoder need to know the payload schema
```

**Role of each component:**
- **WisGate:** LoRaWAN gateway only. Forwards uplink frames to TTN. No application logic.
- **TTN:** Network server. Handles OTAA join, MIC verification, frame counter, payload decoding via formatter. Pushes decoded data to Pico W.
- **Pico W:** Application server only. Receives TTN data via MQTT subscription or webhook. Runs a Phew!-based dashboard. No LoRaWAN stack.

---

## Hardware Specification

### Node Hardware (Sensor & Relay Nodes)

| Component | Model | Specifications |
|-----------|-------|----------------|
| **Microcontroller** | Cytron Maker UNO | ATmega328P, 5V, 16MHz, 2KB SRAM, 32KB Flash |
| **LoRa Shield** | Cytron LoRa-RFM Shield | RFM95W (SX1276), SPI interface |
| **Temperature & Humidity Sensor** | DHT22 (AM2302) | Digital output, ±0.5°C temp accuracy, ±2% RH accuracy |

**Confirmed sensor assignment:** Each sensor node carries a **DHT22** sensor providing both temperature and humidity readings.

**Power:** All nodes are connected to **mains power** (5V USB adapter or regulated 5V supply) at all times. No battery operation, no sleep mode required.

**Memory Constraints:**
- ATmega328P SRAM: **2,048 bytes total**
- Arduino core overhead: ~350–400 bytes
- Available for application: ~1,600–1,700 bytes
- **LMIC LoRaWAN library risk:** 600–800 bytes if used → may cause stack overflow
- **Recommendation:** Use Arduino-LoRa (Sandeep Mistry) for mesh nodes — lightweight raw LoRa driver. Reserve LMIC for ESP32 edge node only.

### Gateway Hardware

| Component | Model | Specifications |
|-----------|-------|----------------|
| **LoRaWAN Gateway** | RAK WisGate Edge Lite 2 (RAK7268) ×2 | 8-channel LoRa concentrator, Ethernet/WiFi backhaul, AS923 support |

### Edge Bridge Node

**Requirement:** Dual-radio architecture
- Radio A: SX1276 (mesh transceiver, 923 MHz, Arduino-LoRa library) on **VSPI** bus.
- Radio B: SX1276 (LoRaWAN transceiver, AS923, LMIC library) on **HSPI** bus.
- **MCU:** ESP32 (dual-core, FreeRTOS, 520KB SRAM, 4MB flash) — required for dual-task firmware

**Note:** Maker UNO cannot serve as edge bridge due to insufficient SRAM for dual-stack operation.

### Application Server

| Component | Model | Role |
|-----------|-------|------|
| **Pico W** | Raspberry Pi Pico W | Application server (MicroPython + Phew!). Subscribes to TTN MQTT or receives TTN webhook. Serves dashboard. No LoRaWAN stack. |

---

## Software Libraries

### Sensor & Relay Nodes (ATmega328P)

| Library | Purpose | SRAM Impact | Used By |
|---------|---------|-------------|---------|
| **Arduino-LoRa** (Sandeep Mistry) | Raw LoRa radio driver | ~50–100 bytes | All mesh nodes |
| **SPI.h** | Hardware SPI communication | ~20 bytes | All mesh nodes |
| **DHT sensor library** (Adafruit) | DHT22 temperature + humidity driver | ~100 bytes | Sensor nodes |
| **mesh_protocol.h** | Custom mesh protocol definitions | ~200–300 bytes | All mesh nodes |

### Edge Node (ESP32)

| Library | Purpose | SRAM Impact |
|---------|---------|-------------|
| **Arduino-LoRa** (Sandeep Mistry) | Radio A mesh transceiver driver | ~100 bytes |
| **Arduino-LMIC** (MCCI) | Radio B LoRaWAN stack (OTAA, AES, duty cycle) | ~600–800 bytes |
| **FreeRTOS** | Dual-task firmware (mesh_task + lorawan_task) | Built into ESP32 Arduino core |

**Critical:** DO NOT use LMIC on ATmega328P. It will exhaust SRAM. LMIC runs on ESP32 edge node only.

### Application Server (Pico W)

| Library | Purpose |
|---------|---------|
| **MicroPython** | Runtime |
| **umqtt.simple** | MQTT client to subscribe to TTN application MQTT broker |
| **Phew!** (or equivalent MicroPython non-blocking HTTP server) | Lightweight HTTP server for dashboard |

---

## TTN Airtime Budget Analysis (Critical — Read Before Proceeding)

This analysis must be resolved before committing to the bridged uplink architecture. TTN fair use policy allows **30 seconds of airtime per LoRaWAN device per day**. Each edge bridge node is a single LoRaWAN device.

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
- Load split: bridge 1 gets ~2 sensors, bridge 2 gets ~1 sensor

In steady state, the **time trigger dominates**:
- **Bridge 1:** ~4 records/uplink, 360 uplinks/day → 360 × 133.4 ms ≈ **~48.0 s/day**
- **Bridge 2:** ~2 records/uplink, 360 uplinks/day → 360 × 92.4 ms ≈ **~33.3 s/day**

Result: both bridges exceed TTN 30s/day fair use under current low-latency defaults.

### Operating profiles

1. **Low-latency lab profile (default in this plan):**
   - `sensor_interval_s = 120`
   - `flush_interval_s = 240`
   - Suitable for latency/convergence experiments, but **not TTN fair-use compliant** for 24h continuous operation.
2. **TTN fair-use profile (optional):**
   - `sensor_interval_s = 180`
   - `flush_interval_s = 450`
   - Expected busy bridge at SF7: ~5 records/uplink, 192 uplinks/day → 192 × 153.9 ms ≈ **~29.5 s/day**
   - Requirement: keep edge uplink at SF7; prolonged SF9 operation will exceed 30s/day.

**Decision:** Keep the low-latency profile for bench performance experiments. Use TTN fair-use profile when running long-duration tests on public TTN.

---

## End-to-End Data Path (What Happens Per Packet)

1. Sensor node samples DHT22, packs temperature and humidity into `SensorPayload`.
2. Sensor adds `MeshHeader` and sends `PKT_TYPE_DATA` to current parent.
3. Relay receives frame, validates CRC, deduplicates, ACKs previous hop, decrements TTL, forwards payload **unchanged** to its parent.
4. Edge node receives final mesh frame, ACKs previous hop. Stores in aggregation buffer.
5. On flush trigger (240s elapsed or 7 records buffered): edge packs `BridgeAggV1` and transmits LoRaWAN uplink via OTAA session to WisGate.
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

Used when the edge bridge flushes its aggregation buffer. All sensor data is DHT22 telemetry.

**Header (3 bytes):**

| Offset | Field          | Size   | Notes |
|--------|---------------|--------|-------|
| 0      | schema_version | 1 byte | Always 0x02 |
| 1      | bridge_id      | 1 byte | Edge node ID |
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

`score_weights[3]`: runtime-configurable weights for `[rank, rssi, queue]`, integers summing to 100. Default `[60, 25, 15]`. Stored in EEPROM. Adjustable via serial command without reflash.

---

## ATmega328P SRAM Memory Map (Critical Constraint)

**Total SRAM: 2048 bytes.** Stack consumes ~200–400 bytes. Usable for static allocation: ~1600 bytes.

| Structure | Size (bytes) | Notes |
|-----------|-------------|-------|
| MeshHeader (RX buffer) | 10 | Single working buffer |
| SensorPayload (TX) | 7 | DHT22 payload (7 bytes) |
| Neighbor table (8 entries × 8B) | 64 | id, rssi, rank, queue_pct, lq, health, last_seen_s, flags |
| Dedup table (16 entries × 4B) | 64 | src_id + seq_num + timestamp (16-bit seconds) |
| TX retry buffer | 32 | One packet held for retry |
| Uplink queue (relay only, 4 × 32B) | 128 | LoRa frame staging buffer |
| BeaconPayloadV1 | 4 | Current outbound beacon payload |
| Config struct | 25 | All runtime config fields (`flush_interval_s` is uint16) |
| Score weights | 3 | Fast-access copy from EEPROM |
| Congestion state | 1 | `CONGEST_GREEN/YELLOW/RED` |
| ACK-fail bit window | 3 | Packed 20-attempt failure history |
| ACK-fail running count | 1 | Fast `ack_fail_pct` update |
| Effective retries cache | 1 | State-derived retries |
| Channel-sense RSSI scratch | 1 | Last live channel RSSI sample |
| LoRa library buffers (Arduino-LoRa) | ~100 | Measured; much lighter than LMIC |
| Stack headroom | 300 | Conservative estimate |
| **Total estimated** | **~745** | Leaves ~1300 bytes margin with Arduino-LoRa |

**Action items:**
- Profile actual stack depth with a watermark canary before final firmware.
- Dedup table uses 16-bit timestamps (seconds, rolls over in ~18 hours) to save 2 bytes/entry.
- Neighbor table capped at 8 entries; evict by LRU (`last_seen_s`) if full.
- Do not use dynamic memory allocation (malloc/free) anywhere in firmware.

---

## ATmega328P Flash Budget Gate (Mandatory)

SRAM is not the only constraint on Maker UNO nodes. Relay firmware in particular can approach 32 KB flash limits once forwarding, scoring, logging, and congestion logic are enabled.

1. Run `avr-size` on sensor and relay firmware after each major feature merge.
2. Record `Program` size in build logs.
3. Alert threshold: if program size exceeds **90%** of flash (~28.8 KB), freeze feature additions and reduce logging/debug code before continuing.
4. Hard stop: do not ship if program size exceeds board flash limit after bootloader allowance.

---

## Node ID Provisioning (Not Compile-Time)

Node IDs are **not** compile-time constants. Provisioning mechanism:

1. Factory default: node boots with `node_id = 0xFF` (unprovisioned sentinel).
2. Operator connects USB serial and runs `provision.py --node-id 0x01 --role sensor`.
3. Script sends `SETID:<id>` command. Node stores ID in EEPROM 0x00 and reboots.
4. On boot: if EEPROM[0x00] == `0xFF`, node halts and blinks error. If 0x01–0xFE, proceeds.
5. `provision.py` maintains `nodes.csv` mapping node IDs to physical locations and flash dates.
6. Duplicate ID detection: relays log a `CONFLICT` event if two sources share the same ID.

**EEPROM layout:**

| Address   | Field               | Size | Notes |
|-----------|--------------------|----- |-------|
| 0x00      | node_id             | 1B   | 0xFF = unprovisioned |
| 0x01      | role                | 1B   | 0x01=sensor, 0x02=relay, 0x03=edge |
| 0x02–0x03 | feature_flags       | 2B   | Reserved |
| 0x04      | sf_default          | 1B   | Spreading factor 7–12 |
| 0x05      | sf_fallback         | 1B   | Fallback SF on link degradation |
| 0x06–0x08 | score_weights       | 3B   | [rank, rssi, queue], sum to 100 |
| 0x09–0x0A | flush_interval_s    | 2B   | Edge aggregation flush interval in seconds, uint16 little-endian |
| 0x0B–0x0F | reserved            | 5B   | Do not use |
| 0x10–0x13 | overflow_drop_count | 4B   | Persistent queue overflow counter |
| 0x14      | wdt_reset_count     | 1B   | Incremented on each WDT reboot |
| 0x15–0x18 | crc_fail_count      | 4B   | Persistent CRC failure counter (relay nodes) |
| 0x19–0x1B | reserved            | 3B   | |

`flush_interval_s` uses 2 bytes specifically to support values >255 (for example TTN fair-use profile value 450s).

---

## Watchdog Timer Policy

All node types must implement hardware watchdog. Non-negotiable for unattended underground deployment.

1. **Timeout:** 8 seconds (maximum supported by ATmega328P hardware WDT).
2. **Kick locations:** Main loop top, post-TX callback, post-RX callback.
3. **Do NOT kick inside:** retry loops, blocking wait states, or sleep sequences.
4. **On watchdog reset:** read reset cause register. If WDT reset, increment EEPROM[0x14] before normal boot. Log `WDT_RESET` on first serial output after boot.
5. **ESP32 edge node:** FreeRTOS TWDT per task. Both `mesh_task` and `lorawan_task` register with TWDT. Timeout: 30 seconds.
6. **Safe mode threshold:** If EEPROM[0x14] exceeds 5 since last manual clear, node enters safe mode (health alert only, no sensor reads) and waits for operator reset.
   - ATmega328P has no RTC in this design, so a true 24-hour rolling window is not enforced in firmware.

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
6. Radio B (LoRaWAN uplink via LMIC): disable ADR for deterministic airtime budgeting. After OTAA join in `lorawan_task`, call `LMIC_setAdrMode(0)` and keep fixed DR/SF7 for TTN fair-use profile runs. Re-enable ADR only for explicit ADR experiments.

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
5. On expiry without parent: drop held queue, increment EEPROM `overflow_drop_count`, log `ORPHAN_DROP`. Enter beacon-listen-only mode.
6. Held packets retain `PacketUID`. If re-delivered after recovery, next-hop dedup will catch duplicates — acceptable behaviour.

---

## Congestion Control Policy (Decision-Complete)

This policy is applied to **mesh nodes** (sensor + relay). It is designed to reduce collisions during burst traffic while staying within ATmega328P memory/timing limits.

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
6. Beacon staggering (all mesh nodes):
   - `beacon_offset_ms = (node_id % 6) * 1667`
   - apply once at boot so 6 nodes spread across a 10s beacon window.

### Feasibility note

Worst-case sensing+backoff wait here is < 500 ms, well below 8s WDT timeout. No extra WDT kick points are required.

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
   - Set explicit shield pin mapping instead of relying on library defaults:
     - `LoRa.setPins(MESH_SS_PIN, MESH_RST_PIN, MESH_DIO0_PIN);`
   - Baseline Cytron Maker UNO + Cytron LoRa-RFM Shield profile:
     - `MESH_SS_PIN=10`, `MESH_RST_PIN=9`, `MESH_DIO0_PIN=2`
   - If your shield revision differs, update these constants from board silk-screen/wiring check before firmware testing.

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
   - Weights from EEPROM[0x06–0x08]. Adjustable via serial without reflash.
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
   - Rationale: for temperature/humidity telemetry, the latest reading is always more valuable than a stale one. Buffering multiple readings is not worth the SRAM cost on ATmega328P.
   - Log `TX_OVERWRITE` with the discarded `PacketUID` for post-analysis.
   - This should rarely trigger at 120s intervals (3 retries × 600ms ACK timeout = 1.8s worst case at SF7), but protects against sustained link failure scenarios.

8. **Reliability:** Dedup by `(src_id, seq_num)` with 30s window, 16-entry table. TTL default 10.

9. **Self-healing:** Parent timeout 35s. Promote best backup from neighbor table. Orphan policy as above.

10. **Watchdog:** Kick at main loop top and post-TX. 8s timeout.

---

## Relay Node Implementation (How + Why)

### Responsibilities
1. Forward packets opaquely — no inspection of sensor payload bytes.
2. Maintain neighbor table and upstream parent.
3. Send hop ACK quickly.
4. Advertise congestion via beacon metadata.

### Implementation Steps

1. **Mesh radio pin init (mandatory before `LoRa.begin()`):**
   - Set explicit shield pin mapping:
     - `LoRa.setPins(MESH_SS_PIN, MESH_RST_PIN, MESH_DIO0_PIN);`
   - Use the same Cytron baseline pin profile as sensor nodes unless bench wiring confirms otherwise.

2. **Ingress pipeline:**
   - Validate minimum length (10 bytes).
   - Drop self-originated frames.
   - Dedup check.
   - CRC16 validation over bytes 0–7. Drop on mismatch, increment EEPROM `crc_fail_count`, log `CRC_FAIL`.
   - ACK previous hop if `ACK_REQ` set.

3. **Forwarding pipeline:**
   - Drop if TTL == 0. Log `TTL_DROP` with `PacketUID`.
   - Drop if no parent → enter orphan policy.
   - Set `prev_hop = MY_NODE_ID`, set `FWD` flag.
   - Forward bytes 0–(payload_len+9) unchanged. Do not inspect bytes 10–N.

4. **Queue-aware beacon:** Compute `queue_pct`, `link_quality` (RSSI EWMA), `parent_health` (ACK success rate over last 20 attempts). Emit in `BeaconPayloadV1` every 10s.
   - Apply startup beacon phase offset: `beacon_offset_ms = (node_id % 6) * 1667`.
   - Relay congestion state uses this node's own forwarding ACK-failure window (`ack_fail_window = 20`) plus local `queue_pct` as defined in the shared congestion policy.

5. **Parent algorithm:** Use the same shared normalization as sensor nodes (`MAX_RANK=4`, `rank_score` mapping above, `rssi_norm` in 0–100), then:
   - `score = w_rank * rank_score + w_rssi * rssi_norm + w_queue * (100 - queue_pct)`
   - Switch with hysteresis + 20s dwell.

6. **Failure handling:** ACK failures ≥ 5 consecutive → re-parent. No backup → orphan state.

7. **Logging:** Per-packet events with `PacketUID`, RSSI, forward decision, drop reason. Persist drop counters in EEPROM.

8. **Watchdog:** 8s hardware WDT. Kick at loop top and post-TX.

---

## Dual-Role Edge Node Implementation (How + Why)

### Responsibilities
1. Rank-0 mesh sink.
2. Bridge mesh data to LoRaWAN uplink with aggregation.
3. Maintain mesh responsiveness during LoRaWAN join/uplink windows.

### Hardware Model
- Radio A: SX1276, mesh transceiver, 923 MHz, Arduino-LoRa. **Connected to VSPI (SCK=18, MISO=19, MOSI=23, CS=5).**
  - **Explicit binding required:** Create `SPIClass vspi(VSPI); vspi.begin(18, 19, 23, 5); LoRa.setSPI(vspi);` before `LoRa.begin()`.
  - **Pin mapping:** `CS=5`, `RST=25`, `DIO0=32`.
- Radio B: SX1276, LoRaWAN transceiver, LMIC, AS923 OTAA. **Connected to HSPI (SCK=14, MISO=12, MOSI=13, CS=15).**
  - **Explicit binding required:** Create `SPIClass hspi(HSPI); hspi.begin(14, 12, 13, 15);` before `os_init()`. Pass this SPI instance to LMIC's HAL (method depends on LMIC version, check `hal.cpp` for SPI binding API).
  - **Pin mapping:** `CS=15`, `RST=27`, `DIO0=26`, `DIO1=33`.
- MCU: ESP32, dual-core FreeRTOS.
- **Separate SPI buses and distinct RST/DIO pins** ensure no bus contention or hardware interrupts conflicts between Mesh and LoRaWAN stacks.

### Firmware Structure

1. **`mesh_task` (Core 0):**
   - Receive mesh frames on Radio A.
   - ACK within half of sender's ACK timeout.
   - Dedup check.
   - Push to aggregation buffer. On flush trigger (240s elapsed or 7 records full), pack `BridgeAggV1`, push to uplink queue.

2. **`lorawan_task` (Core 1):**
   - OTAA join with exponential backoff.
   - On join success: `LMIC_setAdrMode(0)` for fixed-airtime operation (TTN fair-use profile).
   - Dequeue aggregated telemetry frames. Use a non-blocking poll `xQueueReceive(uplink_q, &frame, 0)` with a short yield `vTaskDelay(pdMS_TO_TICKS(2))` in the task tight loop to ensure `os_runloop_once()` runs frequently to service LMIC timing windows. Do not use `portMAX_DELAY` as it will block LMIC.
   - Optional TTN airtime guard (enabled in TTN fair-use profile):
     - Track `daily_airtime_ms` and `airtime_window_start_s` (uptime-based 86400s window).
     - Estimate airtime before each uplink from current SF + payload bytes.
     - If `daily_airtime_ms + est_airtime_ms > 28000`, defer send for 60s (`AIRTIME_DEFER`) and keep current frame in task-local `pending_frame`.
     - Reset `daily_airtime_ms` when uptime window reaches 86400s.
   - Telemetry aggregate → unconfirmed uplink.
   - On 3 consecutive uplink failures → log `UPLINK_FAIL` with `PacketUID` list, drop frame, continue.
   - TWDT kick each iteration.

3. **Inter-task queue contract (mandatory):**
   - Queue handle: `QueueHandle_t uplink_q` (created once in `edge_node.ino` before task creation).
   - Element type:

```cpp
typedef struct {
  uint8_t len;             // serialized BridgeAggV1 length (1..101)
  uint8_t bytes[101];      // serialized BridgeAggV1 bytes
  uint8_t record_count;    // 1..7
  uint16_t uid_list[7];    // PacketUID list for overflow/failure logging
} BridgeAggFrame;
```

   - Creation: `uplink_q = xQueueCreate(32, sizeof(BridgeAggFrame));`
   - Producer: `mesh_task` only (`xQueueSend`).
   - Consumer: `lorawan_task` only (`xQueueReceive`).
   - No shared mutable payload buffer across tasks.

4. **Queue policy:** Depth 32. On full queue, drop oldest aggregate (`xQueueReceive` then enqueue newest), log `QUEUE_OVERFLOW` with dropped packet UIDs, increment EEPROM `overflow_drop_count`.

5. **LMIC thread-safety rule:** Only `lorawan_task` may call LMIC APIs (`os_runloop_once`, `LMIC_setTxData2`, join handlers).

6. **Observability:** Log join state transitions, every uplink attempt (result + `PacketUID` list + `edge_uptime_s`), all overflow events.

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
> **Scope note:** In this project, LoRaWAN setup applies to **edge bridge ESP32 nodes only**. Sensor and relay Maker UNO nodes use raw LoRa mesh (Arduino-LoRa) and must not run LMIC.

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

Repeat for **both** edge bridges (`edge-bridge-01` and `edge-bridge-02`):

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
   - **DevEUI:** Auto-generate (each bridge gets a unique DevEUI)
   - **AppKey:** Auto-generate (each bridge gets a unique AppKey)
4. **Record the DevEUI, JoinEUI, and AppKey for each bridge.** These go into each bridge's `edge_node/config.h`.
   - ⚠️ **Byte order matters:** DevEUI and JoinEUI must be stored in **LSB** order in firmware. AppKey in **MSB** order. Use [Mobilefish EUI converter](https://www.mobilefish.com/download/lora/eui_key_converter.html) to convert.
   - Example DevEUI conversion: TTN shows `70 B3 D5 7E D0 05 A1 2B` → firmware array (LSB) `{ 0x2B, 0xA1, 0x05, 0xD0, 0x7E, 0xD5, 0xB3, 0x70 }`.
5. Click **Register end device**. Repeat for the second bridge.

### Step 7A — LMIC Region + Pinmap (Edge Bridges Only)

1. Edit MCCI LMIC `lmic_project_config.h` and ensure AS923 is selected:
   - Comment out `#define CFG_us915 1`
   - Comment out `#define CFG_au915 1`
   - Uncomment `#define CFG_as923 1`
   - Typical locations (confirm on each lab PC):
     - Windows: `%USERPROFILE%\\Documents\\Arduino\\libraries\\MCCI_LoRaWAN_LMIC_library\\project_config\\lmic_project_config.h`
     - macOS/Linux: `~/Documents/Arduino/libraries/MCCI_LoRaWAN_LMIC_library/project_config/lmic_project_config.h`
2. In `edge_node/config.h`, define a complete LMIC pin map for **Radio B** (LoRaWAN radio), including NSS, RST, and DIO lines.
3. Minimum required fields:

```cpp
// Baseline bench profile (ESP32 + dual SX1276):
#define RADIO_B_NSS   15
#define RADIO_B_RST   27
#define RADIO_B_DIO0  26
#define RADIO_B_DIO1  33
#define RADIO_B_DIO2  LMIC_UNUSED_PIN

const lmic_pinmap lmic_pins = {
  .nss = RADIO_B_NSS,
  .rxtx = LMIC_UNUSED_PIN,
  .rst = RADIO_B_RST,
  .dio = { RADIO_B_DIO0, RADIO_B_DIO1, RADIO_B_DIO2 },
};
```

4. Keep SPI assignment aligned with this plan: Radio B on HSPI (`SCK=14`, `MISO=12`, `MOSI=13`, `CS=15`).
   - Do not assign `RADIO_B_RST` to any HSPI/VSPI signal pin (for example, not `14/12/13/15/18/19/23/5`).
5. Bench validation gate (mandatory): before edge firmware development, verify `RADIO_B_RST`, `RADIO_B_DIO0`, and `RADIO_B_DIO1` against physical wiring continuity and first-join logs. If board wiring differs from baseline profile, update these numeric constants in `edge_node/config.h` before continuing.
6. Do not apply this LMIC setup to sensor/relay Maker UNO nodes.

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
| Beacon phase offset        | `(node_id % 6) * 1667 ms`    | Applied once at boot |
| Aggregation flush interval | 240s or 7 records            | Low-latency lab default |
| Aggregation flush interval (TTN fair-use profile) | 450s or 7 records | Use with 180s sensor interval |
| RSSI SF7→SF9 threshold     | < −105 dBm                   | Tune in field |
| RSSI SF9→SF7 threshold     | > −95 dBm                    | |
| WDT timeout (ATmega)       | 8s                           | |
| TWDT timeout (ESP32)       | 30s                          | Per task |

---

## Security and Robustness Choices
1. Mesh CRC16 over header — catches corruption and malformed frames. Not cryptographic.
2. LoRaWAN AES-128 session keys enforced at edge bridge uplink — where it matters.
3. `PacketUID` dedup prevents retry storms.
4. TTL prevents routing loops.
5. Node ID uniqueness enforced by `provision.py` + `nodes.csv`, not compile-time.
6. Payload version fields allow schema evolution without relay firmware updates.
7. EEPROM persistent counters (WDT resets, queue drops, CRC failures) enable post-deployment health analysis without live logging access.

---

## Project File Structure

```
CSC2106-G33/
│
├── shared/                              # Shared protocol definitions (all node types)
│   └── mesh_protocol.h                  #   MeshHeader, BeaconPayloadV1, BridgeAggV1,
│                                        #   ACK timeout PROGMEM table, config struct,
│                                        #   EEPROM address constants, CRC16,
│                                        #   rank normalization constants/helpers
│
├── sensor_node/                         # Sensor node firmware (ATmega328P / Maker UNO)
│   └── sensor_node.ino                  #   DHT22 driver, SensorPayload packing,
│                                        #   parent selection, TX with retry, WDT
│
├── relay_node/                          # Relay node firmware (ATmega328P / Maker UNO)
│   └── relay_node.ino                   #   CRC16 validation, opaque forwarding,
│                                        #   beacon emission, parent selection, WDT
│
├── edge_node/                           # Edge bridge firmware (ESP32, dual-radio)
│   ├── edge_node.ino                    #   Main entry, FreeRTOS task setup
│   ├── mesh_task.cpp                    #   Core 0: mesh RX, dedup, aggregation buffer
│   ├── lorawan_task.cpp                 #   Core 1: OTAA join, uplink queue, BridgeAggV1
│   └── config.h                         #   Pin assignments (SPI CS, IRQ for Radio A/B),
│                                        #   LoRaWAN keys (DevEUI, JoinEUI, AppKey)
│
├── dashboard/                           # Pico W application server (MicroPython)
│   ├── main.py                          #   Boot, WiFi connect, MQTT subscribe, HTTP serve
│   ├── dashboard.py                     #   HTML template string with placeholder substitution
│   └── state.py                         #   In-memory node state dict (update + query)
│
├── tools/                               # Host-side tooling (Python + JS)
│   ├── codec.py                         #   encode_agg_v1(), decode_agg_v1(), roundtrip_test()
│   ├── decoder.js                       #   TTN uplink payload formatter (deployed to TTN)
│   ├── provision.py                     #   Node ID provisioning via USB serial → EEPROM
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

1. **Shared protocol header:** `shared/mesh_protocol.h` — `MeshHeader`, `BeaconPayloadV1`, `BridgeAggV1`, ACK timeout PROGMEM table, config struct, EEPROM address constants.

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
   - EEPROM drop counters.
   - Watchdog + WDT reset logging.
   - Orphan hold queue behaviour.

6. **Edge node firmware** `edge_node/` (ESP32):
   - Real OTAA join on Radio B.
   - `mesh_task` + `lorawan_task` dual-task structure.
   - Aggregation buffer → `BridgeAggV1` packing.
   - Queue with overflow logging.
   - FreeRTOS TWDT per task.
   - `edge_uptime_s` counter.

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

TX logs are captured via USB serial during **lab and bench experiments only**. Field tunnel experiments are not instrumented with USB serial due to physical cable constraints — all tunnel validation relies on TTN-side logs and EEPROM drop counters retrieved post-run.

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
All nodes transmit at 30s interval for 10 minutes. Stresses relay queues and dedup. Observe which failure mode dominates via EEPROM drop counters.

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
9. **Orphan drop:** Kill parent for > 60s. Verify `ORPHAN_DROP` logged, EEPROM counter incremented, node recovers after parent restoration.
10. **TTL and dedup:** No packet loop amplification. Duplicate delivery rate = 0 under stable conditions.
11. **WDT reset logging:** Inject artificial hang in test build. Verify EEPROM[0x14] increments and `WDT_RESET` logged on next boot.
12. **TTN end-to-end + airtime profile check:** Both edge bridges join and uplink through WisGate. In low-latency profile (120s/240s), verify projected airtime exceeds 30s/day and document this as public-TTN non-compliant. In TTN fair-use profile (180s/450s, SF7), verify airtime < 30s/day per bridge.
13. **Aggregation correctness (steady state + burst):** With default 240s flush, nominal topology should produce ~4 records/uplink on bridge 1 and ~2 records/uplink on bridge 2. Under burst load, verify 7-record flush path (`record_count = 7`) decodes correctly.
14. **Queue overflow logging:** Fill uplink queue past capacity. Verify `QUEUE_OVERFLOW` logged with packet UIDs, EEPROM counter incremented.
15. **Pico W dashboard:** Node topology, latest readings (DHT22 temperature + humidity), and network health display correctly. Values match TTN decoded data.
16. **Payload-agnostic relay verification:** Replace one DHT22 sensor node with a different payload pattern (e.g., random bytes). Relay and edge bridge must forward and aggregate it without error. Only the TTN payload formatter output will differ — the mesh pipeline itself must be unaffected.
17. **TX jitter and channel-sense behavior:** Under synchronized start conditions, verify TX timestamps spread over jitter window and busy-sense events trigger bounded backoff (RSSI method; CAD optional if wired/supported).
18. **Congestion state transitions:** Induce queue/ACK stress and verify nodes transition GREEN→YELLOW→RED and recover back with correct logs.
19. **Adaptive retries:** In RED state, verify effective retries reduce to 1; in GREEN return to 3.
20. **Beacon staggering:** On boot, verify beacon transmissions from 6 nodes are phase-offset across ~10s window.
21. **Edge airtime guard:** In TTN fair-use profile, force projected daily airtime above threshold and verify `AIRTIME_DEFER` behavior and 86400s window reset.
22. **ATmega flash footprint gate:** Run `avr-size` on sensor and relay binaries. Verify relay remains below the project threshold (warn at >90% flash).

---

## Acceptance Criteria

1. Real `sensor → relay → edge → WisGate → TTN → Pico W` pipeline works end-to-end for both edge bridges.
2. Airtime budget is explicitly validated against active profile: low-latency profile is documented as TTN fair-use non-compliant; TTN fair-use profile is measured/projection-checked to stay < 30s/day per bridge at SF7.
3. **Relay is payload-agnostic:** zero corruption events across all test scenarios. Relay firmware must work unchanged with any sensor payload variant.
4. Structured dataset across all three stress test scenarios with PDR per node, payload integrity, telemetry latency distribution, and EEPROM drop-reason breakdown. All results evaluated against pass/fail thresholds.
5. Self-healing demonstrated: orphan recovery time and re-parent convergence time measured.
6. WDT behaviour verified on bench before field deployment.
7. Pico W dashboard shows topology, node health, latest readings (temperature + humidity).
8. Report includes deployment guidance: node spacing, SF selection for tunnel geometry, score weight tuning, congestion handling.
9. Congestion-control policy (jitter/channel-sense/state-based interval+retry) is validated with test cases 17–20.
10. TTN fair-use profile edge airtime guard is validated (test case 21) for long-duration public TTN operation.
11. ATmega flash usage is measured and recorded for sensor/relay builds (test case 22), with mitigation actions documented if relay exceeds 90% flash.

---

## Assumptions and Constraints

1. Operator has WisGate admin access and TTN application/device credentials.
2. Edge hardware confirmed as ESP32 with dual SX1276 radios.
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
 2. **Dual SPI radio arbitration on ESP32:** **Decision: Use separate SPI buses.**
    - Radio A (Mesh): **VSPI** (SCK=18, MISO=19, MOSI=23, CS=5)
    - Radio B (WAN): **HSPI** (SCK=14, MISO=12, MOSI=13, CS=15)
    - **Rationale:** Prevents bus contention completely. No complex arbitration software needed.
 3. **WisGate mode:** **Decision: Legacy Packet Forwarder.**
    - **Rationale:** Simplest configuration for lab/educational use. "It just works" without certificate management.
