// ads1115_helper.h
#pragma once
#include <Arduino.h>
#include <Wire.h>

class ADS1115 {
public:
    ADS1115(TwoWire &wire, uint8_t address = 0x48, float vref = 4.096f);

    // Probe device on bus, return true if found
    bool begin();

    // Read single-ended channel 0..3, return raw + millivolts
    bool readChannelMv(uint8_t ch, int16_t &rawOut, float &mvOut);

private:
    TwoWire   *m_wire;
    uint8_t    m_addr;
    float      m_vref;
    uint16_t   m_baseConfig;
};
