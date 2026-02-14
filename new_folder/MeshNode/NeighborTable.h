#ifndef NEIGHBOR_TABLE_H
#define NEIGHBOR_TABLE_H

#include "MeshPackets.h"
#include "NodeConfig.h"

#define MAX_NEIGHBORS 6  // Reduced from 10

struct Neighbor {
  uint8_t nodeId;
  uint8_t nodeType;
  uint8_t distanceToSink;
  int16_t rssi;
  uint32_t lastHeard;
  bool isValid;
};

class NeighborTable {
private:
  Neighbor neighbors[MAX_NEIGHBORS];
  
public:
  NeighborTable() {
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      neighbors[i].isValid = false;
    }
  }
  
  void updateNeighbor(uint8_t id, uint8_t type, uint8_t distance, int16_t rssi) {
    int slot = findNeighbor(id);
    if (slot == -1) slot = findEmptySlot();
    
    if (slot != -1) {
      neighbors[slot].nodeId = id;
      neighbors[slot].nodeType = type;
      neighbors[slot].distanceToSink = distance;
      neighbors[slot].rssi = rssi;
      neighbors[slot].lastHeard = millis();
      neighbors[slot].isValid = true;
    }
  }
  
  int findNeighbor(uint8_t id) {
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      if (neighbors[i].isValid && neighbors[i].nodeId == id) {
        return i;
      }
    }
    return -1;
  }
  
  int findEmptySlot() {
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      if (!neighbors[i].isValid) return i;
    }
    return -1;
  }
  
  void cleanupOldNeighbors() {
    uint32_t now = millis();
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      if (neighbors[i].isValid && 
          (now - neighbors[i].lastHeard) > NEIGHBOR_TIMEOUT) {
        neighbors[i].isValid = false;
      }
    }
  }
  
  uint8_t getBestNextHop() {
    int bestSlot = -1;
    uint8_t lowestDistance = INVALID_DISTANCE;
    int16_t bestRssi = -200;
    
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      if (!neighbors[i].isValid) continue;
      
      if (neighbors[i].distanceToSink < lowestDistance) {
        lowestDistance = neighbors[i].distanceToSink;
        bestRssi = neighbors[i].rssi;
        bestSlot = i;
      } 
      else if (neighbors[i].distanceToSink == lowestDistance && 
               neighbors[i].rssi > bestRssi) {
        bestRssi = neighbors[i].rssi;
        bestSlot = i;
      }
    }
    
    if (bestSlot != -1) return neighbors[bestSlot].nodeId;
    return 0xFF;
  }
  
  uint8_t getMyDistanceToSink() {
    uint8_t minDistance = INVALID_DISTANCE;
    
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      if (neighbors[i].isValid && 
          neighbors[i].distanceToSink < minDistance) {
        minDistance = neighbors[i].distanceToSink;
      }
    }
    
    if (minDistance == INVALID_DISTANCE) return INVALID_DISTANCE;
    return minDistance + 1;
  }
  
  uint8_t getNeighborCount() {
    uint8_t count = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      if (neighbors[i].isValid) count++;
    }
    return count;
  }
  
  void printTable() {
    Serial.println(F("=== Neighbors ==="));
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
      if (neighbors[i].isValid) {
        Serial.print(F("N"));
        Serial.print(neighbors[i].nodeId);
        Serial.print(F(" D:"));
        Serial.print(neighbors[i].distanceToSink);
        Serial.print(F(" R:"));
        Serial.println(neighbors[i].rssi);
      }
    }
  }
};

#endif