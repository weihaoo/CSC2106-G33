/*
 * LoRa Mesh Network Node - MEMORY OPTIMIZED for Arduino Uno
 * Hardware: Cytron LoRa-RFM Shield + Maker UNO
 */

#include <SPI.h>
#include <LoRa.h>
#include "NodeConfig.h"
#include "MeshPackets.h"
#include "NeighborTable.h"

// ============================================
// GLOBAL STATE (Minimized)
// ============================================
NeighborTable neighborTable;
uint16_t packetIdCounter = 0;
uint8_t myDistanceToSink = INVALID_DISTANCE;

unsigned long lastBeaconTime = 0;
unsigned long lastDataSendTime = 0;
unsigned long lastCleanupTime = 0;

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(9600);
  while (!Serial);
  
  Serial.println(F("\n=== LoRa Mesh Node ==="));
  Serial.print(F("Node ID: ")); Serial.println(MY_NODE_ID);
  Serial.print(F("Type: "));
  Serial.println(MY_NODE_TYPE == NODE_TYPE_SINK ? F("SINK") : F("REGULAR"));
  
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println(F("LoRa FAILED!"));
    while (1);
  }
  
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setSyncWord(SYNC_WORD);
  
  Serial.println(F("LoRa OK!"));
  
  if (MY_NODE_TYPE == NODE_TYPE_SINK) {
    myDistanceToSink = 0;
  }
  
  Serial.println(F("Starting...\n"));
  sendBeacon();
  lastBeaconTime = millis();
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  unsigned long now = millis();
  
  receivePackets();
  
  if (now - lastBeaconTime >= BEACON_INTERVAL) {
    sendBeacon();
    lastBeaconTime = now;
  }
  
  if (now - lastCleanupTime >= NEIGHBOR_TIMEOUT / 2) {
    neighborTable.cleanupOldNeighbors();
    updateMyDistance();
    lastCleanupTime = now;
  }
  
  if (MY_NODE_TYPE == NODE_TYPE_REGULAR && 
      now - lastDataSendTime >= DATA_SEND_INTERVAL) {
    sendSensorData();
    lastDataSendTime = now;
  }
  
  delay(10);
}

// ============================================
// BEACON FUNCTIONS
// ============================================
void sendBeacon() {
  BeaconPacket beacon;
  
  beacon.header.packetType = PKT_BEACON;
  beacon.header.srcId = MY_NODE_ID;
  beacon.header.destId = BROADCAST_ADDR;
  beacon.header.hopCount = 0;
  beacon.header.packetId = packetIdCounter++;
  beacon.nodeType = MY_NODE_TYPE;
  beacon.distanceToSink = myDistanceToSink;
  beacon.rssi = 0;
  beacon.timestamp = millis();
  beacon.header.checksum = calculateChecksum((uint8_t*)&beacon, sizeof(beacon));
  
  if (!channelIsClear()) {
    delay(random(10, CSMA_MAX_BACKOFF));
  }
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&beacon, sizeof(beacon));
  LoRa.endPacket();
  
  Serial.print(F("BCN> D:"));
  Serial.print(myDistanceToSink);
  Serial.print(F(" N:"));
  Serial.println(neighborTable.getNeighborCount());
}

void handleBeacon(BeaconPacket* beacon, int16_t rssi) {
  Serial.print(F("BCN< N"));
  Serial.print(beacon->header.srcId);
  Serial.print(F(" D:"));
  Serial.print(beacon->distanceToSink);
  Serial.print(F(" R:"));
  Serial.println(rssi);
  
  neighborTable.updateNeighbor(
    beacon->header.srcId,
    beacon->nodeType,
    beacon->distanceToSink,
    rssi
  );
  
  updateMyDistance();
}

// ============================================
// DATA FUNCTIONS
// ============================================
void sendSensorData() {
  uint8_t nextHop = neighborTable.getBestNextHop();
  
  if (nextHop == 0xFF) {
    Serial.println(F("No route!"));
    return;
  }
  
  float temp = readTemperature();
  
  DataPacket data;
  data.header.packetType = PKT_DATA;
  data.header.srcId = MY_NODE_ID;
  data.header.destId = nextHop;
  data.header.hopCount = 0;
  data.header.packetId = packetIdCounter++;
  data.sinkId = 0xFF;
  data.sensorValue = temp;
  data.timestamp = millis();
  data.batteryLevel = 100;
  data.header.checksum = calculateChecksum((uint8_t*)&data, sizeof(data));
  
  if (!channelIsClear()) {
    delay(random(10, CSMA_MAX_BACKOFF));
  }
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&data, sizeof(data));
  LoRa.endPacket();
  
  Serial.print(F("DATA> "));
  Serial.print(temp, 1);
  Serial.print(F("C->N"));
  Serial.println(nextHop);
}

void handleDataPacket(DataPacket* data) {
  Serial.print(F("DATA< N"));
  Serial.print(data->header.srcId);
  Serial.print(F(" "));
  Serial.print(data->sensorValue, 1);
  Serial.print(F("C H:"));
  Serial.println(data->header.hopCount);
  
  if (MY_NODE_TYPE == NODE_TYPE_SINK) {
    processSinkData(data);
    return;
  }
  
  if (data->header.destId != MY_NODE_ID && 
      data->header.destId != BROADCAST_ADDR) {
    return;
  }
  
  if (data->header.hopCount >= MAX_HOPS) {
    Serial.println(F("Max hops!"));
    return;
  }
  
  uint8_t nextHop = neighborTable.getBestNextHop();
  if (nextHop == 0xFF) {
    Serial.println(F("No fwd route!"));
    return;
  }
  
  data->header.destId = nextHop;
  data->header.hopCount++;
  data->header.checksum = calculateChecksum((uint8_t*)data, sizeof(DataPacket));
  
  delay(random(10, 50));
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)data, sizeof(DataPacket));
  LoRa.endPacket();
  
  Serial.print(F("FWD->N"));
  Serial.println(nextHop);
}

void processSinkData(DataPacket* data) {
  Serial.println(F("=== SINK RX ==="));
  Serial.print(F("From: N"));
  Serial.println(data->header.srcId);
  Serial.print(F("Temp: "));
  Serial.print(data->sensorValue, 1);
  Serial.println(F("C"));
  Serial.print(F("Hops: "));
  Serial.println(data->header.hopCount);
  Serial.println(F("==============="));
}

// ============================================
// PACKET RECEPTION
// ============================================
void receivePackets() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;
  
  // CRITICAL: Reduced from 256 to 32 bytes
  uint8_t buffer[32];
  int len = 0;
  while (LoRa.available() && len < 32) {
    buffer[len++] = LoRa.read();
  }
  
  if (len < sizeof(PacketHeader)) return;
  
  PacketHeader* header = (PacketHeader*)buffer;
  int16_t rssi = LoRa.packetRssi();
  
  if (!verifyChecksum(buffer, len)) {
    Serial.println(F("CHK FAIL"));
    return;
  }
  
  if (header->srcId == MY_NODE_ID) return;
  
  switch (header->packetType) {
    case PKT_BEACON:
      if (len == sizeof(BeaconPacket)) {
        handleBeacon((BeaconPacket*)buffer, rssi);
      }
      break;
      
    case PKT_DATA:
      if (len == sizeof(DataPacket)) {
        handleDataPacket((DataPacket*)buffer);
      }
      break;
  }
}

// ============================================
// ROUTING
// ============================================
void updateMyDistance() {
  if (MY_NODE_TYPE == NODE_TYPE_SINK) {
    myDistanceToSink = 0;
    return;
  }
  
  uint8_t newDistance = neighborTable.getMyDistanceToSink();
  
  if (newDistance != myDistanceToSink) {
    myDistanceToSink = newDistance;
    Serial.print(F("Dist: "));
    Serial.println(newDistance);
    neighborTable.printTable();
  }
}

bool channelIsClear() {
  return (LoRa.rssi() < RSSI_THRESHOLD);
}

// ============================================
// SENSOR
// ============================================
float readTemperature() {
  #if USE_SIMULATED_SENSOR
    return 20.0 + random(0, 100) / 10.0;
  #else
    int reading = analogRead(TEMP_SENSOR_PIN);
    float voltage = reading * (5.0 / 1024.0);
    return (voltage - 0.5) * 100.0;
  #endif
}