#pragma once

// Mothership V1 PCB pin definitions
// All GPIO assignments match the V1 schematic.
// Use #ifndef guards so platformio.ini build_flags can override.

// Power control
#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD        26
#endif
#ifndef PIN_CONFIG_WAKE
#define PIN_CONFIG_WAKE      32
#endif
#ifndef PIN_CONFIG_CLEAR
#define PIN_CONFIG_CLEAR     25
#endif
#ifndef PIN_CFG_LED
#define PIN_CFG_LED          27
#endif

// Modem control
#ifndef PIN_4V_EN
#define PIN_4V_EN            33
#endif
#ifndef PIN_MODEM_PWRKEY
#define PIN_MODEM_PWRKEY     14
#endif
#ifndef PIN_MODEM_STATUS
#define PIN_MODEM_STATUS      4
#endif
#ifndef PIN_MODEM_PG
#define PIN_MODEM_PG         35  // TPS63020 power-good (input-only)
#endif

// Modem UART
#ifndef PIN_MODEM_TX
#define PIN_MODEM_TX         17   // ESP32 TX2 → modem RXD
#endif
#ifndef PIN_MODEM_RX
#define PIN_MODEM_RX         16   // ESP32 RX2 ← modem TXD
#endif

// Battery ADC
#ifndef PIN_BATTERY_ADC
#define PIN_BATTERY_ADC      34   // VOLT_ESP, ADC1 (input-only)
#endif

// I2C (DS3231)
#ifndef PIN_SDA
#define PIN_SDA              21
#endif
#ifndef PIN_SCL
#define PIN_SCL              22
#endif

// SPI (SD card)
#ifndef PIN_SD_CS
#define PIN_SD_CS            13
#endif
#ifndef PIN_SD_SCK
#define PIN_SD_SCK           18
#endif
#ifndef PIN_SD_MISO
#define PIN_SD_MISO          19
#endif
#ifndef PIN_SD_MOSI
#define PIN_SD_MOSI          23
#endif

// Battery voltage divider constants
#ifndef BAT_DIVIDER_R1
#define BAT_DIVIDER_R1       220000.0f  // 220 kΩ
#endif
#ifndef BAT_DIVIDER_R2
#define BAT_DIVIDER_R2       100000.0f   // 100 kΩ
#endif
#ifndef BAT_ADC_VREF
#define BAT_ADC_VREF         3.3f        // ADC reference voltage
#endif
#ifndef BAT_ADC_MAX
#define BAT_ADC_MAX          4095.0f     // 12-bit ADC
#endif
#ifndef BAT_ADC_SAMPLES
#define BAT_ADC_SAMPLES      16
#endif

// Config latch timing
#ifndef CONFIG_CLEAR_PULSE_MS
#define CONFIG_CLEAR_PULSE_MS  20
#endif

// Modem power timing
#ifndef MODEM_PG_TIMEOUT_MS
#define MODEM_PG_TIMEOUT_MS    5000
#endif
#ifndef MODEM_PWRKEY_ON_MS
#define MODEM_PWRKEY_ON_MS     1100  // A7670 PWRKEY pulse
#endif
#ifndef MODEM_BOOT_WAIT_MS
#define MODEM_BOOT_WAIT_MS     5000
#endif

// Sync defaults
#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL         11
#endif
#ifndef SYNC_WINDOW_MS
#define SYNC_WINDOW_MS         60000
#endif
#ifndef DEFAULT_SYNC_INTERVAL_MIN
#define DEFAULT_SYNC_INTERVAL_MIN 60
#endif