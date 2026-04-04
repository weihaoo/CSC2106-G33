# LoRa Mesh Metrics Testing Scenarios

CSC2106 Group 33 - Test Guide for Report Metrics

## Hardware Setup

| Node | Role | Hardware | Notes |
|------|------|----------|-------|
| Node 0x01 | Sensor | LilyGo T-Beam | Generates sensor data |
| Node 0x02 | Sensor | LilyGo T-Beam | Generates sensor data |
| Node 0x03 | Relay | LilyGo T-Beam | Forwards packets only |
| Node 0x04 | Relay | LilyGo T-Beam | Forwards packets only |
| Node 0x05 | Edge | LilyGo T-Beam | Bridges to LoRaWAN |
| WisGate | Gateway | RAK WisGate | LoRaWAN gateway |
| Pi 4B | Server | Raspberry Pi 4 | Runs TTN MQTT + Dashboard |
| **Laptop** | **Logger** | Any with USB | **Runs serial_capture.py** |

---

## Who Runs What

| Script | Run On | Connected To | Purpose |
|--------|--------|--------------|---------|
| `serial_capture.py` | **Laptop** (USB to Edge) | Edge node via USB cable | Capture serial logs |
| `metrics_analysis.py` | **Laptop** (after test) | N/A (reads log files) | Generate graphs/CSV |
| `dashboard/main.py` | **Pi 4B** | TTN via MQTT | Live metrics display |

---

## Testing Priority Guide

**Time Available**: 5-8 hours recommended for comprehensive testing

### Scenario Priorities

| Priority | Scenarios | Time Required | Reason |
|----------|-----------|---------------|--------|
| **[REQUIRED]** | 1, 2, 3, 6, 7 | ~5-6 hours | Core mesh functionality, stress testing, resilience, scalability |
| **[OPTIONAL]** | 5 | ~30 min | Additional resilience data (requires firmware modification) |
| **[SKIP]** | 4 | ~1.5 hours | Requires outdoor space (50-100m+ separation) |

### Recommended Test Sequence

Follow this order for optimal workflow (minimizes reflashing and setup changes):

1. **Setup & Baseline** (30 min) - Scenario 1
2. **Stress Testing** (30 min) - Scenario 2 (all sub-scenarios: 2a, 2b, 2c)
3. **Load & Scalability** (40 min) - Scenario 7 (sub-scenarios 7a, 7b, 7c, 7d)
4. **Multi-Hop Routing** (45 min) - Scenario 3 (both variations + per-hop analysis)
5. **Resilience & Recovery** (30 min) - Scenario 6 (with recovery time measurement)
6. **Optional: Burst** (30 min) - Scenario 5 (if time permits)
7. **Analysis** (60 min) - Generate all reports and graphs

**Total Core Testing**: ~5-6 hours (scenarios 1, 2, 3, 6, 7 with all sub-scenarios)

---

## Pre-Test Checklist

1. **Flash firmware** to all nodes (`sensor_node/`, `relay_node/`, `edge_node/`)
2. **Verify NTP sync** - Check serial output shows valid timestamps on BOTH sensor and edge
3. **Connect Edge to laptop** via USB cable
4. **Start serial capture** on laptop (connected to Edge node):
   ```bash
   # On your laptop (Linux/Mac/Windows)
   cd /path/to/CSC2106-G33
   pip install pyserial
   python tools/serial_capture.py --output logs/scenario_X.log
   ```
5. **Start dashboard** on Pi 4B:
   ```bash
   # On Raspberry Pi 4B
   pip install paho-mqtt flask
   python dashboard/main.py
   ```
6. **Label log files** clearly for each scenario

---

## Scenario 1: Baseline (Normal Operation) [REQUIRED]

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_1.log
    ```

**Goal:** Establish baseline metrics under ideal conditions.

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Run with `TX_INTERVAL_MS = 5000` | 10 min |
| 0x02 (Sensor) | Run with `TX_INTERVAL_MS = 5000` | 10 min |
| 0x05 (Edge) | Capture serial logs | 10 min |

**Expected Metrics:**
- Latency: 100-300 ms
- PDR: >95%
- Hop count: 1 (direct)

**Log file:** `logs/scenario_1.log`

---

## Scenario 2: TX Interval Stress Test [REQUIRED]

**Goal:** Find throughput limits by reducing transmission interval.

### 2a: 2-second interval

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_2_2s.log
    ```


| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Change `TX_INTERVAL_MS = 2000`, reflash | 5 min |
| 0x02 (Sensor) | Change `TX_INTERVAL_MS = 2000`, reflash | 5 min |
| 0x05 (Edge) | Capture serial logs | 5 min |

**Log file:** `logs/scenario_2_1s.log`

### 2b: 1-second interval

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_2_1s.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Change `TX_INTERVAL_MS = 1000`, reflash | 5 min |
| 0x02 (Sensor) | Change `TX_INTERVAL_MS = 1000`, reflash | 5 min |
| 0x05 (Edge) | Capture serial logs | 5 min |

**Log file:** `logs/scenario_2_1s.log`

### 2c: 500ms interval (stress)

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_2_500ms.log
    ```


| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Change `TX_INTERVAL_MS = 500`, reflash | 3 min |
| 0x02 (Sensor) | Change `TX_INTERVAL_MS = 500`, reflash | 3 min |
| 0x05 (Edge) | Capture serial logs | 3 min |

**Log file:** `logs/scenario_2_500ms.log`

**Expected:** PDR degrades, collisions increase, latency rises.

---

## Scenario 3: Multi-Hop Chain [REQUIRED]

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_3.log
    ```

**Goal:** Measure latency and PDR increase with hop count.

### Option A: Physical Spacing (Outdoor/Large Indoor)

Place nodes in a line, far enough apart that direct links are weak/impossible.

```
[Sensor 0x03] --50m-- [Relay 0x02] --50m-- [Edge 0x01]
```

### Option B: Forced Topology via Whitelist (Indoor/Close Proximity)

If you cannot physically space nodes apart, use the **PARENT_WHITELIST** feature
to force a specific routing path. This makes nodes reject beacons from non-whitelisted
parents, forcing multi-hop even when all nodes are within direct range.

**How to configure (edit each node's .ino file before flashing):**

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ FORCED 3-HOP LINEAR TOPOLOGY                                                │
│                                                                             │
│   Sensor 0x04 → Sensor 0x03 → Relay 0x02 → Edge 0x01                       │
│     (3 hops)      (2 hops)      (1 hop)      (edge)                        │
│                                                                             │
│ Configuration per node:                                                     │
│   sensor_node.ino (0x04): #define PARENT_WHITELIST {0x03}                  │
│   sensor_node.ino (0x03): #define PARENT_WHITELIST {0x02}                  │
│   relay_node.ino  (0x02): #define PARENT_WHITELIST {0x01}                  │
│   edge_node.ino   (0x01): (no whitelist needed - edge has no parent)       │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Steps to enable:**
1. Open `sensor_node/sensor_node.ino` (for each sensor)
2. Find the commented `// #define PARENT_WHITELIST {0x03}` line
3. Uncomment and set the allowed parent ID(s)
4. Flash the node
5. Repeat for relay_node if needed

**To disable (return to normal auto-routing):**
- Comment out the `#define PARENT_WHITELIST` line and reflash

**Serial output when whitelist rejects a beacon:**
```
BCN | WHITELIST_REJECT src=0x01 | not in whitelist (0x02)
```

### Test Procedure

| Node | Whitelist | Expected Parent | Expected Hops |
|------|-----------|-----------------|---------------|
| 0x04 (Sensor) | `{0x03}` | 0x03 | 3 |
| 0x03 (Sensor) | `{0x02}` | 0x02 | 2 |
| 0x02 (Relay) | `{0x01}` | 0x01 | 1 |
| 0x01 (Edge) | (none) | - | 0 |

| Node | Action | Duration |
|------|--------|----------|
| All | Flash with whitelist config | - |
| 0x01 (Edge) | Start first, capture logs | 10 min |
| 0x02 (Relay) | Start after edge is up | 10 min |
| 0x03 (Sensor) | Start after relay is up | 10 min |
| 0x04 (Sensor) | Start last | 10 min |

**Expected Metrics:**
- Hop count: 1, 2, or 3 (depending on source node)
- Latency: Increases ~50-150ms per hop
- PDR: 80-95%

**Log files:** `logs/multihop_forced.log`

### Per-Hop Latency Analysis

After running the multi-hop test, analyze the relationship between hop count and latency:

1. **Group packets by hop count**: Extract all packets with `hops=1`, `hops=2`, `hops=3`
2. **Calculate average latency per hop count**:
   - Average latency at 1 hop: `avg_latency_1`
   - Average latency at 2 hops: `avg_latency_2`
   - Average latency at 3 hops: `avg_latency_3`
3. **Calculate per-hop forwarding delay**:
   - Per-hop delay ≈ `(avg_latency_2 - avg_latency_1)` or `(avg_latency_3 - avg_latency_2)`
   - If per-hop delay is consistent (~50-100ms), forwarding is efficient
4. **Analyze variance**: Higher variance at more hops indicates congestion/retries

**Expected per-hop forwarding delay**: 50-150ms (includes CAD, ACK wait, processing)

---

## Scenario 4: Distance/Signal Stress [SKIP - Outdoor Space Required]

**Note:** This scenario requires 50-100m+ separation and is not feasible in limited indoor environments. Skip if testing space is constrained. Focus on Scenarios 1-3, 6-7 instead.

**Goal:** Test performance at range limits.

### 4a: Near range (10m line-of-sight)

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_4a_10m.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | 10m from Edge, clear LOS | 5 min |
| 0x05 (Edge) | Capture logs, note RSSI | 5 min |

### 4b: Medium range (50m line-of-sight)

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_4b_50m.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | 50m from Edge, clear LOS | 5 min |
| 0x05 (Edge) | Capture logs, note RSSI | 5 min |

### 4c: Far range (100m+ or obstructed)

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_4c_100m.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | 100m+ or through walls | 5 min |
| 0x05 (Edge) | Capture logs, note RSSI | 5 min |

**Log files:** `logs/distance_10m.log`, `logs/distance_50m.log`, `logs/distance_100m.log`

**Expected:** RSSI drops, PDR decreases at extreme range.

---

## Scenario 5: Burst Traffic [OPTIONAL - If Time Permits]

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_5_burst.log
    ```

**Note:** Requires firmware modification. Test only if you have extra time after completing Scenarios 1-3, 6-7.

**Goal:** Test network response to sudden traffic spike.

**Requires firmware modification** - Add burst function to sensor:

```cpp
// In sensor_node.ino, add button handler or timer trigger:
void sendBurst(int count) {
    for (int i = 0; i < count; i++) {
        sendSensorData();
        delay(100);  // 100ms between packets
    }
}
```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Trigger 10-packet burst | Instant |
| 0x02 (Sensor) | Normal TX every 5s | 2 min |
| 0x05 (Edge) | Capture logs | 2 min |

**Repeat burst** 3 times with 30s gaps.

**Log file:** `logs/burst_traffic.log`

**Expected:** Some packets lost during burst, recovery after.

---

## Scenario 6: Node Failure/Recovery [REQUIRED]

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_6_failure.log
    ```

**Goal:** Test mesh resilience when nodes drop and measure recovery time.

### Recovery Time Measurement Protocol

To measure recovery time accurately:
1. **Before power-off**: Note the timestamp of the last successful packet from the target node
2. **During power-off**: Record the exact power-off time (use a stopwatch or note serial log timestamp)
3. **After power-on**: Record the exact power-on time and note when first packet arrives
4. **Recovery time** = (First successful packet timestamp) - (Power-on timestamp)

**Expected Recovery Time**: Based on `PARENT_TIMEOUT_MS = 35000` (35s parent timeout), recovery should occur within:
- Immediate if parent is still valid (no timeout)
- Up to 35s if parent selection is required

| Time | Action | Record |
|------|--------|--------|
| 0:00 | Start all nodes normally | Note first packet timestamps |
| 2:00 | Power off Relay 0x03 | Note last packet time before power-off |
| 4:00 | Power on Relay 0x03 | Note power-on time, watch for first packet |
| 6:00 | Power off Sensor 0x02 | Note last packet time from 0x02 |
| 8:00 | Power on Sensor 0x02 | Note power-on time, watch for first packet from 0x02 |
| 10:00 | End test | |

| Node | Action | Duration |
|------|--------|----------|
| 0x01, 0x02 (Sensor) | Normal TX | 10 min |
| 0x03 (Relay) | Toggle power at 2:00/4:00 | 10 min |
| 0x05 (Edge) | Capture logs with timestamps | 10 min |

**Log file:** `logs/node_failure.log`

**Expected:** 
- Temporary PDR drop during failure
- Recovery within 35s (parent timeout period)
- Calculate actual recovery time from logs for report

**Metrics to Extract:**
- Recovery time for relay failure
- Recovery time for sensor failure
- PDR during failure window vs normal operation
- Any route changes (parent selection events)

---

## Scenario 7: All Nodes Active (Load Test & Scalability) [REQUIRED]

**Goal:** Maximum realistic load on the mesh and scalability analysis.

### 7a: Baseline Load (5s interval)

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_7a_5s.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | TX every 5s (`TX_INTERVAL_MS = 5000`) | 5 min |
| 0x02 (Sensor) | TX every 5s (`TX_INTERVAL_MS = 5000`) | 5 min |
| 0x03, 0x04 (Relay) | Forward only | 5 min |
| 0x05 (Edge) | Capture logs | 5 min |

**Log file:** `logs/scenario_7a_5s.log`

### 7b: Standard Load (3s interval)

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_7b_3s.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | TX every 3s (`TX_INTERVAL_MS = 3000`) | 5 min |
| 0x02 (Sensor) | TX every 3s (`TX_INTERVAL_MS = 3000`) | 5 min |
| 0x03, 0x04 (Relay) | Forward only | 5 min |
| 0x05 (Edge) | Capture logs | 5 min |

**Log file:** `logs/scenario_7b_3s.log`

### 7c: High Load - Simulating ~10 Sensors (1s interval)

With 2 sensors at 1s interval, this simulates the aggregate traffic of ~10 sensors at 5s interval.

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_7c_1s.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | TX every 1s (`TX_INTERVAL_MS = 1000`) | 5 min |
| 0x02 (Sensor) | TX every 1s (`TX_INTERVAL_MS = 1000`) | 5 min |
| 0x03, 0x04 (Relay) | Forward only | 5 min |
| 0x05 (Edge) | Capture logs | 5 min |

**Log file:** `logs/scenario_7c_1s.log`

### 7d: Maximum Load - Simulating ~20 Sensors (500ms interval)

With 2 sensors at 500ms interval, this simulates the aggregate traffic of ~20 sensors at 5s interval.

**RUN THIS TO CAPTURE LOGS:**
    ```
    python tools/serial_capture.py --port COM12 --output logs/scenario_7d_500ms.log
    ```

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | TX every 500ms (`TX_INTERVAL_MS = 500`) | 3 min |
| 0x02 (Sensor) | TX every 500ms (`TX_INTERVAL_MS = 500`) | 3 min |
| 0x03, 0x04 (Relay) | Forward only | 3 min |
| 0x05 (Edge) | Capture logs | 3 min |

**Log file:** `logs/scenario_7d_500ms.log`

### Expected Results for Scalability Analysis

| Sub-scenario | TX Interval | Simulated Sensors | Expected PDR | Expected Latency |
|--------------|-------------|-------------------|--------------|------------------|
| 7a | 5s | 2 | >95% | 100-300ms |
| 7b | 3s | ~3 | >90% | 100-400ms |
| 7c | 1s | ~10 | 70-90% | 200-500ms |
| 7d | 500ms | ~20 | 50-80% | 300-800ms |

### Scalability Calculation for Report

- **LoRa duty cycle**: ~1% recommended for AS923 regulatory compliance
- **Packet airtime at SF7/125kHz**: ~50ms for 21-byte packet (10-byte header + 11-byte payload)
- **Theoretical max packets/second**: ~20 (at 100% duty cycle)
- **With ACKs + retries**: ~5-10 effective packets/second
- **Practical sensor limit**: Based on test results, extrapolate max sensors at target PDR (e.g., 95%)

---

## Post-Test Analysis

After each scenario, run the analysis script **on your laptop** (where the log files are):

```bash
# On your laptop - make sure you have dependencies
pip install matplotlib pandas

# Create output directory
mkdir -p report

# Single scenario analysis
python tools/metrics_analysis.py logs/baseline_10min.log --output report/baseline/

# Compare multiple scenarios (generates comparison charts)
python tools/metrics_analysis.py logs/tx_*.log --output report/tx_comparison/
```

### Complete Workflow Example

```bash
# 1. DURING TEST: Run on laptop connected to Edge via USB
python tools/serial_capture.py --output logs/baseline_10min.log
# ... wait 10 minutes, then Ctrl+C to stop ...

# 2. AFTER TEST: Generate report graphs (still on laptop)
python tools/metrics_analysis.py logs/baseline_10min.log --output report/baseline/

# 3. View results
ls report/baseline/
# -> summary.csv, packets.csv, latency_histogram.png, etc.
```

### Generated Files

| File | Description |
|------|-------------|
| `summary.csv` | Key metrics table |
| `packets.csv` | Raw packet data |
| `latency_histogram.png` | Latency distribution |
| `latency_timeline.png` | Latency over time |
| `pdr_by_node.png` | PDR per source node |
| `hop_distribution.png` | Hop count frequency |
| `throughput_timeline.png` | Throughput over time |
| `packet_loss_cumulative.png` | Cumulative packet loss |

---

## Quick Reference: Firmware Locations

| Node Type | Firmware Path | Key Config |
|-----------|---------------|------------|
| Sensor | `sensor_node/sensor_node.ino` | `TX_INTERVAL_MS` |
| Relay | `relay_node/relay_node.ino` | - |
| Edge | `edge_node/edge_node.ino` | - |

**To change TX interval:**
1. Edit `sensor_node/sensor_node.ino`
2. Find `#define TX_INTERVAL_MS 5000`
3. Change value (e.g., `2000` for 2s)
4. Flash to sensor nodes

---

## Comparison Data (for Report)

| Technology | Typical Latency | Typical Throughput | Range |
|------------|-----------------|-------------------|-------|
| LoRa Mesh | 100-500 ms | 0.3-5 kbps | 2-15 km |
| WiFi | 1-10 ms | 50-500 Mbps | 50-100 m |
| 4G LTE | 30-50 ms | 5-50 Mbps | 1-10 km |
| 5G | 1-10 ms | 100-1000 Mbps | 0.5-2 km |

**Key differentiators for LoRa:**
- Ultra-low power consumption
- Long range without infrastructure
- Works in remote/rural areas
- Trade-off: Low data rate
