#pragma once
// V3 ultrasonic pin definitions — single source of truth
// All pins have #ifndef guards so platformio.ini build_flags can override.
//
// V3 changes from V2:
//   - PIN_DRV_W=13 was corrupted in V2 platformio.ini (fixed)
//   - REL_* reinterpreted as DAMP_*/SHUNT_* (net names unchanged)
//   - No new GPIO pins added in V3

#ifndef PIN_TOF_EDGE
#define PIN_TOF_EDGE 34
#endif
#ifndef PIN_RX_EN_N
#define PIN_RX_EN_N 4
#endif
#ifndef PIN_MUX_A
#define PIN_MUX_A 16
#endif
#ifndef PIN_MUX_B
#define PIN_MUX_B 17
#endif
#ifndef PIN_DRV_N
#define PIN_DRV_N 26
#endif
#ifndef PIN_DRV_E
#define PIN_DRV_E 27
#endif
#ifndef PIN_DRV_S
#define PIN_DRV_S 14
#endif
#ifndef PIN_DRV_W
#define PIN_DRV_W 13
#endif
#ifndef PIN_REL_N
#define PIN_REL_N 33
#endif
#ifndef PIN_REL_E
#define PIN_REL_E 32
#endif
#ifndef PIN_REL_S
#define PIN_REL_S 21
#endif
#ifndef PIN_REL_W
#define PIN_REL_W 22
#endif
#ifndef PIN_TX_BURST_PWM
#define PIN_TX_BURST_PWM 25
#endif
#ifndef PIN_TX_22V_EN_N
#define PIN_TX_22V_EN_N 5
#endif
#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 23
#endif