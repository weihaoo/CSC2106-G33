/*
 * SIMPLE LoRa Mesh Network - NO CHECKSUM VERSION
 * Just copy this ENTIRE file and upload!
 * 
 * IMPORTANT: Change MY_NODE_ID for each Arduino!
 */

#include <SPI.h>
#include <LoRa.h>

// ============================================
// CHANGE THIS FOR EACH NODE!
// ============================================
#define MY_NODE_ID      1        // Node 1, 2, 3, 4, or 5
#define IS_SINK_NODE    false    // true for sink, false for regular

// ============================================
// Hardware pins (don't change)
// ============================================
#define LORA_SS         10
#define LORA_RST        9
#define LORA_DIO0       2

// ============================================
// Simple packets (no checksum!)
// ============================================
struct BeaconPacket {
  uint8_t type;           // Always 1 for beacon
  uint8_t fromNode;       // Who sent this
  uint8_t distance;       // How far from sink
} __attribute__((packed));

struct DataPacket {
  uint8_t type;           // Always 2 for data
  uint8_t fromNode;       // Who sent this
  uint8_t toNode;         // Who should receive
  uint8_t hops;           // How many jumps
  float temperature;      // Sensor reading
} __attribute__((packed));

// ============================================
// Network state
// ============================================
uint8_t myDistance = 255;        // My distance to sink
uint8_t bestNeighbor = 255;      // Best node to send to
int16_t bestNeighborRSSI = -200; // Signal strength

unsigned long lastBeacon = 0;
unsigned long lastData = 0;

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(9600);
  while (!Serial);
  
  Serial.println("\n============================");
  Serial.print("Node ID: ");
  Serial.println(MY_NODE_ID);
  Serial.print("Type: ");
  Serial.println(IS_SINK_NODE ? "SINK" : "REGULAR");
  Serial.println("============================\n");
  
  // Setup LoRa
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(923E6)) {  // 923 MHz for Singapore
    Serial.println("LoRa FAILED!");
    while (1);
  }
  
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setTxPower(17);
  LoRa.setSyncWord(0x12);
  
  Serial.println("LoRa OK!\n");
  
  // Sink nodes start at distance 0
  if (IS_SINK_NODE) {
    myDistance = 0;
    Serial.println("I am SINK (distance = 0)\n");
  }
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  unsigned long now = millis();
  
  // Listen for packets
  listenForPackets();
  
  // Send beacon every 5 seconds
  if (now - lastBeacon > 5000) {
    sendBeacon();
    lastBeacon = now;
  }
  
  // Send data every 20 seconds (regular nodes only)
  if (!IS_SINK_NODE && now - lastData > 20000) {
    sendData();
    lastData = now;
  }
  
  delay(10);
}

// ============================================
// SEND BEACON
// ============================================
void sendBeacon() {
  BeaconPacket pkt;
  pkt.type = 1;
  pkt.fromNode = MY_NODE_ID;
  pkt.distance = myDistance;
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();
  
  Serial.print(">>> Beacon sent | Distance: ");
  Serial.println(myDistance);
}

// ============================================
// SEND DATA
// ============================================
void sendData() {
  // Check if we have a route
  if (bestNeighbor == 255) {
    Serial.println("XXX No route to sink!");
    return;
  }
  
  // Make packet
  DataPacket pkt;
  pkt.type = 2;
  pkt.fromNode = MY_NODE_ID;
  pkt.toNode = bestNeighbor;
  pkt.hops = 0;
  pkt.temperature = 20.0 + random(0, 100) / 10.0;  // Fake temperature
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();
  
  Serial.print(">>> Data sent: ");
  Serial.print(pkt.temperature);
  Serial.print("°C -> Node ");
  Serial.println(bestNeighbor);
}

// ============================================
// LISTEN FOR PACKETS
// ============================================
void listenForPackets() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;  // No packet
  
  uint8_t buffer[32];
  int len = 0;
  
  while (LoRa.available() && len < 32) {
    buffer[len++] = LoRa.read();
  }
  
  if (len < 3) return;  // Too small
  
  uint8_t type = buffer[0];
  uint8_t fromNode = buffer[1];
  
  // Ignore my own packets
  if (fromNode == MY_NODE_ID) return;
  
  int16_t rssi = LoRa.packetRssi();
  
  // Handle beacon
  if (type == 1 && len == sizeof(BeaconPacket)) {
    BeaconPacket* beacon = (BeaconPacket*)buffer;
    
    Serial.print("<<< Beacon from Node ");
    Serial.print(beacon->fromNode);
    Serial.print(" | Distance: ");
    Serial.print(beacon->distance);
    Serial.print(" | RSSI: ");
    Serial.println(rssi);
    
    // Update my routing
    if (!IS_SINK_NODE) {
      // If this neighbor is closer to sink
      if (beacon->distance < myDistance - 1) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        myDistance = beacon->distance + 1;
        
        Serial.print("*** NEW ROUTE! Distance now: ");
        Serial.print(myDistance);
        Serial.print(" via Node ");
        Serial.println(bestNeighbor);
      }
      // If same distance but better signal
      else if (beacon->distance == myDistance - 1 && rssi > bestNeighborRSSI) {
        bestNeighbor = beacon->fromNode;
        bestNeighborRSSI = rssi;
        
        Serial.print("*** BETTER ROUTE! via Node ");
        Serial.println(bestNeighbor);
      }
    }
  }
  
  // Handle data
  else if (type == 2 && len == sizeof(DataPacket)) {
    DataPacket* data = (DataPacket*)buffer;
    
    Serial.print("<<< Data from Node ");
    Serial.print(data->fromNode);
    Serial.print(" | Temp: ");
    Serial.print(data->temperature);
    Serial.print("°C | Hops: ");
    Serial.println(data->hops);
    
    // If I'm the sink - DONE!
    if (IS_SINK_NODE) {
      Serial.println("=========================");
      Serial.println("SINK RECEIVED DATA!");
      Serial.print("From: Node ");
      Serial.println(data->fromNode);
      Serial.print("Temperature: ");
      Serial.print(data->temperature);
      Serial.println("°C");
      Serial.print("Hops: ");
      Serial.println(data->hops);
      Serial.println("=========================");
      return;
    }
    
    // If it's for me - forward it
    if (data->toNode == MY_NODE_ID) {
      // Forward to my best neighbor
      if (bestNeighbor != 255) {
        data->toNode = bestNeighbor;
        data->hops++;
        
        delay(random(10, 50));  // Random delay
        
        LoRa.beginPacket();
        LoRa.write((uint8_t*)data, sizeof(DataPacket));
        LoRa.endPacket();
        
        Serial.print(">>> Forwarded to Node ");
        Serial.println(bestNeighbor);
      }
    }
  }
}