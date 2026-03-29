// Shared radio + PMU initialization for all mesh nodes

#ifndef MESH_RADIO_H
#define MESH_RADIO_H

#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <RadioLib.h>

#include "mesh_protocol.h"

// T-Beam SX1262 pin mapping
#define RADIO_NSS   18
#define RADIO_DIO1  33
#define RADIO_RST   23
#define RADIO_BUSY  32

// PMU I2C pins
#define PMU_SDA     21
#define PMU_SCL     22

// Each .ino must define these globals
extern volatile bool rxFlag;
extern XPowersAXP2101 PMU;
extern SX1262 radio;

// DIO1 interrupt handler
void IRAM_ATTR setRxFlag() {
    rxFlag = true;
}

// Initialize AXP2101 PMU and enable 3.3V rail for the radio
inline void init_pmu() {
    Wire.begin(PMU_SDA, PMU_SCL);
    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL)) {
        Serial.println(F("PMU init failed"));
        while (true) { delay(1000); }
    }
    PMU.setALDO2Voltage(3300);
    PMU.enableALDO2();
    delay(10);
}

// Initialize SX1262 with mesh protocol radio parameters
inline void init_radio() {
    int state = radio.begin(LORA_FREQUENCY);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("Radio begin failed, code "));
        Serial.println(state);
        while (true) { delay(1000); }
    }

    radio.setSpreadingFactor(LORA_SPREADING);
    radio.setBandwidth(LORA_BANDWIDTH);
    radio.setCodingRate(LORA_CODING_RATE);
    radio.setSyncWord(LORA_SYNC_WORD);
    radio.setOutputPower(LORA_TX_POWER);

    radio.setDio1Action(setRxFlag);
    rxFlag = false;

    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("startReceive failed, code "));
        Serial.println(state);
        while (true) { delay(1000); }
    }
}

#endif // MESH_RADIO_H
