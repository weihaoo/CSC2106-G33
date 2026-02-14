#include <SPI.h>
#include <LoRa.h>

// ============================================
// CONFIGURATION
// ============================================
#define MY_NODE_ID 2        // CHANGE: 1, 2, 45, 89
#define IS_SINK_NODE false   // true for sink, false for others
#define MY_NETWORK_ID 0x67  // Same for ALL your nodes

// ============================================
// Hardware
// ============================================
#define LORA_SS 10
#define LORA_RST 9
#define LORA_DIO0 2

// ============================================
// Timing
// ============================================
#define BEACON_INTERVAL 8000
#define SINK_BEACON_INTERVAL 4000
#define DATA_INTERVAL 5000
#define ROUTE_STABLE_TIME 3000
#define ROUTE_TIMEOUT 30000

// ============================================
// RSSI Thresholds
// ============================================
#define RSSI_MIN_THRESHOLD -140  // Allow weaker tunnel links during route discovery
#define BROADCAST_ADDR 0xFF
#define MAX_DATA_HOPS 8
#define DEDUP_CACHE_SIZE 20
#define DEDUP_ENTRY_TTL_MS 120000UL
#define TX_POWER_SINK_DBM 8
#define TX_POWER_REGULAR_DBM 14
#define ENABLE_LORA_CRC false

// ============================================
// Packets
// ============================================
struct BeaconPacket {
  uint8_t networkId;
  uint8_t type;
  uint8_t fromNode;
  uint8_t distance;
} __attribute__((packed));

struct DataPacket {
  uint8_t networkId;
  uint8_t type;
  uint8_t fromNode;
  uint8_t toNode;
  uint8_t hops;
  uint8_t originNode;    // CHANGED: Store origin node ID
  uint16_t packetCount;  // CHANGED: Packet counter for this node
} __attribute__((packed));

// ============================================
// State
// ============================================
uint8_t myDistance = 255;
uint8_t bestNeighbor = 255;
int16_t bestNeighborRSSI = -200;
unsigned long routeEstablishedTime = 0;
unsigned long lastParentBeaconTime = 0;

unsigned long lastBeacon = 0;
unsigned long lastData = 0;

uint16_t beaconsSent = 0;
uint16_t beaconsRx = 0;
uint16_t dataSent = 0;
uint16_t dataRx = 0;
uint16_t dataFwd = 0;
uint16_t foreignIgnored = 0;
uint16_t weakSignalIgnored = 0;
uint16_t duplicateDropped = 0;
uint16_t ttlDropped = 0;
uint16_t loopDropped = 0;

struct SeenDataPacket {
  uint8_t originNode;
  uint16_t packetCount;
  unsigned long seenAt;
};

SeenDataPacket seenDataCache[DEDUP_CACHE_SIZE];
uint8_t seenDataCacheIndex = 0;

bool isDuplicateDataPacket(uint8_t originNode, uint16_t packetCount, unsigned long now);
void rememberDataPacket(uint8_t originNode, uint16_t packetCount, unsigned long now);

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(9600);
  while (!Serial)
    ;

  Serial.println(F("\n=== LoRa Mesh Node ==="));
  Serial.print(F("ID:"));
  Serial.print(MY_NODE_ID);
  Serial.print(F(" Net:0x"));
  Serial.print(MY_NETWORK_ID, HEX);
  Serial.print(F(" Type:"));
  Serial.println(IS_SINK_NODE ? F("SINK") : F("REGULAR"));

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(923E6)) {
    Serial.println(F("ERROR: LoRa init failed!"));
    while (1)
      ;
  }

  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setSyncWord(0x12);
  if (ENABLE_LORA_CRC) {
    LoRa.enableCrc();
    Serial.println(F("Payload CRC: ON"));
  } else {
    LoRa.disableCrc();
    Serial.println(F("Payload CRC: OFF (compat mode)"));
  }

  if (IS_SINK_NODE) {
    LoRa.setTxPower(TX_POWER_SINK_DBM);
    Serial.print(F("TX Power: "));
    Serial.print(TX_POWER_SINK_DBM);
    Serial.println(F(" dBm (SINK)"));
  } else {
    LoRa.setTxPower(TX_POWER_REGULAR_DBM);
    Serial.print(F("TX Power: "));
    Serial.print(TX_POWER_REGULAR_DBM);
    Serial.println(F(" dBm (REGULAR)"));
  }

  Serial.println(F("LoRa OK!"));

  if (IS_SINK_NODE) {
    myDistance = 0;
    Serial.println(F("** I AM SINK **\n"));
  } else {
    Serial.println(F("** Looking for sink...\n"));
  }

  randomSeed(analogRead(0));
}

// ============================================
// LOOP
// ============================================
void loop() {
  listenForPackets();
  unsigned long now = millis();
  unsigned long beaconInterval = IS_SINK_NODE ? SINK_BEACON_INTERVAL : BEACON_INTERVAL;

  if (!IS_SINK_NODE && bestNeighbor != 255 && lastParentBeaconTime != 0 && (now - lastParentBeaconTime > ROUTE_TIMEOUT)) {
    Serial.print(F("[ROUTE LOST] Parent N"));
    Serial.print(bestNeighbor);
    Serial.println(F(" timed out, resetting route"));
    bestNeighbor = 255;
    myDistance = 255;
    bestNeighborRSSI = -200;
    routeEstablishedTime = 0;
    lastParentBeaconTime = 0;
  }

  if (now - lastBeacon > beaconInterval) {
    delay(random(0, 500));
    sendBeacon();
    lastBeacon = now;
  }

  if (!IS_SINK_NODE && bestNeighbor != 255 && now - routeEstablishedTime > ROUTE_STABLE_TIME && now - lastData > DATA_INTERVAL) {
    delay(random(100, 300));
    sendData();
    lastData = now;
  }

  static unsigned long lastStatus = 0;
  if (now - lastStatus > 30000) {
    printStatus();
    lastStatus = now;
  }

  delay(10);
}

// ============================================
// SEND BEACON
// ============================================
void sendBeacon() {
  BeaconPacket pkt;
  pkt.networkId = MY_NETWORK_ID;
  pkt.type = 1;
  pkt.fromNode = MY_NODE_ID;
  pkt.distance = myDistance;

  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();

  beaconsSent++;

  Serial.print(F("[BEACON OUT] Dist:"));
  if (myDistance == 255) {
    Serial.print(F("???"));
  } else {
    Serial.print(myDistance);
  }
  Serial.print(F(" #"));
  Serial.println(beaconsSent);
}

// ============================================
// SEND DATA
// ============================================
void sendData() {
  if (bestNeighbor == 255) {
    Serial.println(F("[WARN] No route!"));
    return;
  }

  DataPacket pkt;
  pkt.networkId = MY_NETWORK_ID;
  pkt.type = 2;
  pkt.fromNode = MY_NODE_ID;
  pkt.toNode = bestNeighbor;
  pkt.hops = 0;
  pkt.originNode = MY_NODE_ID;     // CHANGED: Send my node ID
  pkt.packetCount = dataSent + 1;  // CHANGED: Packet number
  rememberDataPacket(pkt.originNode, pkt.packetCount, millis());

  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();

  dataSent++;

  Serial.print(F("[DATA OUT] N"));
  Serial.print(MY_NODE_ID);
  Serial.print(F(" Pkt#"));
  Serial.print(pkt.packetCount);
  Serial.print(F(" ->N"));
  Serial.print(bestNeighbor);
  Serial.print(F(" Hops:0 Total:"));
  Serial.println(dataSent);
}

// ============================================
// LISTEN
// ============================================
void listenForPackets() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  uint8_t buffer[32];
  int len = 0;

  while (LoRa.available() && len < 32) {
    buffer[len++] = LoRa.read();
  }

  if (len < 4) return;

  uint8_t networkId = buffer[0];

  if (networkId != MY_NETWORK_ID) {
    foreignIgnored++;
    return;
  }

  uint8_t type = buffer[1];
  uint8_t fromNode = buffer[2];

  if (fromNode == MY_NODE_ID) return;

  int16_t rssi = LoRa.packetRssi();

  // BEACON
  if (type == 1 && len == sizeof(BeaconPacket)) {
    BeaconPacket* beacon = (BeaconPacket*)buffer;
    beaconsRx++;

    // IGNORE WEAK SIGNALS!
    if (rssi < RSSI_MIN_THRESHOLD) {
      weakSignalIgnored++;
      Serial.print(F("[IGNORE WEAK] N"));
      Serial.print(beacon->fromNode);
      Serial.print(F(" RSSI:"));
      Serial.print(rssi);
      Serial.print(F(" (threshold:"));
      Serial.print(RSSI_MIN_THRESHOLD);
      Serial.print(F(") Total:"));
      Serial.println(weakSignalIgnored);
      return;
    }

    Serial.print(F("[BEACON IN] N"));
    Serial.print(beacon->fromNode);
    Serial.print(F(" D:"));
    Serial.print(beacon->distance);
    Serial.print(F(" RSSI:"));
    Serial.println(rssi);

    if (!IS_SINK_NODE) {
      if (beacon->distance == 255) {
        if (beacon->fromNode == bestNeighbor && myDistance != 255) {
          Serial.print(F("[PARENT LOST ROUTE] N"));
          Serial.println(bestNeighbor);
          bestNeighbor = 255;
          myDistance = 255;
          bestNeighborRSSI = -200;
          routeEstablishedTime = 0;
          lastParentBeaconTime = 0;
        }
        return;
      }

      bool routeChanged = false;
      uint8_t oldDist = myDistance;
      uint8_t candidateDist = beacon->distance + 1;

      if (myDistance == 255 || candidateDist < myDistance) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        myDistance = candidateDist;
        routeChanged = true;
      } else if (candidateDist == myDistance && rssi > bestNeighborRSSI + 6) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        routeChanged = true;
      } else if (beacon->fromNode == bestNeighbor) {
        // Keep parent freshness and track drift in parent's advertised distance.
        lastParentBeaconTime = millis();
        bestNeighborRSSI = rssi;
        if (myDistance != candidateDist) {
          myDistance = candidateDist;
          routeChanged = true;
        }
      }

      if (routeChanged) {
        lastParentBeaconTime = millis();
        routeEstablishedTime = millis();
        Serial.print(F("** ROUTE UPDATE: "));
        if (oldDist == 255) Serial.print(F("???"));
        else Serial.print(oldDist);
        Serial.print(F(" -> "));
        Serial.print(myDistance);
        Serial.print(F(" via N"));
        Serial.println(bestNeighbor);
      }
    }
  }

  // DATA
  else if (type == 2 && len == sizeof(DataPacket)) {
    DataPacket* data = (DataPacket*)buffer;
    unsigned long now = millis();

    if (data->hops >= MAX_DATA_HOPS) {
      ttlDropped++;
      Serial.print(F("[DROP TTL] Origin:N"));
      Serial.print(data->originNode);
      Serial.print(F(" Pkt#"));
      Serial.print(data->packetCount);
      Serial.print(F(" Hops:"));
      Serial.print(data->hops);
      Serial.print(F(" (max:"));
      Serial.print(MAX_DATA_HOPS);
      Serial.println(F(")"));
      return;
    }

    if (isDuplicateDataPacket(data->originNode, data->packetCount, now)) {
      duplicateDropped++;
      Serial.print(F("[DROP DUP] Origin:N"));
      Serial.print(data->originNode);
      Serial.print(F(" Pkt#"));
      Serial.print(data->packetCount);
      Serial.print(F(" From:N"));
      Serial.print(data->fromNode);
      Serial.print(F(" Hops:"));
      Serial.println(data->hops);
      return;
    }

    rememberDataPacket(data->originNode, data->packetCount, now);
    dataRx++;

    Serial.print(F("[DATA IN] Origin:N"));
    Serial.print(data->originNode);
    Serial.print(F(" Pkt#"));
    Serial.print(data->packetCount);
    Serial.print(F(" From:N"));
    Serial.print(data->fromNode);
    Serial.print(F(" To:N"));
    Serial.print(data->toNode);
    Serial.print(F(" Hops:"));
    Serial.println(data->hops);

    // SINK RECEIVED
    if (IS_SINK_NODE) {
      Serial.println(F(""));
      Serial.println(F("╔═══════════════════════════════╗"));
      Serial.println(F("║ 🎯 SINK RECEIVED DATA! 🎯    ║"));
      Serial.println(F("╠═══════════════════════════════╣"));
      Serial.print(F("║ Origin Node: "));
      Serial.print(data->originNode);
      Serial.println(F("                 ║"));
      Serial.print(F("║ Packet Number: "));
      Serial.print(data->packetCount);
      Serial.println(F("             ║"));
      Serial.print(F("║ Hops Traveled: "));
      Serial.print(data->hops);
      Serial.println(F("                ║"));
      Serial.print(F("║ Total Received: "));
      Serial.print(dataRx);
      Serial.println(F("             ║"));
      Serial.println(F("╚═══════════════════════════════╝\n"));
      return;
    }

    // FORWARD DATA - only if packet is addressed to me or broadcast
    bool shouldForward = false;

    if (data->toNode == MY_NODE_ID || data->toNode == BROADCAST_ADDR) {
      shouldForward = true;  // Explicitly for me or broadcast
    }

    if (shouldForward) {
      if (bestNeighbor == 255) {
        Serial.println(F("[ERROR] Can't forward - no next hop!"));
        return;
      }
      if (bestNeighbor == data->fromNode) {
        loopDropped++;
        Serial.print(F("[DROP LOOP] Origin:N"));
        Serial.print(data->originNode);
        Serial.print(F(" Pkt#"));
        Serial.print(data->packetCount);
        Serial.print(F(" Next hop equals sender N"));
        Serial.println(bestNeighbor);
        return;
      }

      // UPDATE PACKET
      data->fromNode = MY_NODE_ID;  // I'm now the sender
      data->toNode = bestNeighbor;  // Send to my best neighbor
      data->hops++;                 // Increment hop count

      Serial.print(F("[FORWARDING] Origin:N"));
      Serial.print(data->originNode);
      Serial.print(F(" Pkt#"));
      Serial.print(data->packetCount);
      Serial.print(F(" ->N"));
      Serial.print(bestNeighbor);
      Serial.print(F(" (Hops:"));
      Serial.print(data->hops);
      Serial.println(F(")"));

      delay(random(50, 150));

      LoRa.beginPacket();
      LoRa.write((uint8_t*)data, sizeof(DataPacket));
      LoRa.endPacket();

      dataFwd++;

      Serial.print(F("[FWD SUCCESS] Total forwarded: "));
      Serial.println(dataFwd);
    } else {
      Serial.print(F("[IGNORE] Not for me (for N"));
      Serial.print(data->toNode);
      Serial.println(F(")"));
    }
  }
}

bool isDuplicateDataPacket(uint8_t originNode, uint16_t packetCount, unsigned long now) {
  for (uint8_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
    if (seenDataCache[i].originNode != originNode) continue;
    if (seenDataCache[i].packetCount != packetCount) continue;

    if ((now - seenDataCache[i].seenAt) <= DEDUP_ENTRY_TTL_MS) {
      return true;
    }
  }

  return false;
}

void rememberDataPacket(uint8_t originNode, uint16_t packetCount, unsigned long now) {
  seenDataCache[seenDataCacheIndex].originNode = originNode;
  seenDataCache[seenDataCacheIndex].packetCount = packetCount;
  seenDataCache[seenDataCacheIndex].seenAt = now;
  seenDataCacheIndex = (seenDataCacheIndex + 1) % DEDUP_CACHE_SIZE;
}

// ============================================
// STATUS
// ============================================
void printStatus() {
  Serial.println(F("\n╔═══════════════════════════════╗"));
  Serial.println(F("║      STATUS REPORT            ║"));
  Serial.println(F("╠═══════════════════════════════╣"));
  Serial.print(F("║ My ID: N"));
  Serial.print(MY_NODE_ID);
  Serial.print(F(" | Dist: "));
  if (myDistance == 255) Serial.print(F("???"));
  else Serial.print(myDistance);
  Serial.println(F("        ║"));

  Serial.print(F("║ Next Hop: "));
  if (bestNeighbor == 255) Serial.print(F("None"));
  else {
    Serial.print(F("N"));
    Serial.print(bestNeighbor);
  }
  Serial.println(F("              ║"));

  Serial.println(F("╟───────────────────────────────╢"));
  Serial.print(F("║ Beacons TX: "));
  Serial.print(beaconsSent);
  Serial.print(F(" | RX: "));
  Serial.print(beaconsRx);
  Serial.println(F("      ║"));

  if (!IS_SINK_NODE) {
    Serial.print(F("║ Data TX: "));
    Serial.print(dataSent);
    Serial.print(F(" | Forwarded: "));
    Serial.print(dataFwd);
    Serial.println(F("  ║"));
  } else {
    Serial.print(F("║ Data Received: "));
    Serial.print(dataRx);
    Serial.println(F("            ║"));
  }

  Serial.println(F("╟───────────────────────────────╢"));
  Serial.print(F("║ Weak Ignored: "));
  Serial.print(weakSignalIgnored);
  Serial.println(F("             ║"));
  Serial.print(F("║ Foreign Ignored: "));
  Serial.print(foreignIgnored);
  Serial.println(F("          ║"));
  Serial.print(F("[DROP STATS] Dup:"));
  Serial.print(duplicateDropped);
  Serial.print(F(" TTL:"));
  Serial.print(ttlDropped);
  Serial.print(F(" Loop:"));
  Serial.println(loopDropped);
  Serial.println(F("╚═══════════════════════════════╝\n"));
}
