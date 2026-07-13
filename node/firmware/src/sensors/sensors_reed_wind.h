#pragma once

#include <stddef.h>

// Reed-switch cup anemometer backend (WH-SP-WS01) for the Node V2 sensor
// registry. Speed-only (a cup anemometer has no direction).
//
// Always compiled and registered — like any other sensor. It reads on whichever
// node actually has the anemometer wired to the reed pin (GPIO4 = the RX_EN_N
// net); on nodes without one it reports 0 (calm), and INPUT_PULLUP simply parks
// RX_EN_N in its safe high state. GPIO4 is NOT connected to the RTC (the DS3231
// alarm drives a FET gate that powers VSYS on — a separate hardware line), so
// there is no RTC interaction here. A short probe keeps the cost near-zero when
// there is no wind (or no sensor); only once a *train* of pulses is detected
// (>= REED_WIND_MIN_EDGES, which also rejects a stray electrical glitch) does it
// spend the full measurement window. Note: shares GPIO4 with the ultrasonic
// wind backend (RX_EN_N), so the two cannot run at the same time.
namespace reed_wind_backend {

bool init();
size_t count();
const char* label(size_t index);
const char* type(size_t index);
bool read(size_t index, float& outValue);

}  // namespace reed_wind_backend
