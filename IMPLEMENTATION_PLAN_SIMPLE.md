# Implementation Plan — LoRa Mesh for Deep Tunnel Infrastructure

**CSC2106 Group 33 | Singapore Institute of Technology**

> This is the build guide your team follows day-to-day. It tells you exactly what to build, in what order, and who does what. Keep the detailed spec (`IMPLEMENTATION_PLAN_v4.md`) as a reference for byte layouts and edge cases — but this document is what you work from.
>
> Everything is built and tested in the lab (above ground). The "tunnel" scenario is the motivation, not the test environment.

---

## Deadlines

| Deliverable | Due |
|---|---|
| Final Report | ~5 April 2026 (Week 13) |
| Video + Poster | 31 March 2026 (Week 13) |
| Demo Presentation | Week 14 (~20 min, live demo + Q&A) |

You have roughly **5 weeks** from late February 2026. Plan accordingly.

---

## What We Are Building (The Big Picture)

Two sensor nodes read temperature and humidity (DHT22). They send the data wirelessly over LoRa to a relay node, which forwards the data **without ever looking inside it** — this payload-agnostic forwarding is the core idea of the project. The relay passes the data up to an edge node (primary), which bundles the readings and sends them to The Things Network (TTN) via LoRaWAN. A second edge node sits on standby — if the primary goes down, the relay automatically fails over to it. TTN decodes the data and pushes it to a Pico W, which shows a live dashboard in a browser.

**Two communication protocols demonstrated:**
1. **Raw LoRa mesh** (sensor → relay → edge) — our custom protocol
2. **LoRaWAN** (edge → WisGate → TTN) — industry standard, AES-128 encrypted

```
                 LoRa Mesh (custom protocol)                    LoRaWAN (standard)
                ┌──────────────────────────┐                   ┌──────────────────┐
                │                          │                   │                  │
[Sensor 1] ──►  │                          │                   │                  │
               [Relay] ──► [Edge 1 (primary)] ──► LoRaWAN ──► [WisGate] ──► [TTN] ──► [Pico W]
[Sensor 2] ──►  │                          │                   │      dashboard
                │      [Edge 2 (standby)]  │──► LoRaWAN ──►    │
                │       (failover only)    │                   │
                └──────────────────────────┘                   └──────────────────┘

Normal flow:  Sensor 1 ──► Relay ──► Edge 1 ──► WisGate ──► TTN ──► Pico W
              Sensor 2 ──► Relay ──► Edge 1 ──► WisGate ──► TTN ──► Pico W

If Edge 1 dies: Relay detects timeout (~35s), re-parents to Edge 2.
                All traffic now flows through Edge 2 until Edge 1 recovers.
```

---

## Hardware

### All Nodes

| Node | Hardware | Count | Role |
|------|----------|-------|------|
| Sensor node | LilyGo T-Beam 923 MHz + DHT22 | 2 | Read temp/humidity, send to mesh |
| Relay node | LilyGo T-Beam 923 MHz | 1 | Forward packets without reading them |
| Edge node | LilyGo T-Beam 923 MHz | 2 | Mesh sink + LoRaWAN uplink to TTN |
| Gateway | RAK WisGate (lab-provided) | 2 | Receives LoRaWAN, forwards to TTN |
| Dashboard | Raspberry Pi Pico W | 1 | Shows data from TTN via MQTT |

**All nodes are powered by USB (5V). No batteries, no sleep mode.**

### T-Beam LoRa Pin Map (Same for All 5 Nodes)

Your T-Beam uses the **SX1262** radio (not SX1276). The pin map is different — notably, SX1262 requires a **BUSY** pin and uses **DIO1** (not DIO0) for interrupts.

| Function | GPIO | Notes |
|----------|------|-------|
| NSS (CS) | 18 | SPI chip select |
| RST | 23 | Radio reset |
| DIO1 | 33 | Interrupt pin (replaces DIO0 on SX1276) |
| BUSY | 32 | **Required for SX1262** — radio busy status |
| SCK | 5 | SPI clock |
| MISO | 19 | SPI data in |
| MOSI | 27 | SPI data out |

**Library:** Use **RadioLib** (not Arduino-LoRa). RadioLib supports SX1262 and has built-in LoRaWAN.

### CRITICAL — T-Beam Power Management (Read This First)

The T-Beam has an **AXP2101 PMU** (power management chip) that controls power to the SX1262 LoRa radio. The SX1262 is powered via the **ALDO2** rail (not DC1 as on older boards). If you don't initialise it, the radio gets no power and `radio.begin()` will silently fail. **Every T-Beam sketch (sensor, relay, edge) must call this before anything else:**

```cpp
#include <Wire.h>
#include <XPowersLib.h>
#include <RadioLib.h>

XPowersPMU PMU;

// SX1262 pin definitions for T-Beam
#define RADIO_NSS   18
#define RADIO_RST   23
#define RADIO_DIO1  33
#define RADIO_BUSY  32

SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);

void setup() {
    Serial.begin(115200);

    // --- MUST DO FIRST: Power on the LoRa radio via ALDO2 ---
    Wire.begin(21, 22);  // T-Beam I2C pins for PMU
    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, 21, 22)) {
        Serial.println("ERROR: PMU init failed! Check board wiring.");
        while (true) delay(1000);  // halt
    }
    PMU.setALDO2Voltage(3300);  // 3.3V to SX1262
    PMU.enableALDO2();          // power on the radio
    Serial.println("PMU OK — LoRa radio powered on via ALDO2");

    delay(10);  // let radio power stabilise

    // --- Now safe to init RadioLib ---
    int state = radio.begin(923.0);  // 923 MHz for AS923
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("ERROR: RadioLib init failed, code ");
        Serial.println(state);
        while (true) delay(1000);
    }
    // Set SF7, 125kHz BW, CR 4/5, sync word 0x12 (private network)
    radio.setSpreadingFactor(7);
    radio.setBandwidth(125.0);
    radio.setCodingRate(5);
    radio.setSyncWord(0x12);
    radio.setOutputPower(17);  // 17 dBm
    Serial.println("RadioLib OK — SX1262 @ 923 MHz, SF7");

    // ... rest of setup
}
```

**If you skip the PMU step, you will spend hours wondering why the radio doesn't work. Every team member must include this in their sketch.**

### DHT22 Wiring (Sensor Nodes Only)

| DHT22 Pin | Connect To |
|-----------|-----------|
| VCC (pin 1) | 3.3V on T-Beam |
| Data (pin 2) | GPIO 13 (confirm free on your board revision) |
| GND (pin 4) | GND on T-Beam |

Add a 10K pull-up resistor between Data and VCC.

---

## Node ID Assignment

Hardcode these as `#define` at the top of each sketch. No provisioning system needed for 5 nodes.

```
Edge 1 (primary):   NODE_ID = 0x01
Relay:              NODE_ID = 0x02
Sensor 1:           NODE_ID = 0x03
Sensor 2:           NODE_ID = 0x04
Edge 2 (standby):   NODE_ID = 0x06
```

---

## Team Split

| Person | Work Package |
|---|---|
| **Person 1** | Sensor node firmware (DHT22 + mesh send) |
| **Person 2** | Relay node firmware (opaque forwarding + beacons) |
| **Person 3** | Edge node firmware (mesh receive + LoRaWAN uplink) |
| **Person 4** | TTN setup + decoder.js + Pico W dashboard |
| **Person 5** | Testing, PDR scripts, data collection + report |

---

## Packet Formats

There are only three formats you need to understand. The relay only ever reads the first one.

### 1. MeshHeader — On Every LoRa Packet (10 bytes)

| Byte | Field | What It Means |
|------|-------|---------------|
| 0 | flags | Packet type (data/beacon/ACK), ACK requested?, forwarded? |
| 1 | src_id | Who originally created this packet |
| 2 | dst_id | Who it's going to (`0xFF` = broadcast) |
| 3 | prev_hop | Last node that touched this packet |
| 4 | ttl | Decremented each hop. Drop the packet if it reaches 0. |
| 5 | seq_num | Packet counter from the sender (for detecting duplicates) |
| 6 | rank | How many hops from the edge node (edge=0, relay=1, sensor=2) |
| 7 | payload_len | How many bytes follow after this header |
| 8–9 | crc16 | Checksum of bytes 0–7 (catch corrupted packets) |

**The relay reads bytes 0–9 only. Everything after byte 9 is copied blindly.**

### 2. SensorPayload — DHT22 Reading (7 bytes)

Attached after MeshHeader. Only the sensor node creates it.

| Byte | Field | Value |
|------|-------|-------|
| 0 | schema_version | `0x01` |
| 1 | sensor_type | `0x03` = DHT22 |
| 2–3 | temp_c_x10 | Temperature × 10 as signed integer (e.g. 253 = 25.3°C) |
| 4–5 | humidity_x10 | Humidity × 10 as unsigned integer (e.g. 655 = 65.5%) |
| 6 | status | `0x00` = OK, `0x01` = read error (skip this reading) |

**Why × 10?** LoRa packets are raw bytes — no floats. Multiply by 10, send as integer, divide by 10 at the receiving end.

### 3. BridgeAggV1 — What the Edge Sends to TTN

The edge collects multiple sensor readings and bundles them into one LoRaWAN uplink.

**Header (3 bytes):**

| Byte | Field | Meaning |
|------|-------|---------|
| 0 | schema_version | `0x02` |
| 1 | bridge_id | Edge node ID (`0x01` or `0x06`) |
| 2 | record_count | How many sensor readings follow (max 7) |

**Each record (14 bytes):**

| Bytes | Field | Meaning |
|-------|-------|---------|
| 0 | mesh_src_id | Which sensor sent this |
| 1 | mesh_seq | Packet sequence number |
| 2 | sensor_type_hint | `0x03` = DHT22 |
| 3 | hop_estimate | How many hops it took |
| 4–5 | edge_uptime_s | Edge uptime when received |
| 6 | opaque_len | Payload length (always 7 for DHT22) |
| 7–13 | opaque_payload | The raw 7-byte SensorPayload, copied as-is |

---

## Step-by-Step: Person 1 — Sensor Node

**File:** `sensor_node/sensor_node.ino`

### Step 1 — PMU + LoRa Init

Copy the PMU + LoRa init code from the "CRITICAL" section above into your `setup()`. Confirm the serial output says `PMU OK` and `LoRa OK`.

### Step 2 — DHT22 Setup

```cpp
#include <DHT.h>
#define DHT_PIN  13
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// In setup(), after LoRa init:
dht.begin();
```

### Step 3 — Read the Sensor and Pack the Payload

```cpp
uint8_t payload[7];

float temp = dht.readTemperature();
float hum  = dht.readHumidity();

if (isnan(temp) || isnan(hum)) {
    Serial.println("DHT22 read failed — skipping this cycle");
    return;  // skip, try again next cycle
}

int16_t  temp_x10 = (int16_t)(temp * 10);
uint16_t hum_x10  = (uint16_t)(hum * 10);

payload[0] = 0x01;                      // schema version
payload[1] = 0x03;                      // sensor type = DHT22
payload[2] = (temp_x10 >> 8) & 0xFF;   // temp high byte
payload[3] = temp_x10 & 0xFF;           // temp low byte
payload[4] = (hum_x10 >> 8) & 0xFF;    // humidity high byte
payload[5] = hum_x10 & 0xFF;            // humidity low byte
payload[6] = 0x00;                      // status = OK
```

**Pack as a byte array, not a struct.** Struct padding is compiler-dependent and will corrupt the payload.

### Step 4 — Build MeshHeader + Send

Build the 10-byte MeshHeader, append the 7-byte payload, and send via RadioLib:

```cpp
uint8_t packet[17];  // 10-byte header + 7-byte payload

// Fill in MeshHeader (bytes 0-9)
packet[0] = flags;        // PKT_TYPE_DATA | ACK_REQ
packet[1] = NODE_ID;      // src_id
packet[2] = parent_id;    // dst_id (your current parent)
packet[3] = NODE_ID;      // prev_hop
packet[4] = 10;           // ttl
packet[5] = seq_num++;    // increment each send
packet[6] = 2;            // rank (sensor = 2)
packet[7] = 7;            // payload_len

// Compute CRC16 over bytes 0-7
uint16_t crc = crc16(packet, 8);
packet[8] = (crc >> 8) & 0xFF;
packet[9] = crc & 0xFF;

// Append payload (bytes 10-16)
memcpy(&packet[10], payload, 7);

// Send with RadioLib
int state = radio.transmit(packet, 17);
if (state == RADIOLIB_ERR_NONE) {
    Serial.println("TX OK");
} else {
    Serial.print("TX failed, code ");
    Serial.println(state);
}
```

Key fields to set:
- `flags`: PKT_TYPE_DATA + ACK_REQ
- `src_id`: your `NODE_ID`
- `dst_id`: your current parent's ID
- `prev_hop`: your `NODE_ID`
- `ttl`: 10
- `seq_num`: increment by 1 each send
- `payload_len`: 7
- `crc16`: compute over bytes 0–7

### Step 5 — Add TX Jitter

Before each transmission, add a random delay to prevent all nodes from transmitting at the same time (especially after a power cycle):

```cpp
delay(random(0, 2000));  // 0–2 seconds random jitter
```

### Step 6 — Wait for ACK

After sending, switch to RX mode and wait up to **600ms** for an ACK from the next hop:

```cpp
// Start listening for ACK
radio.startReceive();
uint32_t start = millis();
bool ack_received = false;

while (millis() - start < 600) {
    if (radio.available()) {
        uint8_t ack_buf[10];
        int len = radio.readData(ack_buf, sizeof(ack_buf));
        if (len >= 10 && is_ack_for_me(ack_buf)) {
            ack_received = true;
            break;
        }
    }
    delay(1);
}

if (!ack_received) {
    // Retry logic here (max 3 retries)
}
```

If no ACK, retry up to **3 times**. If all retries fail, log the failure and wait for the next cycle.

### Step 7 — Parent Selection

Listen for beacons from relay and edge nodes. Score each potential parent:

```
score = (60 × rank_score) + (25 × rssi_score) + (15 × (100 − queue_pct))
```

Where:
- `rank_score`: lower rank is better (edge=0 gets score 100, relay=1 gets 75, sensor=2 gets 50)
- `rssi_score`: map RSSI from [-120, -60] to [0, 100]
- `queue_pct`: from the parent's beacon (lower is better)

**Only switch parents if the new score is ≥ 8 points better** to prevent flapping.

### Step 8 — Sending Interval

- Use **30 seconds** during development (you'll see activity faster)
- Switch to **120 seconds** for final test runs

### Step 9 — Serial Logging

Print every TX so it can be captured for PDR analysis:

```
TX | node=03 | seq=12 | temp=25.3C | hum=65.5% | parent=02 | rssi=-68
```

---

## Step-by-Step: Person 2 — Relay Node

**File:** `relay_node/relay_node.ino`

This is the most important node — it proves the core research claim (payload-agnostic forwarding).

### Step 1 — PMU + LoRa Init

Same as sensor node. Copy the PMU + LoRa init from the "CRITICAL" section.

### Step 2 — Receive a Packet and Validate

When a packet arrives, run these checks in order:

1. **Length check** — is it at least 10 bytes (MeshHeader size)? If not, drop it.
2. **Self check** — is `src_id == MY_NODE_ID`? If yes, drop it (it's your own packet echoing back).
3. **Dedup check** — have you seen this `(src_id, seq_num)` combination in the last 30 seconds? If yes, drop it. Keep a table of 16 recent entries.
4. **CRC check** — compute CRC16 over bytes 0–7 and compare against bytes 8–9. If mismatch, drop it and log `CRC_FAIL`.
5. **TTL check** — is `ttl > 0`? If zero, drop it and log `TTL_DROP`.

### Step 3 — ACK the Sender

If the `ACK_REQ` flag is set in the header, send a short ACK packet back to `prev_hop`. **Do this before forwarding** — the sender is waiting.

### Step 4 — Forward the Packet (The Payload-Agnostic Part)

This is the core of the project:

1. Set `prev_hop = MY_NODE_ID`
2. Set the `FWD` flag in the flags byte
3. Decrement `ttl` by 1
4. Recalculate `crc16` over the modified bytes 0–7
5. Copy **all bytes** from 0 to `(10 + payload_len - 1)` and re-transmit

**You must NEVER look at bytes 10 onward.** The relay copies them as a block. This is the rule that must never be broken — if the sensor payload changes to a different sensor type tomorrow, the relay must continue working without any firmware update.

### Step 5 — Broadcast Beacons

Every **10 seconds**, send a beacon so downstream nodes know you exist.

Beacon payload (4 bytes after MeshHeader):

| Byte | Field | Value |
|------|-------|-------|
| 0 | schema_version | `0x01` |
| 1 | queue_pct | Your actual TX queue occupancy (0–100) |
| 2 | link_quality | RSSI-based quality to your parent (0–100) |
| 3 | parent_health | ACK success rate to your parent (0–100) |

**Use real values**, not hardcoded. `queue_pct` is computed from how full your forwarding buffer is. Sensor nodes use this to make routing decisions.

Apply beacon phase offset at boot to prevent all nodes from beaconing simultaneously:

```cpp
uint32_t beacon_offset_ms = (NODE_ID % 5) * 2000;  // spread across 10s window
```

### Step 6 — Parent Selection

Same scoring formula as the sensor node. The relay picks between Edge 1 and Edge 2 as its parent. Normally it will choose Edge 1 (if Edge 1 has a stronger signal or was seen first). If Edge 1 disappears, the relay will switch to Edge 2 after the parent timeout (~35 seconds).

### Step 7 — Serial Logging

Log every action for debugging and analysis:

```
FWD  | uid=0312 | rssi=-72 | ttl=9 | parent=01
DROP | uid=0312 | reason=duplicate
DROP | uid=0507 | reason=crc_fail
ACK  | to=03 | for_uid=0312
BCN  | queue=12% | lq=85 | health=95
```

---

## Step-by-Step: Person 3 — Edge Node

**File:** `edge_node/edge_node.ino`

This is the most complex node. The T-Beam has **one SX1262 radio** that must be time-shared between two jobs: listening for mesh packets and sending LoRaWAN uplinks.

### How Time-Sharing Works

```
Normal state:   [MESH_LISTEN] ─── radio listens for incoming mesh packets ───
                                                                              │
When flush triggers (240s elapsed or 7 records buffered):                    │
                [MESH_LISTEN] → [LORAWAN_TXRX] → [MESH_LISTEN]              │
                                    │                                         │
                                    ├─ Reconfigure radio for LoRaWAN         │
                                    ├─ Send BridgeAggV1 uplink               │
                                    ├─ Wait for RX1/RX2 windows (~5s)        │
                                    └─ Reconfigure radio back to mesh        │
```

During the LoRaWAN window (~5 seconds), the edge is deaf to mesh packets. At 120-second sensor intervals, this is fine — you will not miss anything.

**Key advantage of RadioLib:** The same `SX1262` radio object is used for both raw LoRa mesh and LoRaWAN — no need for two separate libraries fighting over the hardware.

### Step 1 — PMU + LoRa Init

Same as other nodes. Copy the PMU + RadioLib init from the "CRITICAL" section.

### Step 2 — RadioLib LoRaWAN Setup for AS923

RadioLib has built-in LoRaWAN support. No LMIC library needed.

```cpp
#include <RadioLib.h>

// Use the same SX1262 radio object from the CRITICAL section
extern SX1262 radio;

// LoRaWAN node object (wraps the radio)
LoRaWANNode node(&radio, &AS923);

// OTAA credentials from TTN (Person 4 will give you these)
// DevEUI and JoinEUI: LSB order (reversed from TTN console display)
// AppKey: MSB order (as TTN shows it)
uint64_t joinEUI = 0x0000000000000000;
uint64_t devEUI  = 0x0000000000000000;  // Get from TTN
uint8_t appKey[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

bool lorawan_joined = false;

void setup_lorawan() {
    // Begin LoRaWAN with AS923 band
    node.beginOTAA(joinEUI, devEUI, nullptr, appKey);
    
    // Attempt OTAA join (blocking, up to 60s)
    Serial.println("Attempting LoRaWAN OTAA join...");
    int state = node.activateOTAA();
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("JOIN OK");
        lorawan_joined = true;
    } else {
        Serial.print("JOIN FAILED, code ");
        Serial.println(state);
        // Will retry on next uplink attempt
    }
}
```

### Step 3 — Radio Arbitration (Cooperative Loop)

```cpp
enum RadioMode { MESH_LISTEN, LORAWAN_TXRX };
RadioMode radio_mode = MESH_LISTEN;
bool uplink_pending = false;
uint32_t lorawan_window_start = 0;
const uint32_t LORAWAN_WINDOW_MAX_MS = 10000;  // max time in LoRaWAN mode

void loop() {
    if (radio_mode == MESH_LISTEN) {
        receive_mesh_packets();      // check radio for incoming mesh frames
        broadcast_beacon_if_due();   // rank-0 beacon every 10s
        check_flush_trigger();       // sets uplink_pending = true if ready

        if (uplink_pending && lorawan_joined) {
            radio_mode = LORAWAN_TXRX;
            lorawan_window_start = millis();
            send_lorawan_uplink();   // blocking call, returns when done
            radio_mode = MESH_LISTEN;
            switch_to_mesh_rx();     // reconfigure radio for mesh listening
            uplink_pending = false;
        }
    }

    delay(2);  // small tick delay
}

void switch_to_mesh_rx() {
    // Reconfigure radio for raw LoRa mesh reception
    radio.setFrequency(923.0);
    radio.setSpreadingFactor(7);
    radio.setBandwidth(125.0);
    radio.setCodingRate(5);
    radio.setSyncWord(0x12);
    radio.startReceive();
}
```

### Step 4 — Mesh Receive (While in MESH_LISTEN Mode)

Same validation as the relay:
- Minimum length (10 bytes)
- Dedup check
- CRC check
- ACK the sender

Then extract the useful fields and store in the aggregation buffer:
- `src_id`, `seq_num`, `hop_estimate` (= 10 − received TTL)
- Copy the raw payload bytes as-is

```cpp
void receive_mesh_packets() {
    if (radio.available()) {
        uint8_t buf[64];
        int len = radio.readData(buf, sizeof(buf));
        int rssi = radio.getRSSI();
        
        if (len >= 10) {
            // Validate and process packet
            // ... (same logic as relay: length, dedup, CRC, TTL checks)
            
            // Store in aggregation buffer
            add_to_agg_buffer(buf, len, rssi);
        }
    }
}
```

### Step 5 — Aggregation + Flush via RadioLib LoRaWAN

Collect mesh records in a buffer (max 7 slots). Flush when either:
- **240 seconds** have elapsed since last uplink, OR
- **7 records** are buffered

On flush, pack a `BridgeAggV1` byte array and send via RadioLib LoRaWAN:

```cpp
void send_lorawan_uplink() {
    uint8_t agg_buffer[101];  // 3 + (7 × 14) max
    int agg_len = pack_bridge_agg_v1(agg_buffer);
    
    Serial.print("UPLINK | records=");
    Serial.print(record_count);
    Serial.print(" | bytes=");
    Serial.println(agg_len);
    
    // Send unconfirmed uplink on fport 1
    int state = node.sendReceive(agg_buffer, agg_len, 1);
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("UPLINK OK");
    } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
        Serial.println("UPLINK OK (no downlink)");
    } else {
        Serial.print("UPLINK FAILED, code ");
        Serial.println(state);
    }
    
    // Clear aggregation buffer after uplink
    clear_agg_buffer();
}
```

### Step 6 — Broadcast Rank-0 Beacons

Same format as relay beacons, but with `rank = 0`. This tells sensor nodes and the relay that you are the top of the mesh hierarchy.

Apply phase offset: `(NODE_ID % 5) * 2000 ms` at boot.

**Both Edge 1 and Edge 2 broadcast beacons.** The relay hears both and picks the one with the better score as its parent.

### Step 7 — Serial Logging

```
RX_MESH | src=03 | seq=12 | hops=2 | rssi=-68
RX_MESH | src=04 | seq=08 | hops=2 | rssi=-71
FLUSH   | records=2 | bytes=31
UPLINK  | records=2 | bytes=31
JOIN    | success | devaddr=260BXXXX
```

### What to Flash

The same firmware goes onto both Edge 1 and Edge 2. Only two things differ:
- `#define NODE_ID 0x01` (Edge 1) vs `#define NODE_ID 0x06` (Edge 2)
- OTAA credentials (each edge has its own DevEUI/AppKey from TTN)

---

## Step-by-Step: Person 4 — TTN Setup + Decoder + Dashboard

### Part A — WisGate Gateway Setup

**Repeat these steps for both WisGate gateways:**

1. Attach the LoRa antenna to the WisGate. **Do not power on without an antenna.**
2. Power on via 12V adapter or PoE.
3. Connect to the WisGate web UI:
   - WiFi: connect to `RAK7268_XXXX`, browse to `192.168.230.1`
   - Login: `root` / `admin12345678!`
4. Set the WisGate to **Packet Forwarder** mode.
5. Server URL: `as1.cloud.thethings.network` (Singapore cluster, **not** `au1`).
6. Note the **Gateway EUI** from the bottom label — you need it for TTN.
7. Connect the WisGate to lab WiFi (Network → Wi-Fi → DHCP Client).

### Part B — TTN Console Setup

1. Log into [TTN Console](https://console.cloud.thethings.network/) → select **`as1`** cluster.
2. Register **both** WisGate gateways:
   - Gateways → Register gateway → enter Gateway EUI (from each WisGate's label)
   - Frequency plan: **Asia 920–923 MHz (AS923 Group 1)**
   - Verify status shows **Connected** for both
3. Create an application: `csc2106-g33-mesh`
4. Register **two** end devices (one per edge node):

   | Field | Edge 1 | Edge 2 |
   |-------|--------|--------|
   | End device ID | `edge-bridge-01` | `edge-bridge-02` |
   | Frequency plan | AS923 Group 1 | AS923 Group 1 |
   | LoRaWAN version | 1.0.2 | 1.0.2 |
   | Activation | OTAA | OTAA |
   | JoinEUI | `00 00 00 00 00 00 00 00` | `00 00 00 00 00 00 00 00` |
   | DevEUI | Auto-generate | Auto-generate |
   | AppKey | Auto-generate | Auto-generate |

5. **Record the DevEUI and AppKey for each device** → give them to Person 3.
   - DevEUI in firmware = **LSB** order (reversed from what TTN shows)
   - AppKey in firmware = **MSB** order (as TTN shows it)

### Part C — Payload Decoder (decoder.js)

**File:** `tools/decoder.js`

Deploy as a TTN uplink payload formatter (Application → Payload formatters → Uplink → JavaScript):

```javascript
function decodeUplink(input) {
    var bytes = input.bytes;
    var i = 0;
    var result = {};

    result.schema_version = bytes[i++];
    result.bridge_id = bytes[i++];
    var record_count = bytes[i++];
    result.record_count = record_count;
    result.readings = [];

    for (var r = 0; r < record_count; r++) {
        var rec = {};
        rec.src_id         = bytes[i++];
        rec.seq            = bytes[i++];
        rec.sensor_type    = bytes[i++];
        rec.hops           = bytes[i++];
        rec.edge_uptime_s  = (bytes[i] << 8) | bytes[i+1]; i += 2;
        var plen           = bytes[i++];

        // Parse DHT22 SensorPayload (7 bytes)
        var sv             = bytes[i++];  // schema_version
        var st             = bytes[i++];  // sensor_type
        var temp_raw       = (bytes[i] << 8) | bytes[i+1]; i += 2;
        var hum_raw        = (bytes[i] << 8) | bytes[i+1]; i += 2;
        var status         = bytes[i++];

        // Handle signed 16-bit temperature
        if (temp_raw > 32767) temp_raw -= 65536;

        rec.temperature_c  = temp_raw / 10.0;
        rec.humidity_pct   = hum_raw / 10.0;
        rec.sensor_ok      = (status === 0);
        result.readings.push(rec);
    }
    return { data: result };
}
```

### Part D — Enable MQTT on TTN

1. Application → Integrations → MQTT
2. Note the connection details:
   - Server: `as1.cloud.thethings.network`
   - Port: `1883`
   - Username: `csc2106-g33-mesh@ttn`
   - Password: Generate an API key with "Read application traffic" rights
3. Topic for Pico W: `v3/csc2106-g33-mesh@ttn/devices/+/up`

### Part E — Pico W Dashboard

**File:** `dashboard/main.py`

The Pico W connects to WiFi, subscribes to TTN MQTT, and serves a web page.

**Libraries needed:**
- `umqtt.simple` (built into MicroPython)
- `phew` (copy from [Pimoroni GitHub](https://github.com/pimoroni/phew) into Pico W filesystem)

```python
import network, time, json
from umqtt.simple import MQTTClient

# ── WiFi ──
wlan = network.WLAN(network.STA_IF)
wlan.active(True)
wlan.connect("YOUR_SSID", "YOUR_PASSWORD")
while not wlan.isconnected():
    time.sleep(0.5)
print("WiFi connected:", wlan.ifconfig())

# ── State ──
nodes = {}  # keyed by src_id string

# ── MQTT ──
TTN_APP_ID  = "csc2106-g33-mesh"
TTN_API_KEY = "YOUR_API_KEY"
TTN_SERVER  = "as1.cloud.thethings.network"
TOPIC       = "v3/{}@ttn/devices/+/up".format(TTN_APP_ID)

def on_message(topic, msg):
    payload = json.loads(msg)
    decoded = payload.get("uplink_message", {}).get("decoded_payload", {})
    readings = decoded.get("readings", [])
    for r in readings:
        sid = str(r["src_id"])
        nodes[sid] = {
            "temp":    r.get("temperature_c", "?"),
            "hum":     r.get("humidity_pct", "?"),
            "hops":    r.get("hops", "?"),
            "ok":      r.get("sensor_ok", False),
            "bridge":  decoded.get("bridge_id", "?"),
            "updated": time.time(),
        }

client = MQTTClient("picow", TTN_SERVER,
                     user="{}@ttn".format(TTN_APP_ID),
                     password=TTN_API_KEY, port=1883)
client.set_callback(on_message)
client.connect()
client.subscribe(TOPIC)

# ── Dashboard HTML ──
def dashboard_html():
    rows = ""
    for sid, d in sorted(nodes.items()):
        status = "OK" if d["ok"] else "ERR"
        rows += "<tr><td>0x{:02X}</td><td>{:.1f}</td><td>{:.1f}</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
            int(sid), d["temp"], d["hum"], d["hops"], d["bridge"], status)
    return """<!DOCTYPE html><html><head>
    <title>G33 Tunnel Dashboard</title>
    <meta http-equiv="refresh" content="10">
    <style>
      body {{ font-family: sans-serif; margin: 20px; }}
      table {{ border-collapse: collapse; width: 100%; }}
      th, td {{ border: 1px solid #ccc; padding: 10px; text-align: center; }}
      th {{ background: #333; color: white; }}
      tr:nth-child(even) {{ background: #f9f9f9; }}
    </style></head><body>
    <h2>CSC2106 G33 — LoRa Mesh Tunnel Monitor</h2>
    <p>Auto-refreshes every 10 seconds</p>
    <table>
      <tr><th>Node</th><th>Temp (C)</th><th>Humidity (%)</th><th>Hops</th><th>Bridge</th><th>Status</th></tr>
      {}</table>
    <p><small>Last page load: {}</small></p>
    </body></html>""".format(rows, time.time())

# ── Simple HTTP server ──
import socket
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("0.0.0.0", 80))
server.listen(1)
server.setblocking(False)
print("Dashboard at http://{}".format(wlan.ifconfig()[0]))

# ── Main loop ──
while True:
    # MQTT
    try:
        client.check_msg()
    except Exception:
        time.sleep(2)
        try:
            client.connect()
            client.subscribe(TOPIC)
        except Exception:
            pass

    # HTTP
    try:
        cl, addr = server.accept()
        request = cl.recv(1024)
        response = dashboard_html()
        cl.send("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n")
        cl.send(response)
        cl.close()
    except OSError:
        pass

    time.sleep(0.05)
```

---

## Step-by-Step: Person 5 — Testing + Data Collection + Report

### Before You Start: codec.py

**File:** `tools/codec.py`

Write a short Python script (~30 lines) that encodes and decodes `BridgeAggV1` payloads. This validates the byte layout before Person 3 writes the edge firmware. It prevents byte-order bugs that are painful to debug on hardware.

Minimum functions:
- `encode_agg_v1(bridge_id, records)` → bytes
- `decode_agg_v1(raw_bytes)` → dict
- `roundtrip_test()` → assert encode→decode produces identical fields

Run this and verify it matches the decoder.js output for the same input hex. **Do this before Person 3 starts the edge node firmware.**

### Data Collection Scripts

**File:** `tools/collect_serial.py` — Connects to a sensor node's USB serial port, captures TX log lines, writes to CSV.

**File:** `tools/collect_ttn.py` — Subscribes to TTN MQTT, captures uplink messages, writes to CSV.

**File:** `tools/compute_pdr.py` — Reads both CSVs, matches `(src_id, seq_num)` pairs, computes PDR per node and end-to-end.

### Three Test Scenarios

Run each scenario and collect data for the report.

#### Scenario 1 — Baseline (Steady State)

- All nodes powered on, normal operation.
- Both sensors transmit every **30 seconds** for **20 minutes**.
- No failures introduced.
- **What to measure:** PDR = packets in TTN ÷ packets sent (from serial logs).
- **Pass threshold:** PDR ≥ 95%, payload integrity 100%.

#### Scenario 2 — Relay Failure and Recovery

- Run steady state for 10 minutes.
- **Power off the relay node** for 90 seconds.
- Observe: sensor nodes should detect parent timeout (~35s) and attempt to re-parent directly to an edge node.
- Power relay back on.
- Observe: sensors should re-parent to the relay once beacons resume.
- **What to measure:** Time to detect failure, time to recover, PDR during and after.
- **Pass threshold:** PDR recovers to ≥ 95% within 2 beacon intervals (20s) after relay is restored.

#### Scenario 3 — Edge Node Failover (Redundancy Test)

- Run steady state for 10 minutes (all traffic through Edge 1).
- **Power off Edge 1.**
- Observe: relay should detect parent timeout (~35s) and re-parent to Edge 2.
- Observe: TTN uplinks should now come from `edge-bridge-02` instead of `edge-bridge-01`.
- Sensors are unaffected — they still route through the relay.
- **What to measure:** Failover time, whether TTN uplinks switch to Edge 2's device ID.
- **Pass threshold:** Uplinks resume on Edge 2 within 60 seconds. PDR ≥ 95% after failover.

### What to Verify During Each Test

| Check | What You're Looking For |
|-------|------------------------|
| Sensor reads real data | Serial shows actual temperature (not NaN, 0, or random) |
| Relay forwards without corruption | Bytes at TTN match sensor TX bytes exactly |
| Parent selection works | Sensors pick relay over direct edge (relay is closer) |
| Relay picks Edge 1 as primary | Under normal conditions Edge 1 is the parent |
| Edge 1 failure → failover to Edge 2 | TTN uplinks switch from `edge-bridge-01` to `edge-bridge-02` |
| Relay failure → sensors re-parent | Sensors route directly to edge during relay outage |
| Dashboard shows live data | Pico W displays readings within ~5 minutes of sensor TX |
| Payload-agnostic proof | Swap one DHT22 to send random bytes — relay keeps forwarding |

### Mapping to Report Rubric

When writing the report, make sure these rubric criteria have dedicated sections:

| Rubric | What to write |
|--------|--------------|
| **Testing and Validation** | All 3 scenarios with data tables, pass/fail results |
| **Security** | LoRaWAN AES-128 encryption on uplink (built into RadioLib LoRaWAN/TTN). Mesh layer uses CRC16 for integrity. |
| **Interoperability** | LoRaWAN AS923 standard compliance. MQTT from TTN. Open payload schema. |
| **Integration** | End-to-end path: sensor → relay → edge → WisGate → TTN → Pico W |
| **Scalability** | Adding more sensors requires no relay firmware change — that's the whole point of payload-agnostic forwarding |
| **User Experience** | Dashboard auto-refreshes, shows per-node data clearly |

---

## Key Parameters Reference

| Parameter | Value | Notes |
|-----------|-------|-------|
| LoRa frequency | 923 MHz | AS923, Singapore |
| Spreading factor | SF7 (fixed) | Good range in lab, fastest airtime |
| Bandwidth | 125 kHz | Standard |
| TX power | 17 dBm | |
| Sensor interval | 30s (dev) / 120s (final) | Lower during development |
| Beacon interval | 10s | Relay + edge announce themselves |
| Beacon phase offset | `(NODE_ID % 5) × 2000 ms` | Prevents simultaneous beacons |
| TX jitter | 0–2000 ms random | Before each sensor transmission |
| ACK timeout | 600 ms | How long to wait for hop ACK |
| Max retries | 3 | Per packet |
| TTL default | 10 | Max hops (we only have 3, so rarely triggers) |
| Dedup window | 30s | Ignore duplicate packets within this window |
| Dedup table size | 16 entries | Per node |
| Aggregation flush | 240s or 7 records | Edge sends to TTN when either is met |
| Parent switch hysteresis | Score diff ≥ 8 | Prevents flapping |
| Parent timeout | 35s | Before declaring parent dead |
| Score weights | rank=60, rssi=25, queue=15 | Summing to 100 |
| LoRaWAN uplink window | ~5s max | Edge is deaf to mesh during this |

---

## File Structure

```
CSC2106-G33/
├── sensor_node/
│   └── sensor_node.ino          ← Person 1
├── relay_node/
│   └── relay_node.ino           ← Person 2
├── edge_node/
│   ├── edge_node.ino            ← Person 3 (flash to both Edge 1 and Edge 2)
│   └── config.h                 ← NODE_ID + OTAA credentials (different per edge)
├── dashboard/
│   ├── main.py                  ← Person 4 (Pico W)
│   └── phew/                    ← Copy from Pimoroni GitHub
├── tools/
│   ├── decoder.js               ← Person 4 (TTN payload formatter)
│   ├── codec.py                 ← Person 5 (BridgeAggV1 encode/decode test)
│   ├── collect_serial.py        ← Person 5 (capture sensor TX logs)
│   ├── collect_ttn.py           ← Person 5 (capture TTN uplinks)
│   └── compute_pdr.py           ← Person 5 (compute PDR from logs)
├── SETTINGUPTTN.md              ← Reference for Person 4
├── IMPLEMENTATION_PLAN_v4.md    ← Detailed reference spec
└── IMPLEMENTATION_PLAN_SIMPLE.md ← This file (build guide)
```

---

## Things Deliberately Left Out

These were in the v4 detailed spec but are **not needed** for the demo or report:

| Removed | Why |
|---------|-----|
| MCCI LMIC library | RadioLib has built-in LoRaWAN support for SX1262 — simpler, same radio object for mesh and LoRaWAN |
| FreeRTOS dual-task edge node | Cooperative loop achieves the same result, far simpler |
| GREEN/YELLOW/RED congestion state machine | 5 nodes at 120s intervals cannot congest. TX jitter + real beacon queue_pct is enough. |
| provision.py + nodes.csv | `#define NODE_ID` is fine for 5 known nodes |
| Adaptive SF7↔SF9 switching | Fixed SF7 covers all lab distances |
| Watchdog timer | Lab demo, not production deployment |
| TTN airtime guard (daily budget tracking) | Keep experiments short; note the TTN fair-use limit in the report |
| EEPROM/NVS persistent counters | Print to serial instead |
| Orphan hold queue with 60s timeout | If parent is lost, just re-parent immediately |
| ESP32 resource budget gates | Check flash size in Arduino IDE after building |
| check_integrity.py, compute_latency.py, plot_results.py | Manual analysis from the 3 kept scripts is sufficient |

---

## What "Done" Looks Like (Acceptance Criteria)

At the demo, you must be able to show **all** of the following:

1. Both sensor nodes transmit **real** DHT22 readings (not random numbers).
2. The relay forwards packets **without modification** — proven by showing bytes match between sensor TX log and TTN decoded payload.
3. Data flows end-to-end: sensor → relay → edge → WisGate → TTN → Pico W dashboard.
4. The dashboard displays per-node temperature, humidity, hop count, and bridge ID, and refreshes automatically.
5. PDR data from at least one 20-minute baseline run (target ≥ 95%).
6. **Relay failure recovery:** Power off relay → sensors re-parent → power on relay → sensors re-parent back. Data flow recovers.
7. **Edge failover:** Power off Edge 1 → relay re-parents to Edge 2 → TTN uplinks now come from `edge-bridge-02`. Data flow continues without manual intervention.
8. **Payload-agnostic proof:** Swap one sensor to send random bytes instead of DHT22 data. The relay continues forwarding without error. Only the TTN decoder output changes.
