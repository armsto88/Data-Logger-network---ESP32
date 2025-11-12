#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"  
#include "driver/i2c.h"
#include "rtc_manager.h"


#define DS3231_ADDR 0x68
static const char* TAG = "rtc_mgr";

/* ----------------- small helpers ----------------- */
static inline uint8_t to_bcd(uint8_t v) { return (uint8_t)(((v/10)<<4) | (v%10)); }
static inline uint8_t from_bcd(uint8_t b) { return (uint8_t)(((b>>4)&0x0F)*10 + (b&0x0F)); }

static esp_err_t i2c_write_byte(i2c_port_t port, uint8_t dev, uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev<<1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_read_bytes(i2c_port_t port, uint8_t dev, uint8_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev<<1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev<<1) | I2C_MASTER_READ, true);
    if (len > 1) i2c_master_read(cmd, buf, len-1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf+len-1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

/* Read DS3231 time (seconds, minutes, hours) in BCD */
static esp_err_t ds3231_read_time(i2c_port_t port, uint8_t *sec, uint8_t *min, uint8_t *hour24) {
    uint8_t b[3] = {0};
    ESP_RETURN_ON_ERROR(i2c_read_bytes(port, DS3231_ADDR, 0x00, b, 3), TAG, "read time failed");
    *sec   = from_bcd(b[0]);
    *min   = from_bcd(b[1]);
    *hour24= from_bcd(b[2] & 0x3F); // 24h bits
    return ESP_OK;
}

static esp_err_t ds3231_read_status(i2c_port_t port, uint8_t *status) {
    return i2c_read_bytes(port, DS3231_ADDR, 0x0F, status, 1);
}
static esp_err_t ds3231_write_status(i2c_port_t port, uint8_t s) {
    return i2c_write_byte(port, DS3231_ADDR, 0x0F, s);
}
static esp_err_t ds3231_read_ctrl(i2c_port_t port, uint8_t *ctrl) {
    return i2c_read_bytes(port, DS3231_ADDR, 0x0E, ctrl, 1);
}
static esp_err_t ds3231_write_ctrl(i2c_port_t port, uint8_t c) {
    return i2c_write_byte(port, DS3231_ADDR, 0x0E, c);
}

/* Write Alarm1 regs 0x07..0x0A in one shot */
static esp_err_t ds3231_write_a1(i2c_port_t port, uint8_t secReg, uint8_t minReg, uint8_t hourReg, uint8_t dayReg) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_ADDR<<1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x07, true);
    i2c_master_write_byte(cmd, secReg, true);
    i2c_master_write_byte(cmd, minReg, true);
    i2c_master_write_byte(cmd, hourReg, true);
    i2c_master_write_byte(cmd, dayReg, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

/* ----------------- main API ----------------- */
esp_err_t rtc_set_wake_interval_minutes(i2c_port_t port,
                                        uint8_t interval_min,
                                        char *dbg,
                                        size_t dbg_len,
                                        uint8_t *out_h,
                                        uint8_t *out_m,
                                        uint8_t *out_s)
{
    // clamp to your allowed set
    switch (interval_min) {
        case 1: case 5: case 10: case 20: case 30: case 60: break;
        default: interval_min = 5; break;
    }

    // Get current RTC time
    uint8_t sec=0, min=0, hr=0;
    ESP_RETURN_ON_ERROR(ds3231_read_time(port, &sec, &min, &hr), TAG, "read time");

    // Compute NEXT boundary (ceil) aligned to interval_min, with seconds=00
    // If not exactly :00, jump to next minute first
    if (sec != 0) {
        min += 1;
        if (min >= 60) { min = 0; hr = (hr + 1) % 24; }
        sec = 0;
    }
    uint8_t mod = (interval_min > 0) ? (min % interval_min) : 0;
    uint8_t add = (mod == 0) ? interval_min : (interval_min - mod);
    if (add == 0) add = interval_min; // never arm "now"

    uint16_t total_min = (uint16_t)min + add;
    uint8_t next_h = (uint8_t)((hr + (total_min/60)) % 24);
    uint8_t next_m = (uint8_t)(total_min % 60);
    uint8_t next_s = 0;

    // Program A1: match sec=00 (A1M1=0), match minute (A1M2=0), match hour (A1M3=0), ignore day/date (A1M4=1)
    // Day register: set A1M4=1, DY/DT=0 → lower bits ignored → 0x80 is fine.
    // (If you prefer your previous style, you can write 0x80 | to_bcd(1).)
    uint8_t a1_sec  = to_bcd(0);          // A1M1=0
    uint8_t a1_min  = to_bcd(next_m);     // A1M2=0
    uint8_t a1_hour = to_bcd(next_h);     // A1M3=0 (24h)
    uint8_t a1_day  = 0x80;               // A1M4=1, DY/DT=0

    // Enable INTCN | A1IE
    uint8_t ctrl=0;
    if (ds3231_read_ctrl(port, &ctrl) == ESP_OK) {
        ctrl |= 0b00000101;
        ds3231_write_ctrl(port, ctrl);
    }

    // Clear A1F
    uint8_t stat=0;
    if (ds3231_read_status(port, &stat) == ESP_OK) {
        stat &= ~0x01;
        ds3231_write_status(port, stat);
    }

    // Write A1
    ESP_RETURN_ON_ERROR(ds3231_write_a1(port, a1_sec, a1_min, a1_hour, a1_day), TAG, "write A1");

    // Outputs
    if (out_h) *out_h = next_h;
    if (out_m) *out_m = next_m;
    if (out_s) *out_s = next_s;

    // Debug text
    if (dbg && dbg_len) {
        int n = snprintf(dbg, dbg_len,
            "Interval: %u min\n"
            "Next alarm (daily): %02u:%02u:%02u\n"
            "A1 regs: %02X %02X %02X %02X\n"
            "CTRL (after INTCN|A1IE): 0x%02X\n"
            "STAT (A1F cleared):      0x%02X\n",
            (unsigned)interval_min,
            (unsigned)next_h, (unsigned)next_m, (unsigned)next_s,
            (unsigned)a1_sec, (unsigned)a1_min, (unsigned)a1_hour, (unsigned)a1_day,
            (unsigned)ctrl, (unsigned)stat);
        (void)n;
    }

    ESP_LOGI(TAG, "Armed next alarm in %u-minute mode → %02u:%02u:%02u",
             (unsigned)interval_min, next_h, next_m, next_s);
    return ESP_OK;
}
