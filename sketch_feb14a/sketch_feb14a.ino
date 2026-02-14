/*
 * SIMPLE LoRa Mesh Network - WITH NETWORK ID FILTER
 * Only accepts packets from YOUR network!
 * 
 * IMPORTANT: 
 * 1. Change MY_NODE_ID for each Arduino (1, 2, 3, 4, 5)
 * 2. Change MY_NETWORK_ID to something unique (your student ID last 4 digits?)
 */

#include <SPI.h>
#include <LoRa.h>

// ============================================
// CHANGE THESE FOR EACH NODE!
// ============================================
#define MY_NODE_ID      1        // Node 1, 2, 3, 4, or 5
#define IS_SINK_NODE    false    // true for sink, false for regular

// ============================================
// CHANGE THIS TO YOUR UNIQUE NETWORK ID!
// ============================================
#define MY_NETWORK_ID   0xAB    // Change to 0x12, 0x34, 0x99, anything!
                                 // Same for ALL your nodes!

// ============================================
// Hardware pins (don't change)
// ============================================
#define LORA_SS         10
#define LORA_RST        9
#define LORA_DIO0       2

// ============================================
// Packets with Network ID
// ============================================
struct BeaconPacket {
  uint8_t networkId;      // NEW! Must match MY_NETWORK_ID
  uint8_t type;           // Always 1 for beacon
  uint8_t fromNode;       // Who sent this
  uint8_t distance;       // How far from sink
} __attribute__((packed));

struct DataPacket {
  uint8_t networkId;      // NEW! Must match MY_NETWORK_ID
  uint8_t type;           // Always 2 for data
  uint8_t fromNode;       // Who sent this
  uint8_t toNode;         // Who should receive
  uint8_t hops;           // How many jumps
  float temperature;      // Sensor reading
} __attribute__((packed));

// ============================================
// Network state
// ============================================
uint8_t myDistance = 255;
uint8_t bestNeighbor = 255;
int16_t bestNeighborRSSI = -200;

unsigned long lastBeacon = 0;
unsigned long lastData = 0;

// Statistics
uint16_t packetsReceived = 0;
uint16_t packetsIgnored = 0;

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(9600);
  while (!Serial);
  
  Serial.println(F("\n============================"));
  Serial.print(F("Node ID: "));
  Serial.println(MY_NODE_ID);
  Serial.print(F("Network ID: 0x"));
  Serial.println(MY_NETWORK_ID, HEX);
  Serial.print(F("Type: "));
  Serial.println(IS_SINK_NODE ? F("SINK") : F("REGULAR"));
  Serial.println(F("============================\n"));
  
  // Setup LoRa
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(923E6)) {
    Serial.println(F("LoRa FAILED!"));
    while (1);
  }
  
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setTxPower(17);
  LoRa.setSyncWord(0x12);
  
  Serial.println(F("LoRa OK!\n"));
  
  if (IS_SINK_NODE) {
    myDistance = 0;
    Serial.println(F("I am SINK (distance = 0)\n"));
  }
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  unsigned long now = millis();
  
  listenForPackets();
  
  if (now - lastBeacon > 5000) {
    sendBeacon();
    lastBeacon = now;
  }
  
  if (!IS_SINK_NODE && now - lastData > 20000) {
    sendData();
    lastData = now;
  }
  
  // Print stats every 60 seconds
  static unsigned long lastStats = 0;
  if (now - lastStats > 60000) {
    printStats();
    lastStats = now;
  }
  
  delay(10);
}

// ============================================
// SEND BEACON
// ============================================
void sendBeacon() {
  BeaconPacket pkt;
  pkt.networkId = MY_NETWORK_ID;  // ADD NETWORK ID!
  pkt.type = 1;
  pkt.fromNode = MY_NODE_ID;
  pkt.distance = myDistance;
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();
  
  Serial.print(F(">>> Beacon sent | Distance: "));
  Serial.println(myDistance);
}

// ============================================
// SEND DATA
// ============================================
void sendData() {
  if (bestNeighbor == 255) {
    Serial.println(F("XXX No route to sink!"));
    return;
  }
  
  DataPacket pkt;
  pkt.networkId = MY_NETWORK_ID;  // ADD NETWORK ID!
  pkt.type = 2;
  pkt.fromNode = MY_NODE_ID;
  pkt.toNode = bestNeighbor;
  pkt.hops = 0;
  pkt.temperature = 20.0 + random(0, 100) / 10.0;
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();
  
  Serial.print(F(">>> Data sent: "));
  Serial.print(pkt.temperature);
  Serial.print(F("°C -> Node "));
  Serial.println(bestNeighbor);
}

// ============================================
// LISTEN FOR PACKETS
// ============================================
void listenForPackets() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;
  
  uint8_t buffer[32];
  int len = 0;
  
  while (LoRa.available() && len < 32) {
    buffer[len++] = LoRa.read();
  }
  
  if (len < 4) return;  // Too small
  
  // CHECK NETWORK ID FIRST!
  uint8_t networkId = buffer[0];
  if (networkId != MY_NETWORK_ID) {
    packetsIgnored++;
    // Uncomment next line to see foreign packets:
    // Serial.print(F("! Foreign network: 0x")); Serial.println(networkId, HEX);
    return;  // IGNORE! Not our network!
  }
  
  uint8_t type = buffer[1];
  uint8_t fromNode = buffer[2];
  
  // Ignore my own packets
  if (fromNode == MY_NODE_ID) return;
  
  packetsReceived++;
  int16_t rssi = LoRa.packetRssi();
  
  // Handle beacon
  if (type == 1 && len == sizeof(BeaconPacket)) {
    BeaconPacket* beacon = (BeaconPacket*)buffer;
    
    Serial.print(F("<<< Beacon from Node "));
    Serial.print(beacon->fromNode);
    Serial.print(F(" | Distance: "));
    Serial.print(beacon->distance);
    Serial.print(F(" | RSSI: "));
    Serial.println(rssi);
    
    if (!IS_SINK_NODE) {
      if (beacon->distance < myDistance - 1) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        myDistance = beacon->distance + 1;
        
        Serial.print(F("*** NEW ROUTE! Distance now: "));
        Serial.print(myDistance);
        Serial.print(F(" via Node "));
        Serial.println(bestNeighbor);
      }
      else if (beacon->distance == myDistance - 1 && rssi > bestNeighborRSSI) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        Serial.print(F("*** BETTER ROUTE! via Node "));
        Serial.println(bestNeighbor);
      }
    }
  }
  
  // Handle data
  else if (type == 2 && len == sizeof(DataPacket)) {
    DataPacket* data = (DataPacket*)buffer;
    
    Serial.print(F("<<< Data from Node "));
    Serial.print(data->fromNode);
    Serial.print(F(" | Temp: "));
    Serial.print(data->temperature);
    Serial.print(F("°C | Hops: "));
    Serial.println(data->hops);
    
    if (IS_SINK_NODE) {
      Serial.println(F("========================="));
      Serial.println(F("SINK RECEIVED DATA!"));
      Serial.print(F("From: Node "));
      Serial.println(data->fromNode);
      Serial.print(F("Temperature: "));
      Serial.print(data->temperature);
      Serial.println(F("°C"));
      Serial.print(F("Hops: "));
      Serial.println(data->hops);
      Serial.println(F("========================="));
      return;
    }
    
    if (data->toNode == MY_NODE_ID) {
      if (bestNeighbor != 255) {
        data->toNode = bestNeighbor;
        data->hops++;
        
        delay(random(10, 50));
        
        LoRa.beginPacket();
        LoRa.write((uint8_t*)data, sizeof(DataPacket));
        LoRa.endPacket();
        
        Serial.print(F(">>> Forwarded to Node "));
        Serial.println(bestNeighbor);
      }
    }
  }
}

// ============================================
// PRINT STATISTICS
// ============================================
void printStats() {
  Serial.println(F("\n--- Statistics ---"));
  Serial.print(F("Packets from my network: "));
  Serial.println(packetsReceived);
  Serial.print(F("Foreign packets ignored: "));
  Serial.println(packetsIgnored);
  Serial.print(F("Current distance: "));
  Serial.println(myDistance);
  Serial.print(F("Best neighbor: "));
  if (bestNeighbor == 255) {
    Serial.println(F("None"));
  } else {
    Serial.print(F("Node "));
    Serial.println(bestNeighbor);
  }
  Serial.println(F("------------------\n"));
}