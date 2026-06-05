// ads1115_helper.cpp
#include "ads1115_helper.h"

ADS1115::ADS1115(TwoWire &wire, uint8_t address, float vref)
: m_wire(&wire),
  m_addr(address),
  m_vref(vref)
{
    // OS=1, PGA=±4.096V, MODE=single-shot, DR=128SPS, COMP disabled
    m_baseConfig = 0x8000 | 0x0200 | 0x0100 | 0x0080 | 0x0003;
}

bool ADS1115::begin() {
    m_wire->beginTransmission(m_addr);
    uint8_t err = m_wire->endTransmission();
    if (err != 0) {
        Serial.printf("[ADS] probe @0x%02X failed (err=%u)\n", m_addr, err);
        return false;
    }
    Serial.printf("[ADS] probe @0x%02X OK\n", m_addr);
    return true;
}

bool ADS1115::readChannelMv(uint8_t ch, int16_t &rawOut, float &mvOut) {
    if (ch > 3) return false;

    // single-ended mux: 100=AIN0, 101=AIN1, 110=AIN2, 111=AIN3
    uint16_t muxBits = 0x4000 | ((ch & 0x03) << 12);
    uint16_t cfg     = m_baseConfig | muxBits;

    uint8_t cfgBytes[2] = {
        (uint8_t)(cfg >> 8),
        (uint8_t)(cfg & 0xFF)
    };

    // Write config
    m_wire->beginTransmission(m_addr);
    m_wire->write(0x01);                  // config register
    m_wire->write(cfgBytes, 2);
    if (m_wire->endTransmission() != 0) {
        return false;
    }

    // Wait conversion (~8ms at 128SPS)
    delay(9);

    // Read conversion register
    m_wire->beginTransmission(m_addr);
    m_wire->write((uint8_t)0x00);
    if (m_wire->endTransmission(false) != 0) {
        return false;
    }

    if (m_wire->requestFrom((int)m_addr, 2) != 2) {
        return false;
    }

    uint8_t hi = m_wire->read();
    uint8_t lo = m_wire->read();
    int16_t val = (int16_t)((hi << 8) | lo);

    rawOut = val;
    mvOut  = (val / 32768.0f) * m_vref * 1000.0f;

    return true;
}
