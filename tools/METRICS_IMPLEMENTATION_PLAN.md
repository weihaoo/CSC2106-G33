# LoRa Mesh Metrics Implementation Plan

## Problem Statement

Add comprehensive network metrics to demonstrate **LoRa-to-LoRaWAN mesh gateway performance** and reveal limitations under stress testing. This is for a report showcasing our implementation's capabilities.

**Target branch**: `latency` (currently checked out)

## Metrics to Implement

| Metric | Definition | Calculation Location | Why Important |
|--------|------------|---------------------|---------------|
| **Latency** | One-way delay (sensor TX → edge RX) | Edge node (NTP timestamps) | Core mesh performance; shows multi-hop delay |
| **PDR** | Packets received / packets sent | Dashboard (seq tracking) | Reliability metric; shows robustness under stress |
| **Hop Count** | DEFAULT_TTL - received_TTL | Edge node | Routing efficiency; topology insights |
| **Packet Loss** | 1 - PDR (percentage) | Dashboard | Explicit failure metric for stress tests |
| **Throughput** | Bytes delivered per second | Dashboard (30s window) | Saturation point under load |

## Graphs for Report

| # | Graph Type | X-Axis | Y-Axis | Insight |
|---|------------|--------|--------|---------|
| 1 | **Latency Distribution** (Histogram) | Latency (ms) | Frequency | Shows spread, mean, outliers |
| 2 | **Latency Over Time** (Line) | Time (s) | Latency (ms) | Stability; spikes = stress |
| 3 | **PDR by Node** (Bar Chart) | Node ID | PDR (%) | Identifies weak links |
| 4 | **Hop Count Distribution** (Histogram) | Hops | Frequency | Routing efficiency |
| 5 | **Throughput Over Time** (Line) | Time (s) | Bytes/sec | Saturation point |
| 6 | **Cumulative Packet Loss** (Line) | Time (s) | Packets Lost | Failure accumulation |

## Stress Testing Scenarios (No Additional Hardware Required)

You have: **5 T-Beams, 2 WisGates, 1 Pi 4B**

### Scenario 1: TX Interval Stress Test
**Goal**: Find throughput ceiling and latency degradation point.

**Implementation**:
1. In `sensor_node/sensor_node.ino`, change:
   ```cpp
   #define TX_INTERVAL_MS  5000UL   // Baseline
   // Then test with: 2000UL, 1000UL, 500UL
   ```
2. Flash each sensor with same interval
3. Run 5-minute test at each interval
4. Capture logs, generate graphs

**Expected results**:
- 5s interval: Low latency, high PDR (~99%)
- 2s interval: Slight latency increase
- 1s interval: Latency spikes, PDR drops (~90%)
- 500ms: Mesh saturation, PDR < 80%

---

### Scenario 2: Multi-Hop Chain Test
**Goal**: Show latency increase per hop.

**Implementation**:
1. Configure T-Beams as chain:
   - Node 0x03: Sensor (furthest from edge)
   - Node 0x04: Relay (middle)
   - Node 0x02: Relay (closer)
   - Node 0x01: Edge
2. In each node's routing, lower `MIN_PARENT_RSSI`:
   ```cpp
   #define MIN_PARENT_RSSI  -115  // Force weaker links
   ```
3. Physically position to force 3-hop path
4. Compare latency: 1-hop vs 2-hop vs 3-hop

**Expected results**:
- 1 hop: ~100-200ms latency
- 2 hops: ~200-400ms latency
- 3 hops: ~400-600ms latency

---

### Scenario 3: Distance/Signal Stress Test
**Goal**: Find range limits and RSSI threshold.

**Implementation**:
1. Start with nodes close together (RSSI ~-60 dBm)
2. Gradually move sensor away from edge (10m increments)
3. Record RSSI vs latency vs PDR at each distance
4. Test until PDR drops below 80%

**Expected results**:
- RSSI > -80 dBm: PDR ~99%, low latency
- RSSI -80 to -100 dBm: PDR 95%, slight latency increase
- RSSI < -105 dBm: PDR drops, retransmissions increase

---

### Scenario 4: Burst Traffic Test
**Goal**: Test queue handling under sudden load.

**Implementation** (new function in sensor firmware):
```cpp
// Add to sensor_node.ino - send burst on button press
#define BURST_BUTTON_PIN 38  // T-Beam user button

void send_burst(int count) {
    for (int i = 0; i < count; i++) {
        // Build and send packet (same as normal TX)
        // ... existing send_with_ack code ...
        delay(100);  // 100ms between burst packets
    }
}

void loop() {
    // Add to existing loop
    if (digitalRead(BURST_BUTTON_PIN) == LOW) {
        Serial.println("BURST | Sending 10 packets rapidly");
        send_burst(10);
        delay(1000);  // Debounce
    }
    // ... rest of loop ...
}
```

**Expected results**:
- Queue fills quickly
- Some packets dropped or delayed
- Recovery time measurable

---

### Scenario 5: Node Failure Recovery Test
**Goal**: Measure rerouting time when relay fails.

**Implementation**:
1. Setup: Sensor → Relay → Edge (2-hop path)
2. Sensor is transmitting normally
3. Power off Relay suddenly
4. Observe:
   - Packets lost during parent timeout (~35s)
   - Time to select new parent (if Edge reachable)
   - PDR during and after failure

**Expected results**:
- ~35s blackout (PARENT_TIMEOUT_MS)
- PDR drops to 0% during blackout
- Recovery when new parent found

## Data Collection Workflow

### Two Options for Capturing Metrics:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ OPTION A: Dashboard via TTN (Real-time display)                             │
│                                                                             │
│   Sensor ──▶ Relay ──▶ Edge ──▶ TTN (LoRaWAN) ──▶ Dashboard (Pi)           │
│                                      │                                      │
│                                 decoder.js                                  │
│                                 extracts latency                            │
│                                                                             │
│   ✓ End-to-end flow                                                         │
│   ✓ Live web dashboard                                                      │
│   ✗ Requires LoRaWAN to be working                                          │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ OPTION B: Serial Logs via USB (For report/analysis)                         │
│                                                                             │
│   Edge Node ──USB──▶ PC ──▶ serial_capture.py ──▶ test_run.log              │
│                                                          │                  │
│                                                          ▼                  │
│                                               metrics_analysis.py           │
│                                               generates 6 graphs + CSV      │
│                                                                             │
│   ✓ Works without LoRaWAN                                                   │
│   ✓ Can copy-paste from Arduino IDE instead                                 │
│   ✓ Offline analysis for report                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Option B Detail: Serial to Graphs Pipeline

```
Step 1: CAPTURE (choose one)
┌─────────────────────────────────────────────────────────────────────────────┐
│ Method A: Python serial_capture.py                                          │
│                                                                             │
│   $ pip install pyserial                                                    │
│   $ python serial_capture.py --port COM3 --output logs/test_run_001.log    │
│                                                                             │
│   (Leave running during test, Ctrl+C to stop)                               │
│                                                                             │
│ Method B: Copy-paste from Arduino IDE                                       │
│                                                                             │
│   1. Open Arduino IDE Serial Monitor (115200 baud)                          │
│   2. Run test for 5 minutes                                                 │
│   3. Select All → Copy → Paste into test_run_001.log                        │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼ (same .log file)
Step 2: ANALYZE
┌─────────────────────────────────────────────────────────────────────────────┐
│ Python metrics_analysis.py                                                  │
│                                                                             │
│   $ pip install matplotlib pandas                                           │
│   $ python metrics_analysis.py logs/test_run_001.log --output report/      │
│                                                                             │
│   What it does:                                                             │
│   1. Reads .log file                                                        │
│   2. Uses regex to extract:                                                 │
│      - RX_MESH lines → seq, hops, rssi                                      │
│      - LATENCY lines → latency_ms                                           │
│   3. Calculates PDR from sequence gaps                                      │
│   4. Generates 6 PNG graphs + summary.csv                                   │
│                                                                             │
│   Output:                                                                   │
│   report/                                                                   │
│   ├── summary.csv                                                           │
│   ├── latency_histogram.png                                                 │
│   ├── latency_timeline.png                                                  │
│   ├── pdr_by_node.png                                                       │
│   ├── hop_distribution.png                                                  │
│   ├── throughput_timeline.png                                               │
│   └── packet_loss_cumulative.png                                            │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Summary: Yes, Two Scripts

| Script | Purpose | Dependencies |
|--------|---------|--------------|
| `serial_capture.py` | Capture serial logs from Edge via USB | `pyserial` |
| `metrics_analysis.py` | Parse logs → generate graphs/CSV | `matplotlib`, `pandas` |

**You run them sequentially**:
1. `serial_capture.py` during test → produces `.log` file
2. `metrics_analysis.py` after test → produces graphs from `.log`

Or skip `serial_capture.py` entirely and copy-paste from Arduino IDE.

## How Latency is Captured (Existing Implementation)

The system already has NTP-based latency measurement. Here's the complete flow:

### Step 1: Sensor Node Embeds Timestamp
**File**: `sensor_node/sensor_node.ino` → `build_sensor_payload()`

```
┌─────────────────────────────────────────────────────────────┐
│ SENSOR NODE (at TX time)                                    │
├─────────────────────────────────────────────────────────────┤
│ 1. Connect to WiFi at boot                                  │
│ 2. Sync NTP (pool.ntp.org) → stores _sync_epoch + _sync_ms  │
│ 3. Disconnect WiFi (saves power, avoids SPI conflicts)      │
│ 4. When sending packet:                                     │
│    uint32_t ts = get_ntp_epoch_s();  // Unix seconds        │
│    payload[7..10] = ts (big-endian)  // Embed in packet     │
└─────────────────────────────────────────────────────────────┘
```

**Sensor payload bytes [7-10]**: NTP timestamp at TX (0 if not synced)

### Step 2: Edge Node Computes Latency
**File**: `edge_node/edge_packets.h` → `handle_data()`

```
┌─────────────────────────────────────────────────────────────┐
│ EDGE NODE (at RX time)                                      │
├─────────────────────────────────────────────────────────────┤
│ 1. Also syncs NTP at boot (same pool.ntp.org)               │
│ 2. When packet arrives:                                     │
│    recv_time_s = get_ntp_epoch_s();       // Edge RX time   │
│    send_ts = payload[7..10];               // From sensor   │
│    latency_ms = (recv_time_s - send_ts) × 1000              │
│                 + (millis() - _sync_millis) % 1000          │
│ 3. Prints: LATENCY | src=0x03 | one-way ~123 ms             │
└─────────────────────────────────────────────────────────────┘
```

### Current Limitation
**Latency is only printed to Serial** - NOT sent to TTN or dashboard.

### What This Plan Adds
1. **Store** `latency_ms` in `AggRecord` when packet received
2. **Pack** into LoRaWAN uplink (new 2-byte field in BridgeRecord)
3. **Decode** in TTN payload formatter
4. **Display** on dashboard

### NTP Accuracy
- ESP32 NTP accuracy: ±10-50ms
- This is acceptable for LoRa (air time ~100ms+ at SF7)
- Both sensor and edge sync to same NTP pool → minimal clock skew

### Diagram

```
SENSOR                                    EDGE
  │                                         │
  │ NTP sync (boot)                         │ NTP sync (boot)
  │ _sync_epoch = 1712345600               │ _sync_epoch = 1712345600
  ▼                                         ▼
  
  ╔═══════════════╗    LoRa TX    ╔═══════════════╗
  ║ Build Packet  ║──────────────▶║ Receive Pkt   ║
  ║ ts = 1712345678║              ║ recv = 1712345678.5║
  ╚═══════════════╝               ╚═══════════════╝
                                          │
                                          ▼
                                  latency = 0.5s = 500ms
                                  Serial: LATENCY | one-way ~500 ms
```

## Implementation Approach

### Phase 1: Edge Node - Embed Latency in LoRaWAN Payload

**File**: `edge_node/edge_packets.h`, `shared/mesh_protocol.h`

1. Extend `BridgeRecord` struct to include latency:
   ```cpp
   // Current: 14 bytes per record
   // New: 16 bytes per record (+2 bytes for latency_ms as uint16)
   typedef struct __attribute__((packed)) {
       uint8_t  mesh_src_id;
       uint8_t  mesh_seq;
       uint8_t  sensor_type_hint;
       uint8_t  hop_estimate;
       uint16_t edge_uptime_s;
       uint16_t latency_ms;      // NEW: one-way latency in ms (0 = no NTP)
       uint8_t  opaque_len;
       uint8_t  opaque_payload[7];
   } BridgeRecord;
   ```

2. Compute and store latency when adding to aggregation buffer
3. Update `BRIDGE_RECORD_SIZE` to 16

### Phase 2: TTN Decoder - Parse Latency

**File**: `tools/decoder.js`

1. Update decoder to extract `latency_ms` field from each record
2. Add to output JSON: `rec.latency_ms`

### Phase 3: Dashboard - Display Metrics

**File**: `dashboard/main.py`

1. **Data structures**:
   - Per-node: `last_seq`, `packets_received`, `packets_expected`, `latencies[]`, `hop_counts[]`
   - Global: `bytes_received`, `window_start_time`

2. **Calculations**:
   - **PDR**: Track sequence numbers, detect gaps → `received / (received + gaps)`
   - **Throughput**: `bytes_received_in_window / 30` (bytes/sec over 30s rolling window)
   - **Avg Latency**: Mean of recent latencies per node

3. **UI additions**:
   - Summary panel: Total throughput, overall PDR, avg latency
   - Per-node columns: Latency (ms), PDR (%), Hop Count

### Phase 4: Python Serial Capture Script

**File**: `tools/serial_capture.py` (new)

**Is it feasible? YES** - `pyserial` is a mature, widely-used library that works on Windows/Mac/Linux.

1. **How it works**:
   ```python
   import serial
   ser = serial.Serial('COM3', 115200)  # Same baud rate as Arduino
   while True:
       line = ser.readline().decode('utf-8')
       print(line, end='')
       log_file.write(line)
   ```

2. **Usage**: `python serial_capture.py --port COM3 --output logs/test_run_001.log`
3. **Auto-detect**: Lists available ports, prompts user to select
4. **Timestamps**: Adds `[HH:MM:SS.mmm]` prefix to each line for precise timing
5. **Dependencies**: `pip install pyserial`

**Note**: While capturing, you cannot use Arduino IDE Serial Monitor (only one app can access the port). Either use the capture script OR Arduino IDE, not both.

### Phase 5: Python Analysis Script

**File**: `tools/metrics_analysis.py` (new)

**Smart filtering? YES** - The script will use regex patterns to extract only metrics-relevant lines.

**Log lines to parse** (based on actual edge node output):
```
RX_MESH | src=0x03 | seq=42 | hops=2 | rssi=-85    ← extract src, seq, hops, rssi
LATENCY | src=0x03 | send_ts=... | one-way ~123 ms ← extract src, latency_ms
[DROP] Duplicate | src=0x03 seq=42                  ← track as dropped (not lost)
[BCN] RX from 0x02 | rank=1 | rssi=-72              ← IGNORED (not metrics)
[INIT] LoRaWAN DISABLED...                          ← IGNORED
```

**Filtering logic**:
```python
PATTERNS = {
    'rx_data': r'RX_MESH \| src=0x(\w+) \| seq=(\d+) \| hops=(\d+) \| rssi=(-?\d+)',
    'latency': r'LATENCY \| src=0x(\w+) .* one-way ~(\d+) ms',
    'drop': r'\[DROP\] .* src=0x(\w+) seq=(\d+)',
}
# Lines not matching any pattern are silently ignored
```

**Input flexibility**:
- Copy-paste from Arduino IDE works fine
- Timestamps optional (script uses line order if no timestamps)
- Handles mixed output (beacons, errors, etc.) by filtering

**Output files** (all 6 graphs + CSV):
```
report/
├── summary.csv               # Table of all metrics
├── latency_histogram.png     # Latency distribution
├── latency_timeline.png      # Latency over time
├── pdr_by_node.png           # Per-node PDR bar chart
├── hop_distribution.png      # Hop count histogram
├── throughput_timeline.png   # Throughput over time
└── packet_loss_cumulative.png # Cumulative loss over time
```

**Usage**:
```bash
# Analyze a log file, generate all 6 graphs
python metrics_analysis.py logs/test_run_001.log --output report/

# Compare multiple test runs (e.g., normal vs stress)
python metrics_analysis.py logs/normal.log logs/stress.log --compare --output comparison/
```

---

## Detailed Todos

### 1. Extend BridgeRecord with latency field
- **File**: `shared/mesh_protocol.h`
- **Change**: Add `uint16_t latency_ms` to BridgeRecord struct
- **Update**: `BRIDGE_RECORD_SIZE` from 14 → 16

### 2. Compute and store latency in aggregation buffer
- **File**: `edge_node/edge_packets.h`
- **Change**: In `handle_data()`, after computing `latency_ms`, store it in `AggRecord`
- **Update**: `AggRecord` struct to include `latency_ms`

### 3. Pack latency into LoRaWAN uplink
- **File**: `edge_node/edge_lorawan.h`
- **Change**: When building BridgeAggV1 payload, include latency_ms (big-endian)

### 4. Update TTN decoder
- **File**: `tools/decoder.js`
- **Change**: Parse 16-byte records, extract `latency_ms` at new offset

### 5. Dashboard: Add metrics tracking state
- **File**: `dashboard/main.py`
- **Change**: Add per-node tracking dict with seq history, latency list, hop counts

### 6. Dashboard: Calculate PDR from sequence gaps
- **File**: `dashboard/main.py`
- **Logic**: Compare `current_seq` to `last_seq + 1`, count gaps as lost packets

### 7. Dashboard: Calculate throughput (30s rolling window)
- **File**: `dashboard/main.py`
- **Logic**: Track bytes received with timestamps, sum last 30s, divide by 30

### 8. Dashboard: Update HTML with metrics display
- **File**: `dashboard/main.py`
- **UI**: Add summary panel + per-node latency/PDR/hops columns

### 9. Create serial capture script
- **File**: `tools/serial_capture.py` (new)
- **Features**: pyserial-based capture, auto port detection, timestamped output, clean Ctrl+C handling

### 10. Create Python analysis script
- **File**: `tools/metrics_analysis.py` (new)
- **Features**: 
  - Regex-based filtering (only parses RX_MESH, LATENCY, DROP lines; ignores beacons/status)
  - Works with copy-pasted text or captured logs
  - CSV export, matplotlib charts for latency/throughput/PDR

### 10. Test end-to-end metrics flow
- Verify latency appears in dashboard
- Verify PDR calculation with intentional packet loss
- Test serial capture script with real device
- Generate sample report data

---

## Serial Capture & Analysis Workflow

```
┌─────────────────┐     USB/Serial      ┌────────────────────┐
│   Edge Node     │ ──────────────────> │  serial_capture.py │
│   (T-Beam)      │   LATENCY | ...     │  saves to .log     │
└─────────────────┘   RX DATA | ...     └────────────────────┘
                                                 │
                                                 ▼
                                        ┌────────────────────┐
                                        │ metrics_analysis.py│
                                        │  - CSV export      │
                                        │  - PNG charts      │
                                        └────────────────────┘
```

**Alternative**: Copy-paste from Arduino IDE Serial Monitor into a `.log` file, then run `metrics_analysis.py` on it.

---

## File Changes Summary

| File | Change Type | Description |
|------|-------------|-------------|
| `shared/mesh_protocol.h` | Modify | Add latency_ms to BridgeRecord |
| `edge_node/edge_packets.h` | Modify | Store latency in AggRecord, compute on RX |
| `edge_node/edge_lorawan.h` | Modify | Pack latency into uplink payload |
| `tools/decoder.js` | Modify | Parse 16-byte records with latency |
| `dashboard/main.py` | Modify | Add metrics tracking, PDR calc, throughput, UI |
| `tools/serial_capture.py` | Create | Capture serial logs via USB |
| `tools/metrics_analysis.py` | Create | Python script for report generation |

---

## Risks & Mitigations

1. **LoRaWAN payload size**: Adding 2 bytes × 7 records = 14 more bytes. Current max ~94 bytes, new ~108 bytes. Still within DR5 (SF7/125kHz) limit of 242 bytes. ✅ OK

2. **Dashboard memory (Pico W)**: Storing latency history could exhaust RAM. Mitigation: Keep only last 10 latencies per node, use fixed-size arrays.

3. **NTP accuracy**: ±10-50ms accuracy means latency measurements have inherent error. Document this in report comparison section.

---

## Testing Checklist

- [ ] Edge node computes and logs latency correctly
- [ ] LoRaWAN payload includes latency bytes
- [ ] TTN decoder outputs latency_ms in JSON
- [ ] Dashboard receives and displays latency
- [ ] PDR calculation handles sequence wraps (0xFF → 0x00)
- [ ] Throughput calculation resets properly on window boundary
- [ ] Python script generates readable CSV/charts
