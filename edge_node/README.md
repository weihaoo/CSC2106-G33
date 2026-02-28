# Edge Node Firmware - Person 3

**CSC2106 Group 33 - LoRa Mesh Network**

This firmware implements the Edge Node (sink) that receives mesh packets and forwards them to The Things Network via LoRaWAN.

---

## What This Node Does

1. **Listens for mesh packets** from sensors and relay nodes
2. **Validates packets** (CRC check, deduplication, TTL check)
3. **Sends ACKs** back to senders
4. **Aggregates** sensor readings (up to 7) into BridgeAggV1 format
5. **Sends to TTN** via LoRaWAN when buffer full or 4 minutes elapsed
6. **Broadcasts rank-0 beacons** every 10 seconds
7. **Time-shares one radio** between mesh RX and LoRaWAN TX

---

## Hardware Required

- **LilyGo T-Beam** (SX1262 version, 923 MHz for AS923)
- **LoRa antenna** (attach to SMA connector - **CRITICAL!**)
- **Micro-usb cable** (for power and programming)

---

## Software Setup

### Step 1: Install Arduino IDE

1. Download Arduino IDE 2.x from [arduino.cc](https://www.arduino.cc/en/software)
2. Open **Preferences** → **Additional Board Manager URLs**
3. Add: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
4. Go to **Tools → Board → Boards Manager**
5. Search for **"esp32"** and install **"esp32 by Espressif Systems"**
6. Select **Tools → Board → ESP32 Dev Module**

### Step 2: Install Required Libraries

Open **Sketch → Include Library → Manage Libraries** and install:

- **RadioLib** by Jan Gromeš (for LoRa + LoRaWAN)
- **XPowersLib** by Lewis He (for T-Beam PMU power management)

### Step 3: Get OTAA Credentials from Person 4

Person 4 (TTN setup) will give you:

- **DevEUI** (8 bytes, in **LSB** byte order - reversed from TTN console!)
- **AppKey** (16 bytes, in **MSB** byte order - same as TTN shows)

Example conversion:

```
TTN console shows DevEUI: 70B3D57ED0012345
Firmware needs (LSB):     0x4523010000000000  (reversed!)

TTN console shows AppKey: 0123456789ABCDEF0123456789ABCDEF
Firmware needs (MSB):     {0x01, 0x23, 0x45, ...}  (same order)
```

### Step 4: Edit `config.h`

1. Open `edge_node/config.h`
2. **For Edge 1 (primary):**
   - Set `#define NODE_ID 0x01`
   - Replace `DEV_EUI` with Edge 1's DevEUI (LSB order)
   - Replace `APP_KEY[]` with Edge 1's AppKey (MSB order)

3. **For Edge 2 (standby):**
   - Set `#define NODE_ID 0x06`
   - Replace `DEV_EUI` with Edge 2's DevEUI (LSB order)
   - Replace `APP_KEY[]` with Edge 2's AppKey (MSB order)

### Step 5: Flash the Firmware

1. Connect T-Beam via USB-C
2. Select correct COM port in **Tools → Port**
3. Click **Upload** (or press Ctrl+U)
4. Wait for upload to complete

### Step 6: Monitor Serial Output

1. Open **Tools → Serial Monitor**
2. Set baud rate to **115200**
3. You should see:

```
════════════════════════════════════════════════════
     EDGE NODE - CSC2106 G33 (Person 3)
════════════════════════════════════════════════════
Node ID: 0x01
Rank: 0
════════════════════════════════════════════════════

[INIT] Initializing PMU (AXP2101)...
[OK] PMU initialized — LoRa radio powered via ALDO2
[INIT] Initializing RadioLib for mesh...
[OK] RadioLib mesh config:
     Frequency: 923.0 MHz (AS923)
     SF: 7, BW: 125.0 kHz
[INIT] Starting LoRaWAN OTAA join...
[OK] LoRaWAN OTAA join successful!

════════════════════════════════════════════════════
Edge node ready! Listening for mesh packets...
════════════════════════════════════════════════════
```

---

## What to Look For (Testing)

### Normal Operation

You should see these messages cycling:

```
[BCN] TX | rank=0 | queue=14%         ← Broadcasting beacon every 10s

────────────────────────────────────────────────────
RX_MESH | src=0x03 | seq=12 | hops=2 | rssi=-68     ← Received sensor data
ACK | to=0x02 | seq=12                              ← Sent ACK to relay
AGG | Added to buffer | count=2/7                   ← Added to buffer
────────────────────────────────────────────────────

[FLUSH] Trigger detected, switching to LoRaWAN mode  ← Time to send to TTN
════════════════════════════════════════════════════
FLUSH | records=2 | bytes=31
UPLINK | OK | TTN received
Total uplinks sent: 5
════════════════════════════════════════════════════
[RADIO] Switched back to MESH_LISTEN mode
```

### Error Messages

| Message | Meaning | Fix |
|---------|---------|-----|
| `[ERROR] PMU init failed!` | PMU not responding | Check I2C wiring, board type |
| `[ERROR] RadioLib init failed, code -2` | Radio not powered | PMU didn't enable ALDO2 |
| `[WARN] LoRaWAN join failed, code X` | OTAA join failed | Check credentials, gateway online |
| `[DROP] CRC_FAIL` | Corrupted packet | Normal, radio interference |
| `[DROP] Duplicate` | Already seen this packet | Normal, deduplication working |

---

## 🧪 Integration Testing

### Test 1: Can You Receive Mesh Packets?

**Prerequisites:** Person 1 (sensor) or Person 2 (relay) is transmitting

**Expected:**
```
RX_MESH | src=0x03 | seq=12 | hops=2 | rssi=-68
ACK | to=0x02 | seq=12
AGG | Added to buffer | count=1/7
```

✅ **Pass:** You see RX_MESH and ACK messages  
❌ **Fail:** No messages → check antenna attached, same frequency (923 MHz)

---

### Test 2: Does LoRaWAN Join Work?

**Prerequisites:** Person 4 has set up TTN application and gateway

**Expected:**
```
[INIT] Starting LoRaWAN OTAA join...
[OK] LoRaWAN OTAA join successful!
```

✅ **Pass:** You see "join successful"  
❌ **Fail:** Check credentials (DevEUI LSB vs MSB!), gateway connected to TTN

---

### Test 3: Does Uplink to TTN Work?

**Prerequisites:** Mesh packets received + joined TTN

**Expected after 4 minutes or 7 packets:**
```
[FLUSH] Trigger detected, switching to LoRaWAN mode
FLUSH | records=2 | bytes=31
UPLINK | OK | TTN received
```

Then check **TTN Console → Applications → Live Data** for uplink.

✅ **Pass:** Uplink appears in TTN  
❌ **Fail:** Check gateway coverage, LoRaWAN join status

---

### Test 4: Does Dashboard Show Data?

**Prerequisites:** Person 4 has dashboard running

**Expected:** Dashboard shows temperature/humidity from your node

✅ **Pass:** Data appears with correct `bridge_id` (0x01 or 0x06)  
❌ **Fail:** Check Person 4's decoder.js and dashboard code

---

## 🚨 Common Issues & Solutions

### Issue: "RadioLib init failed, code -2"

**Cause:** PMU didn't power the radio  
**Fix:** 
1. Check that `PMU.begin()` succeeded
2. Verify T-Beam board type (make sure it's SX1262, not SX1276)
3. Try power cycling the board

---

### Issue: LoRaWAN join keeps failing

**Cause:** Wrong credentials or byte order  
**Fix:**
1. Double-check DevEUI is **LSB** (reversed from TTN console)
2. Double-check AppKey is **MSB** (same as TTN console)
3. Verify gateway is online in TTN console
4. Check frequency plan is AS923 Group 1

---

### Issue: Receive mesh packets but no uplinks to TTN

**Cause:** LoRaWAN not joined or flush not triggering  
**Fix:**
1. Check `lorawan_joined` is true (watch serial output)
2. Wait 4 minutes or send 7 packets to trigger flush
3. Check aggregation buffer: `AGG | Added to buffer | count=X/7`

---

### Issue: Uplink sent but TTN doesn't receive it

**Cause:** Gateway not covering the area or wrong frequency  
**Fix:**
1. Check TTN gateway status (must be "Connected")
2. Move closer to gateway
3. Verify edge node is using AS923 (923 MHz)
4. Check TTN Live Data for join accepts (confirms gateway works)

---

## Serial Log Reference

### Log Prefixes

| Prefix | Meaning |
|--------|---------|
| `[INIT]` | Initialization stage |
| `[OK]` | Success |
| `[ERROR]` | Fatal error (stops execution) |
| `[WARN]` | Warning (continues execution) |
| `[BCN]` | Beacon transmission |
| `RX_MESH` | Mesh packet received |
| `ACK` | ACK sent |
| `AGG` | Aggregation buffer action |
| `[FLUSH]` | Uplink flush triggered |
| `UPLINK` | LoRaWAN uplink status |
| `[DROP]` | Packet dropped (with reason) |
| `[RADIO]` | Radio mode switch |

---

## Acceptance Criteria (For Demo)

- [ ] Edge 1 and Edge 2 both boot and join TTN successfully
- [ ] Both nodes broadcast rank-0 beacons every 10s
- [ ] Receive mesh packets from relay/sensors and send ACKs
- [ ] Aggregate readings into BridgeAggV1 format correctly
- [ ] Send uplinks to TTN when buffer full or timeout
- [ ] Person 4's decoder.js decodes your uplinks correctly
- [ ] Dashboard shows data from both edge nodes
- [ ] **Failover test:** Power off Edge 1 → relay switches to Edge 2 → TTN uplinks now come from Edge 2

---

## File Structure

```
edge_node/
├── edge_node.ino        ← Main firmware (this is what you flash)
├── config.h             ← EDIT THIS: Node ID + OTAA credentials
├── mesh_protocol.h      ← Protocol definitions (shared with other nodes)
└── README.md            ← This file
```

---

## Coordination with Team

### Person 1 (Sensor Node)
- They send data packets with 7-byte DHT22 payload
- You receive, ACK, and aggregate their packets

### Person 2 (Relay Node)
- They forward packets from sensors to you
- You receive their forwarded packets (check `IS_FORWARDED` flag)

### Person 4 (TTN + Dashboard)
- They give you OTAA credentials (DevEUI, AppKey)
- They deploy decoder.js that parses your BridgeAggV1 format
- They run dashboard that shows your uplinked data

### Person 5 (Testing)
- They capture your serial logs for PDR analysis
- They verify payload integrity (bytes match sensor → relay → you → TTN)

---

## Help?

**If something doesn't work:**

1. Check serial output for error messages
2. Verify antenna is attached (CRITICAL!)
3. Check credentials in `config.h` (DevEUI byte order!)
4. Ask Person 4 if TTN gateway is online
5. Compare with Person 2's relay code (similar structure)

**Serial output looks good but still issues?**

- Ask Person 5 to check if packets are arriving
- Ask Person 4 to check TTN Live Data
- Verify with Person 2 that relay is routing to you (not direct to sensor)

---

## 🎓 For Beginners: How the Code Works

1. **setup()** runs once:
   - Powers on radio via PMU
   - Configures RadioLib for mesh
   - Joins TTN via OTAA
   - Starts listening for mesh packets

2. **loop()** runs forever:
   - Checks if mesh packet arrived → validate → ACK → add to buffer
   - Checks if beacon is due → send rank-0 beacon
   - Checks if flush needed → switch to LoRaWAN → send uplink → switch back

3. **Key functions:**
   - `receive_mesh_packets()` - validates and processes incoming mesh data
   - `handle_data()` - adds packet to aggregation buffer
   - `send_lorawan_uplink()` - packs BridgeAggV1 and sends to TTN
   - `switch_to_mesh_rx()` - reconfigures radio back to mesh mode

---
