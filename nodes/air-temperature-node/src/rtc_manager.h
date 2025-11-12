#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Programs DS3231 Alarm1 for the NEXT boundary aligned to interval_min.
 * - If interval_min <= 1, it treats it as 1 minute (next MM+1 at :00).
 * - Uses absolute HH:MM:00 matching (sec=00, match hour/min, ignore day/date).
 * - Enables INTCN|A1IE, clears A1F.
 *
 * @param i2c_port   I2C master port already configured/installed.
 * @param interval_min  Desired interval (minutes). Will be clamped to {1,5,10,20,30,60}.
 * @param debug_out  Optional buffer to receive a printable debug log (may be NULL).
 * @param debug_len  Size of debug_out.
 * @param out_h      Optional: returns next alarm hour (24h).
 * @param out_m      Optional: returns next alarm minute.
 * @param out_s      Optional: returns next alarm second (always 0).
 *
 * @return ESP_OK on success, error otherwise.
 */
esp_err_t rtc_set_wake_interval_minutes(i2c_port_t i2c_port,
                                        uint8_t interval_min,
                                        char *debug_out,
                                        size_t debug_len,
                                        uint8_t *out_h,
                                        uint8_t *out_m,
                                        uint8_t *out_s);

#ifdef __cplusplus
}
#endif
