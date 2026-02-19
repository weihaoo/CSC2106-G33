/*
 * Simple LoRa Transmitter Test
 * Hardware: Cytron LoRa-RFM Shield + Maker UNO
 * 
 * This code sends a simple message every 2 seconds and increments a counter.
 * Use this to test basic LoRa transmission before building mesh networking.
 */

#include <SPI.h>
#include <LoRa.h>

// Pin definitions for Cytron LoRa-RFM Shield
#define LORA_SS    10   // NSS pin
#define LORA_RST   9    // Reset pin
#define LORA_DIO0  2    // DIO0 pin

// LoRa frequency - IMPORTANT: Use 923MHz for Singapore (AS923)
#define LORA_FREQUENCY 923E6  // 923 MHz for Singapore

// Message counter
int messageCounter = 0;

void setup() {
  // Initialize serial for debugging
  Serial.begin(9600);
  while (!Serial);  // Wait for serial port to connect (for debugging)
  
  Serial.println("LoRa Transmitter Test");
  Serial.println("Cytron LoRa-RFM Shield");
  
  // Configure LoRa pins
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  
  // Initialize LoRa module
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("ERROR: LoRa initialization failed!");
    Serial.println("Check:");
    Serial.println("1. Shield is properly seated on Maker UNO");
    Serial.println("2. Antenna is connected");
    Serial.println("3. Power supply is adequate");
    while (1);  // Halt if initialization fails
  }
  
  Serial.println("LoRa initialized successfully!");
  
  // Optional: Configure LoRa parameters for better range
  LoRa.setSpreadingFactor(7);     // SF7 to SF12 (higher = longer range, slower)
  LoRa.setSignalBandwidth(125E3);  // 125 kHz bandwidth
  LoRa.setCodingRate4(5);          // 4/5 coding rate
  LoRa.setTxPower(17);             // TX power in dBm (max 20 for RFM95)
  
  Serial.println("Configuration:");
  Serial.println("  Frequency: 923 MHz");
  Serial.println("  Spreading Factor: 7");
  Serial.println("  Bandwidth: 125 kHz");
  Serial.println("  TX Power: 17 dBm");
  Serial.println("\nStarting transmission...\n");
}

void loop() {
  // Create message
  String message = "Hello LoRa #" + String(messageCounter);
  
  // Print to serial
  Serial.print("Sending: ");
  Serial.println(message);
  
  // Send LoRa packet
  LoRa.beginPacket();
  LoRa.print(message);
  LoRa.endPacket();
  
  // Increment counter
  messageCounter++;
  
  // Wait 2 seconds before next transmission
  delay(2000);
}