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

## Scenario 1: Baseline (Normal Operation)

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

**Log file:** `logs/baseline_10min.log`

---

## Scenario 2: TX Interval Stress Test

**Goal:** Find throughput limits by reducing transmission interval.

### 2a: 2-second interval

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Change `TX_INTERVAL_MS = 2000`, reflash | 5 min |
| 0x02 (Sensor) | Change `TX_INTERVAL_MS = 2000`, reflash | 5 min |
| 0x05 (Edge) | Capture serial logs | 5 min |

**Log file:** `logs/tx_2s.log`

### 2b: 1-second interval

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Change `TX_INTERVAL_MS = 1000`, reflash | 5 min |
| 0x02 (Sensor) | Change `TX_INTERVAL_MS = 1000`, reflash | 5 min |
| 0x05 (Edge) | Capture serial logs | 5 min |

**Log file:** `logs/tx_1s.log`

### 2c: 500ms interval (stress)

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Change `TX_INTERVAL_MS = 500`, reflash | 3 min |
| 0x02 (Sensor) | Change `TX_INTERVAL_MS = 500`, reflash | 3 min |
| 0x05 (Edge) | Capture serial logs | 3 min |

**Log file:** `logs/tx_500ms.log`

**Expected:** PDR degrades, collisions increase, latency rises.

---

## Scenario 3: Multi-Hop Chain

**Goal:** Measure latency and PDR increase with hop count.

**Physical Setup:**
```
[Sensor 0x01] --50m-- [Relay 0x03] --50m-- [Relay 0x04] --50m-- [Edge 0x05]
```

Place nodes in a line, far enough apart that direct links are weak/impossible.

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Place at position A, TX every 5s | 10 min |
| 0x03 (Relay) | Place at position B (middle) | 10 min |
| 0x04 (Relay) | Place at position C | 10 min |
| 0x05 (Edge) | Place at position D, capture logs | 10 min |

**Variation:** Add a second sensor at position A to test multi-hop with load.

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | Position A | 10 min |
| 0x02 (Sensor) | Position A (same location) | 10 min |
| 0x03, 0x04 (Relay) | Positions B, C | 10 min |
| 0x05 (Edge) | Position D, capture logs | 10 min |

**Expected Metrics:**
- Hop count: 2-3
- Latency: 300-600 ms (additive per hop)
- PDR: 80-90%

**Log files:** `logs/multihop_1sensor.log`, `logs/multihop_2sensor.log`

---

## Scenario 4: Distance/Signal Stress

**Goal:** Test performance at range limits.

### 4a: Near range (10m line-of-sight)

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | 10m from Edge, clear LOS | 5 min |
| 0x05 (Edge) | Capture logs, note RSSI | 5 min |

### 4b: Medium range (50m line-of-sight)

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | 50m from Edge, clear LOS | 5 min |
| 0x05 (Edge) | Capture logs, note RSSI | 5 min |

### 4c: Far range (100m+ or obstructed)

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | 100m+ or through walls | 5 min |
| 0x05 (Edge) | Capture logs, note RSSI | 5 min |

**Log files:** `logs/distance_10m.log`, `logs/distance_50m.log`, `logs/distance_100m.log`

**Expected:** RSSI drops, PDR decreases at extreme range.

---

## Scenario 5: Burst Traffic

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

## Scenario 6: Node Failure/Recovery

**Goal:** Test mesh resilience when nodes drop.

| Time | Action |
|------|--------|
| 0:00 | Start all nodes normally |
| 2:00 | Power off Relay 0x03 |
| 4:00 | Power on Relay 0x03 |
| 6:00 | Power off Sensor 0x02 |
| 8:00 | Power on Sensor 0x02 |
| 10:00 | End test |

| Node | Action | Duration |
|------|--------|----------|
| 0x01, 0x02 (Sensor) | Normal TX | 10 min |
| 0x03 (Relay) | Toggle power at 2:00/4:00 | 10 min |
| 0x05 (Edge) | Capture logs with timestamps | 10 min |

**Log file:** `logs/node_failure.log`

**Expected:** Temporary PDR drop, recovery within 30s.

---

## Scenario 7: All Nodes Active (Load Test)

**Goal:** Maximum realistic load on the mesh.

| Node | Action | Duration |
|------|--------|----------|
| 0x01 (Sensor) | TX every 3s | 10 min |
| 0x02 (Sensor) | TX every 3s | 10 min |
| 0x03 (Relay) | Forward only | 10 min |
| 0x04 (Relay) | Forward only | 10 min |
| 0x05 (Edge) | Capture logs | 10 min |

**Log file:** `logs/full_load.log`

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
