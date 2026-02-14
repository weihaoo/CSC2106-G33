/*
 * LoRa Mesh - COMPACT DEBUG (Fits easily on Arduino Uno)
 */

#include <SPI.h>
#include <LoRa.h>
// ============================================
// CHANGE THESE FOR EACH NODE!
// ============================================
#define MY_NODE_ID      2        // Node 1, 2, 3, 4, or 5
#define IS_SINK_NODE    false    // true for sink, false for regular

// ============================================
// CHANGE THIS TO YOUR UNIQUE NETWORK ID!
// ============================================
#define MY_NETWORK_ID   0x67    // Change to 0x12, 0x34, 0x99, anything!
                                 // Same for ALL your nodes!

// ============================================
// Hardware pins (don't change)

// ============================================
#define LORA_SS         10
#define LORA_RST        9
#define LORA_DIO0       2

// ============================================
// Timing
// ============================================
#define BEACON_INTERVAL     8000
#define DATA_INTERVAL       25000
#define ROUTE_STABLE_TIME   30000

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
  float temperature;
} __attribute__((packed));

// ============================================
// State
// ============================================
uint8_t myDistance = 255;
uint8_t bestNeighbor = 255;
int16_t bestNeighborRSSI = -200;
unsigned long routeEstablishedTime = 0;

unsigned long lastBeacon = 0;
unsigned long lastData = 0;

uint16_t beaconsSent = 0;
uint16_t beaconsRx = 0;
uint16_t dataSent = 0;
uint16_t dataRx = 0;
uint16_t dataFwd = 0;
uint16_t foreignIgnored = 0;

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(9600);
  while (!Serial);
  
  Serial.println(F("\n=== LoRa Mesh Node ==="));
  Serial.print(F("ID:")); Serial.print(MY_NODE_ID);
  Serial.print(F(" Net:0x")); Serial.print(MY_NETWORK_ID, HEX);
  Serial.print(F(" Type:"));
  Serial.println(IS_SINK_NODE ? F("SINK") : F("REGULAR"));
  
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(923E6)) {
    Serial.println(F("ERROR: LoRa init failed!"));
    while (1);
  }
  
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setTxPower(17);
  LoRa.setSyncWord(0x12);
  
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
  unsigned long now = millis();
  
  listenForPackets();


  if (now - lastBeacon > BEACON_INTERVAL) {
    Serial.println(F("[DEBUG] Time to send beacon..."));  // ADD THIS
    delay(random(0, 500));
    sendBeacon();
    lastBeacon = now;
  }
  
  if (!IS_SINK_NODE && 
      bestNeighbor != 255 &&
      now - routeEstablishedTime > ROUTE_STABLE_TIME &&
      now - lastData > DATA_INTERVAL) {
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
  Serial.print(F(" Count:"));
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
  pkt.temperature = 20.0 + random(0, 100) / 10.0;
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();
  
  dataSent++;
  
  Serial.print(F("[DATA OUT] Temp:"));
  Serial.print(pkt.temperature, 1);
  Serial.print(F("C -> N"));
  Serial.print(bestNeighbor);
  Serial.print(F(" #"));
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
    Serial.print(F("[IGNORE] Foreign 0x"));
    Serial.print(networkId, HEX);
    Serial.print(F(" ("));
    Serial.print(foreignIgnored);
    Serial.println(F(")"));
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
    
    Serial.print(F("[BEACON IN] N"));
    Serial.print(beacon->fromNode);
    Serial.print(F(" D:"));
    Serial.print(beacon->distance);
    Serial.print(F(" RSSI:"));
    Serial.println(rssi);
    
    if (!IS_SINK_NODE) {
      bool routeChanged = false;
      uint8_t oldDist = myDistance;
      
      if (beacon->distance < myDistance - 1) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        myDistance = beacon->distance + 1;
        routeChanged = true;
      }
      else if (beacon->distance == myDistance - 1 && rssi > bestNeighborRSSI + 10) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        routeChanged = true;
      }
      
      if (routeChanged) {
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
    dataRx++;
    
    Serial.print(F("[DATA IN] N"));
    Serial.print(data->fromNode);
    Serial.print(F(" Temp:"));
    Serial.print(data->temperature, 1);
    Serial.print(F("C Hops:"));
    Serial.println(data->hops);
    
    if (IS_SINK_NODE) {
      Serial.println(F(""));
      Serial.println(F("*** SINK RECEIVED DATA! ***"));
      Serial.print(F("Origin: N"));
      Serial.println(data->fromNode);
      Serial.print(F("Temp: "));
      Serial.print(data->temperature, 1);
      Serial.println(F("C"));
      Serial.print(F("Hops: "));
      Serial.println(data->hops);
      Serial.print(F("Total RX: "));
      Serial.println(dataRx);
      Serial.println(F("***************************\n"));
      return;
    }
    
    if (data->toNode == MY_NODE_ID && bestNeighbor != 255) {
      data->toNode = bestNeighbor;
      data->hops++;
      
      delay(random(50, 150));
      
      LoRa.beginPacket();
      LoRa.write((uint8_t*)data, sizeof(DataPacket));
      LoRa.endPacket();
      
      dataFwd++;
      
      Serial.print(F("[FORWARD] -> N"));
      Serial.print(bestNeighbor);
      Serial.print(F(" #"));
      Serial.println(dataFwd);
    }
  }
}

// ============================================
// STATUS
// ============================================
void printStatus() {
  Serial.println(F("\n--- STATUS ---"));
  Serial.print(F("Dist:")); 
  if (myDistance == 255) Serial.print(F("???"));
  else Serial.print(myDistance);
  Serial.print(F(" NextHop:"));
  if (bestNeighbor == 255) Serial.println(F("None"));
  else { Serial.print(F("N")); Serial.println(bestNeighbor); }
  
  Serial.print(F("BCN TX:")); Serial.print(beaconsSent);
  Serial.print(F(" RX:")); Serial.println(beaconsRx);
  
  if (!IS_SINK_NODE) {
    Serial.print(F("DATA TX:")); Serial.print(dataSent);
    Serial.print(F(" FWD:")); Serial.println(dataFwd);
  } else {
    Serial.print(F("DATA RX:")); Serial.println(dataRx);
  }
  
  Serial.print(F("Foreign:")); Serial.println(foreignIgnored);
  Serial.println(F("--------------\n"));
}