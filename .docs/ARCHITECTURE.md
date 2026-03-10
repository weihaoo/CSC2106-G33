# CSC2106 Group 33 - LoRa Mesh Network Architecture

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                                    CLOUD LAYER                                      │
│  ┌───────────────────────────────────────────────────────────────────────────────┐  │
│  │                     The Things Network (TTN)                                  │  │
│  │                     AS923 Singapore Cluster                                   │  │
│  │   • Receives LoRaWAN uplinks (BridgeAggV1 format)                            │  │
│  │   • Decodes via decoder.js                                                    │  │
│  │   • Publishes to MQTT                                                         │  │
│  └───────────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────────┘
                                         │
                                         │ MQTT
                                         ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                               APPLICATION LAYER                                      │
│  ┌───────────────────────────────────────────────────────────────────────────────┐  │
│  │                    Raspberry Pi Pico W Dashboard                              │  │
│  │                    (MicroPython HTTP Server)                                  │  │
│  └───────────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────────┘
                                         ▲
                                         │ LoRaWAN (AES-128 encrypted)
                                         │
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                               GATEWAY LAYER                                          │
│  ┌───────────────────────────────────────────────────────────────────────────────┐  │
│  │                      RAK WisGate LoRaWAN Gateway                              │  │
│  │                      (Lab-provided, AS923)                                    │  │
│  └───────────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────────┘
                                         ▲
                                         │ LoRaWAN Uplink
                                         │
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                          MESH NETWORK (Custom LoRa Protocol)                        │
│                                                                                     │
│  ╔═══════════════════════════════════════════════════════════════════════════════╗  │
│  ║                    DAG ARCHITECTURE (Directed Acyclic Graph)                   ║  │
│  ║                                                                                 ║  │
│  ║   ┌─────────────┐                                                             ║  │
│  ║   │  EDGE-01    │◄──────────────────┐                                         ║  │
│  ║   │  (0x01)     │                   │                                         ║  │
│  ║   │  Rank: 0    │                   │  ┌─────────────┐                        ║  │
│  ║   │  PRIMARY    │                   └──┤  EDGE-06    │                        ║  │
│  ║   └──────┬──────┘                      │  (0x06)     │                        ║  │
│  ║          │                             │  Rank: 0    │                        ║  │
│  ║          │  ◄──┐   WEIGHTED RANDOM    │  SECONDARY  │                        ║  │
│  ║          │     │   SELECTION          └─────────────┘                        ║  │
│  ║          │     │                                                             ║  │
│  ║          │     │   • Score-based probability                                 ║  │
│  ║          │     │   • Load balancing across edges                             ║  │
│  ║          │     │   • Per-packet parent selection                             ║  │
│  ║          │     │   • Instant failover (no timeout delay)                     ║  │
│  ║          │     │                                                             ║  │
│  ║          │     └──┐                                                          ║  │
│  ║          │        │                                                          ║  │
│  ║   ┌──────┴────────┴──────┐                                                   ║  │
│  ║   │                     │                                                   ║  │
│  ║   │    RELAY-02         │                                                   ║  │
│  ║   │    (0x02)           │                                                   ║  │
│  ║   │    Rank: 1          │                                                   ║  │
│  ║   │                     │                                                   ║  │
│  ║   │  PARENT SET:        │                                                   ║  │
│  ║   │  ┌───────────────┐  │                                                   ║  │
│  ║   │  │ EDGE-01 (92)  │  │  ◄── Active parents (score ≥ 50)                 ║  │
│  ║   │  │ EDGE-06 (88)  │  │                                                   ║  │
│  ║   │  └───────────────┘  │                                                   ║  │
│  ║   │                     │                                                   ║  │
│  ║   │  Weighted Random:   │                                                   ║  │
│  ║   │  51% → EDGE-01      │                                                   ║  │
│  ║   │  49% → EDGE-06      │                                                   ║  │
│  ║   └──────────┬──────────┘                                                   ║  │
│  ║              │                                                               ║  │
│  ║   ┌──────────┴──────────┐                                                   ║  │
│  ║   │                     │                                                   ║  │
│  ║   │  PAYLOAD-AGNOSTIC   │                                                   ║  │
│  ║   │  FORWARDING         │                                                   ║  │
│  ║   │                     │                                                   ║  │
│  ║   └──────────┬──────────┘                                                   ║  │
│  ╚══════════════╪═══════════════════════════════════════════════════════════════╝  │
│                 │                                                                   │
│        ┌────────┴────────┐                                                        │
│        │                 │                                                        │
│        ▼                 ▼                                                        │
│  ┌─────────────┐   ┌─────────────┐                                               │
│  │ SENSOR-03   │   │ SENSOR-04   │                                               │
│  │ (0x03)      │   │ (0x04)      │                                               │
│  │ Rank: 2     │   │ Rank: 2     │                                               │
│  │             │   │             │                                               │
│  │ DHT22       │   │ DHT22       │                                               │
│  │ TX: 30s     │   │ TX: 30s     │                                               │
│  └─────────────┘   └─────────────┘                                               │
│                                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

## Key Architecture Changes

### 1. DAG (Directed Acyclic Graph)

**Before:** Single parent with failover (Tree)
- Relay had ONE active parent at a time
- 35-second timeout before switching
- Hot standby (Edge 2 only used on Edge 1 failure)

**After:** Multiple active parents (DAG)
- Relay maintains SET of active parents (score ≥ 50)
- Weighted random selection per packet
- Both edges actively receive packets simultaneously
- No failover delay - instant load balancing

### 2. Parent Selection Algorithm

```
Score Calculation (per parent):
┌────────────────────────────────────────┐
│ score = (60 × rank_score / 100)       │
│       + (25 × rssi_score / 100)       │
│       + (15 × queue_score / 100)      │
│                                       │
│ If queue ≥ 80%: score = score / 2    │
└────────────────────────────────────────┘

Active Parent Threshold:
┌────────────────────────────────────────┐
│ score ≥ 50 → Added to active set      │
│ score < 50 → Candidate only           │
└────────────────────────────────────────┘

Weighted Random Selection:
┌────────────────────────────────────────┐
│ parent_probability =                   │
│   parent_score / total_active_scores   │
└────────────────────────────────────────┘
```

### 3. Clear Logging Format

Every node now outputs structured, human-readable logs:

```
[TIMESTAMP] [NODE-ID] [LEVEL] Message
     │           │       │        │
     │           │       │        └── Human-readable description
     │           │       └── INFO, OK, WARN, ERROR, TX, RX, ACK, FWD, BEACON, etc.
     │           └── SENSOR-03, RELAY-02, EDGE-01, EDGE-06
     └── [000.0s] since boot
```

**Example Log Output:**
```
════════════════════════════════════════════════════════════════════════
  RELAY-02 | Relay Node | CSC2106 G33 LoRa Mesh
════════════════════════════════════════════════════════════════════════

[000.5s] [RELAY-02] [OK    ] Radio ready: 923.0 MHz, SF7, BW125 kHz
[010.0s] [RELAY-02] [BEACON] Received beacon from EDGE-01
                                    Signal: -58 dBm | Rank: 0 | Queue: 14%
[010.0s] [RELAY-02] [PARENT] Parent set updated: 1 active parents
                                    [1] EDGE-01 (score: 92)
[015.0s] [RELAY-02] [BEACON] Received beacon from EDGE-06
                                    Signal: -62 dBm | Rank: 0 | Queue: 0%
[015.0s] [RELAY-02] [PARENT] Parent set updated: 2 active parents
                                    [1] EDGE-01 (score: 92)
                                    [2] EDGE-06 (score: 88)
┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
[030.3s] [RELAY-02] [FWD   ] Forwarding packet #12 to EDGE-01
                                    Source: SENSOR-03 | TTL: 10→9 | Payload: 7 bytes
                                    DAG selection: 2 active parents, chose EDGE-01 (weighted random)
┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
```

## Packet Flow

### Normal Operation (Both Edges Active)

```
Packet #1: SENSOR-03 → RELAY-02 → EDGE-01 → TTN
Packet #2: SENSOR-04 → RELAY-02 → EDGE-06 → TTN
Packet #3: SENSOR-03 → RELAY-02 → EDGE-06 → TTN  (weighted random)
Packet #4: SENSOR-04 → RELAY-02 → EDGE-01 → TTN  (weighted random)
```

Distribution is proportional to parent scores:
- If EDGE-01 score = 92, EDGE-06 score = 88
- EDGE-01 receives ~51% of packets
- EDGE-06 receives ~49% of packets

### Failover Scenario

```
Scenario: EDGE-01 goes offline

[030.0s] RELAY stops hearing EDGE-01 beacons
[065.0s] (35s timeout) EDGE-01 removed from active parent set
[065.0s] Parent set: 1 active parent (EDGE-06 only)

Result:
Packet #N: SENSOR-03 → RELAY-02 → EDGE-06 → TTN
Packet #N+1: SENSOR-04 → RELAY-02 → EDGE-06 → TTN

Zero downtime - instant failover to remaining active parent
```

## Protocol Stack

```
┌─────────────────────────────────────────────────────────────────┐
│                         APPLICATION                              │
│                    Dashboard, MQTT Consumer                      │
├─────────────────────────────────────────────────────────────────┤
│                          TRANSPORT                               │
│    LoRaWAN (AES-128)    │    Custom Mesh (CRC16)                │
│    Edge ↔ TTN          │    Sensor ↔ Relay ↔ Edge               │
├─────────────────────────────────────────────────────────────────┤
│                           NETWORK                                │
│   BridgeAggV1 Format    │   MeshHeader (10B) + Payload           │
│   (aggregated uplink)   │   DAG-based multi-parent routing       │
├─────────────────────────────────────────────────────────────────┤
│                           PHYSICAL                               │
│                    LoRa SX1262 @ 923 MHz                         │
│                    SF7, BW 125kHz, CR 4/5                        │
└─────────────────────────────────────────────────────────────────┘
```

## Architecture Classification

| Aspect | Classification | Notes |
|--------|----------------|-------|
| **Topology** | DAG (Directed Acyclic Graph) | Multiple active parents, not single-parent tree |
| **Routing** | Proactive + Weighted | Beacon-based discovery + score-weighted selection |
| **Control** | Decentralized | Each node makes local routing decisions |
| **Data Flow** | Convergecast | All data flows toward edge sinks |
| **Redundancy** | Active-Active | Both edges actively receive packets |
| **Load Balancing** | Weighted Random | Proportional to parent quality scores |

## Files Structure

```
CSC2106-G33/
├── logging.h                 # New: Clear logging module (all nodes)
├── sensor_node/
│   ├── sensor_node.ino       # Modified: Clear logging
│   ├── logging.h             # New: Shared logging module
│   └── mesh_protocol.h       # Modified: DAG constants added
├── relay_node/
│   ├── relay_node.ino        # Modified: Full DAG + clear logging
│   ├── logging.h             # New: Shared logging module
│   └── mesh_protocol.h       # Modified: DAG constants added
├── edge_node/
│   ├── edge_node.ino         # Modified: Clear logging
│   ├── config.h              # Unchanged
│   ├── logging.h             # New: Shared logging module
│   └── mesh_protocol.h       # Modified: DAG constants added
└── .docs/
    └── ARCHITECTURE.md       # This file
```

## Testing the DAG

**To verify DAG is working:**
1. Power on both EDGE-01 and EDGE-06
2. Watch RELAY-02 serial output for parent set updates
3. You should see "Parent set updated: 2 active parents"
4. Check that packets are forwarded to BOTH edges (not just one)
5. Distribution should be roughly proportional to scores

**Example verification:**
```
Forward 100 packets through relay
Count packets received by each edge:
- EDGE-01: ~51 packets (if score 92)
- EDGE-06: ~49 packets (if score 88)
```
