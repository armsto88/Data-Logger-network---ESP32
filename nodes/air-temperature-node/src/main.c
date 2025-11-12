// src/main.c  — verbose logging + safer alarm handling
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_random.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

// ---- shared protocol & pins ----
#include "protocol.h"

#ifndef NODE_ID
#  define NODE_ID   "TEMP_001"
#endif
#ifndef NODE_TYPE
#  define NODE_TYPE "temperature"
#endif

// Fallbacks if protocol.h didn’t define them
#ifndef RTC_SDA_PIN
#  define RTC_SDA_PIN 8
#endif
#ifndef RTC_SCL_PIN
#  define RTC_SCL_PIN 9
#endif
#ifndef RTC_INT_PIN
#  define RTC_INT_PIN 3   // ESP32-C3: RTC-capable GPIO
#endif
#ifndef ESPNOW_CHANNEL
#  define ESPNOW_CHANNEL 1
#endif

#define I2C_PORT       I2C_NUM_0
#define I2C_FREQ_HZ    100000
#define DS3231_ADDR    0x68

static const char* TAG = "NODE";

typedef enum { STATE_UNPAIRED=0, STATE_PAIRED=1, STATE_DEPLOYED=2 } node_state_t;

// --------- retained across deep sleep ----------
RTC_DATA_ATTR static node_state_t g_state = STATE_UNPAIRED;
RTC_DATA_ATTR static uint8_t      g_mothership[6] = {0};
RTC_DATA_ATTR static uint8_t      g_interval_min = 1;   // default 1 minute

// ================= helpers =================
static inline const char* state_str(node_state_t s){
    switch(s){ case STATE_UNPAIRED: return "UNPAIRED"; case STATE_PAIRED: return "PAIRED"; case STATE_DEPLOYED: return "DEPLOYED"; default: return "?"; }
}
static void print_mac(const char* label, const uint8_t mac[6]){
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
        label, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}
static uint8_t clamp_interval(uint8_t m){
    if (m == 0) return 1;         // never allow 0 → arm at least 1 minute
    if (m > 60) return 60;        // practical cap
    return m;
}

// ================= I2C helpers =================
static esp_err_t i2c_write_byte(uint8_t dev, uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd, ESP_ERR_NO_MEM, TAG, "i2c link alloc");
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev<<1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_read_bytes(uint8_t dev, uint8_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd, ESP_ERR_NO_MEM, TAG, "i2c link alloc");
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev<<1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev<<1) | I2C_MASTER_READ, true);
    if (len > 1) i2c_master_read(cmd, buf, len-1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf+len-1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

// ============== DS3231 helpers (A1 alarm) ==============
static uint8_t bcd(uint8_t v) { return (uint8_t)(((v/10)<<4) | (v%10)); }

static esp_err_t ds3231_read_status(uint8_t *status) {
    return i2c_read_bytes(DS3231_ADDR, 0x0F, status, 1);
}
static esp_err_t ds3231_write_status(uint8_t status) {
    return i2c_write_byte(DS3231_ADDR, 0x0F, status);
}
static esp_err_t ds3231_read_ctrl(uint8_t *ctrl){
    return i2c_read_bytes(DS3231_ADDR, 0x0E, ctrl, 1);
}
static esp_err_t ds3231_clear_a1f(void) {
    uint8_t s=0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_read_status(&s));
    s &= (uint8_t)~0x01; // clear A1F
    return ds3231_write_status(s);
}
static esp_err_t ds3231_enable_int_a1(void) {
    uint8_t ctrl=0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_read_ctrl(&ctrl));
    // INTCN | A1IE
    ctrl |= 0b00000101;
    return i2c_write_byte(DS3231_ADDR, 0x0E, ctrl);
}
static void log_rtc_regs(const char* where){
    uint8_t ctrl=0, stat=0;
    if (ds3231_read_ctrl(&ctrl) == ESP_OK && ds3231_read_status(&stat) == ESP_OK){
        ESP_LOGI(TAG, "[RTC] %s CTRL=0x%02X STATUS=0x%02X (A1F=%u, A2F=%u, BSY=%u, EN32k=%u)",
            where, ctrl, stat, (unsigned)(stat&1), (unsigned)((stat>>1)&1), (unsigned)((stat>>2)&1), (unsigned)((ctrl>>3)&1));
    }
}
static void log_rtc_time(const char* prefix){
    uint8_t tb[7]={0}; // sec,min,hour,day,date,month,year
    if (i2c_read_bytes(DS3231_ADDR, 0x00, tb, 7) == ESP_OK){
        uint8_t sec = (uint8_t)((tb[0]>>4)*10 + (tb[0] & 0x0F));
        uint8_t min = (uint8_t)((tb[1]>>4)*10 + (tb[1] & 0x0F));
        uint8_t hrb = (uint8_t)(tb[2] & 0x3F);
        uint8_t hr  = (uint8_t)((hrb>>4)*10 + (hrb & 0x0F));
        uint8_t day = (uint8_t)((tb[4]>>4)*10 + (tb[4] & 0x0F));
        uint8_t mon = (uint8_t)((tb[5]>>4)*10 + (tb[5] & 0x0F));
        uint8_t yr  = (uint8_t)((tb[6]>>4)*10 + (tb[6] & 0x0F)); // 00..99
        ESP_LOGI(TAG, "[RTC] %s %02u-%02u-%02u %02u:%02u:%02u", prefix, (unsigned)yr, (unsigned)mon, (unsigned)day, (unsigned)hr, (unsigned)min, (unsigned)sec);
    }
}
static esp_err_t ds3231_write_a1(uint8_t secReg, uint8_t minReg, uint8_t hourReg, uint8_t dayReg) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd, ESP_ERR_NO_MEM, TAG, "i2c link alloc");
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_ADDR<<1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x07, true);
    i2c_master_write_byte(cmd, secReg, true);
    i2c_master_write_byte(cmd, minReg, true);
    i2c_master_write_byte(cmd, hourReg, true);
    i2c_master_write_byte(cmd, dayReg, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

// Program :00 every minute
static esp_err_t ds3231_every_minute(void) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_enable_int_a1());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_clear_a1f());
    esp_err_t e = ds3231_write_a1(0x00, 0x80, 0x80, 0x80); // A1M2..A1M4 set
    ESP_LOGI(TAG, "[A1] Armed every minute at :00");
    return e;
}

// Arm next absolute HH:MM:00 at N-minute boundary
static esp_err_t ds3231_arm_next_n_minutes(uint8_t interval_min, uint8_t *next_h, uint8_t *next_m) {
    uint8_t tb[3] = {0}; // sec, min, hour (BCD 24h)
    ESP_RETURN_ON_ERROR(i2c_read_bytes(DS3231_ADDR, 0x00, tb, 3), TAG, "read time");
    uint8_t sec = (uint8_t)((tb[0]>>4)*10 + (tb[0] & 0x0F));
    uint8_t min = (uint8_t)((tb[1]>>4)*10 + (tb[1] & 0x0F));
    uint8_t hrb = tb[2] & 0x3F;
    uint8_t hr  = (uint8_t)((hrb>>4)*10 + (hrb & 0x0F));

    if (sec != 0) {
        min += 1;
        sec = 0;
        if (min >= 60) { min = 0; hr = (uint8_t)((hr+1)%24); }
    }
    uint8_t mod = (uint8_t)(min % interval_min);
    uint8_t add = (mod == 0) ? interval_min : (uint8_t)(interval_min - mod);

    uint16_t total_min = (uint16_t)min + add;
    hr  = (uint8_t)((hr + (total_min/60)) % 24);
    min = (uint8_t)(total_min % 60);

    ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_enable_int_a1());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ds3231_clear_a1f());
    ESP_RETURN_ON_ERROR(ds3231_write_a1(0x00, bcd(min), bcd(hr), 0x80), TAG, "write A1");

    if (next_h) *next_h = hr;
    if (next_m) *next_m = min;
    ESP_LOGI(TAG, "[A1] Next alarm -> %02u:%02u:00 (every %u min)", hr, min, interval_min);
    return ESP_OK;
}

// Clear pending and arm next alarm (with logging)
static esp_err_t ensure_next_alarm(uint8_t interval_min, uint8_t *h, uint8_t *m) {
    interval_min = clamp_interval(interval_min);

    uint8_t s=0;
    if (ds3231_read_status(&s) == ESP_OK && (s & 0x01)) {
        ESP_LOGI(TAG, "[A1] Pending A1F detected, clearing…");
        ds3231_clear_a1f();
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    log_rtc_time("now");
    log_rtc_regs("before-arm");

    esp_err_t e = (interval_min <= 1)
        ? ds3231_every_minute()
        : ds3231_arm_next_n_minutes(interval_min, h, m);

    log_rtc_regs("after-arm");
    return e;
}

// ======== Deep-sleep wake using GPIO (IDF 5.x, ESP32-C3) ========
static void enable_alarm_gpio_wakeup(void) {
    gpio_num_t pin = (gpio_num_t)RTC_INT_PIN;

    // Clear any prior wake sources to avoid conflicts
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // DS3231 INT is open-drain, idle HIGH → enable pull-up
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);

    uint64_t mask = (1ULL << pin);
    esp_err_t e = esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    ESP_LOGI(TAG, "[SLEEP] GPIO wake on pin %d (LOW), status=%d", (int)pin, (int)e);
}

static void go_deep_sleep(void) {
    ESP_LOGI(TAG, "[SLEEP] Going to deep sleep… (state=%s, interval=%u min)", state_str(g_state), g_interval_min);
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_deep_sleep_start();
}

// =================== ESP-NOW ======================
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    ESP_LOGI(TAG, "RX %d bytes from %02X:%02X:%02X:%02X:%02X:%02X (state=%s, interval=%u)",
             len, info->src_addr[0],info->src_addr[1],info->src_addr[2],
             info->src_addr[3],info->src_addr[4],info->src_addr[5], state_str(g_state), g_interval_min);

    // Discovery response
    if (len == sizeof(discovery_response_t)) {
        const discovery_response_t *r = (const discovery_response_t*)data;
        if (strcmp(r->command, "DISCOVER_RESPONSE")==0) {
            memcpy(g_mothership, info->src_addr, 6);
            ESP_LOGI(TAG, "[PAIR] Discovered mothership id='%s'", r->mothership_id);
            print_mac("[PAIR] Mothership MAC:", g_mothership);

            esp_now_peer_info_t p = {0};
            memcpy(p.peer_addr, g_mothership, 6);
            p.ifidx   = WIFI_IF_STA;
            p.channel = ESPNOW_CHANNEL;
            p.encrypt = false;

            (void)esp_now_del_peer(g_mothership);
            esp_err_t a = esp_now_add_peer(&p);
            ESP_LOGI(TAG, "[PAIR] add_peer -> %s", (a==ESP_OK)?"OK":"FAIL");
            if (g_state == STATE_UNPAIRED) {
                g_state = STATE_PAIRED;
                ESP_LOGI(TAG, "[STATE] -> PAIRED");
            }
        }
        return;
    }

    // Deployment (RTC sync)
    if (len == sizeof(deployment_command_t)) {
        const deployment_command_t *dc = (const deployment_command_t*)data;
        if ((strcmp(dc->command,"DEPLOY_NODE")==0) && (strcmp(dc->nodeId,NODE_ID)==0)) {
            // set time (HH:MM:SS only here; date setting omitted for brevity)
            (void)i2c_write_byte(DS3231_ADDR, 0x00, bcd((uint8_t)dc->second));
            (void)i2c_write_byte(DS3231_ADDR, 0x01, bcd((uint8_t)dc->minute));
            (void)i2c_write_byte(DS3231_ADDR, 0x02, bcd((uint8_t)dc->hour)); // 24h
            ESP_LOGI(TAG, "[DEPLOY] RTC set to %04lu-%02lu-%02lu %02lu:%02lu:%02lu",
                     dc->year, dc->month, dc->day, dc->hour, dc->minute, dc->second);
            log_rtc_time("after-deploy");
            g_state = STATE_DEPLOYED;
            ESP_LOGI(TAG, "[STATE] -> DEPLOYED");

            uint8_t nh=0,nm=0;
            esp_err_t ok = ensure_next_alarm(g_interval_min, &nh, &nm);
            ESP_LOGI(TAG, "[DEPLOY] next=%02u:%02u (arm=%s)", nh, nm, ok==ESP_OK?"OK":"FAIL");
        }
        return;
    }

    // Schedule
    if (len == sizeof(schedule_command_message_t)) {
        const schedule_command_message_t *cmd = (const schedule_command_message_t*)data;
        if (strcmp(cmd->command,"SET_SCHEDULE")==0) {
            uint8_t old = g_interval_min;
            g_interval_min = clamp_interval((uint8_t)cmd->intervalMinutes);
            uint8_t nh=0,nm=0;
            esp_err_t ok = ensure_next_alarm(g_interval_min, &nh, &nm);
            ESP_LOGI(TAG, "[SET_SCHEDULE] %u->%u min | next %02u:%02u (%s)",
                     old, g_interval_min, nh, nm, ok==ESP_OK?"OK":"FAIL");
        }
        return;
    }

    // Unpair
    if (len == sizeof(unpair_command_t)) {
        const unpair_command_t *u = (const unpair_command_t*)data;
        if (strcmp(u->command,"UNPAIR_NODE")==0) {
            memset(g_mothership, 0, sizeof(g_mothership));
            ESP_LOGI(TAG, "[PAIR] remote UNPAIR received");
            g_state = STATE_UNPAIRED;
            ESP_LOGI(TAG, "[STATE] -> UNPAIRED");
        }
        return;
    }
}

// IDF 5.x signature: wifi_tx_info_t*
static void on_sent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    const uint8_t *mac = tx_info ? tx_info->des_addr : NULL; // destination MAC
    if (mac) {
        ESP_LOGI(TAG, "TX %s to %02X:%02X:%02X:%02X:%02X:%02X",
                 status==ESP_NOW_SEND_SUCCESS?"OK":"FAIL",
                 mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    } else {
        ESP_LOGI(TAG, "TX %s", status==ESP_NOW_SEND_SUCCESS?"OK":"FAIL");
    }
}

static void send_discover(void) {
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    discovery_message_t m = {0};
    strcpy(m.nodeId, NODE_ID);
    strcpy(m.nodeType, NODE_TYPE);
    strcpy(m.command, "DISCOVER_REQUEST");
    m.timestamp = (uint32_t)(esp_timer_get_time()/1000ULL);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    esp_err_t r = esp_now_send(bcast, (uint8_t*)&m, sizeof(m));
    ESP_LOGI(TAG, "[DISCOVER] sent (%s)", (r==ESP_OK)?"OK":"FAIL");
}

static void send_sensor(void) {
    if (g_state != STATE_DEPLOYED) {
        ESP_LOGW(TAG, "Not deployed, skip data");
        return;
    }
    // Dummy temperature for demo
    float temp_c = 20.0f + (float)(esp_random() % 200) / 10.0f;

    sensor_data_message_t s = (sensor_data_message_t){0};
    strcpy(s.nodeId, NODE_ID);
    strcpy(s.sensorType, NODE_TYPE);
    s.value = temp_c;
    s.nodeTimestamp = (uint32_t)(esp_timer_get_time()/1000000ULL);

    print_mac("[DATA] send ->", g_mothership);
    esp_err_t r = esp_now_send(g_mothership, (uint8_t*)&s, sizeof(s));
    ESP_LOGI(TAG, "[DATA] temp=%.2f °C (%s)", (double)temp_c, (r==ESP_OK)?"queued":"send_fail");
}

// ========================= app_main =========================
void app_main(void) {
    // Boot context
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "===== BOOT ===== reset_reason=%d", (int)rr);

    // NVS / netif / Wi-Fi
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t self_mac[6];
    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);
    print_mac("[WIFI] STA MAC:", self_mac);
    ESP_LOGI(TAG, "[WIFI] ESPNOW channel=%d", ESPNOW_CHANNEL);

    // ESPNOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));

    // Broadcast peer
    esp_now_peer_info_t bpeer = (esp_now_peer_info_t){0};
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    memcpy(bpeer.peer_addr, bcast, 6);
    bpeer.ifidx = WIFI_IF_STA;
    bpeer.channel = ESPNOW_CHANNEL;
    bpeer.encrypt = false;
    esp_err_t ap = esp_now_add_peer(&bpeer);
    ESP_LOGI(TAG, "[ESPNOW] add broadcast peer -> %s", (ap==ESP_OK)?"OK":"FAIL");

    // Force ESPNOW channel
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    // I²C init
    i2c_config_t icfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RTC_SDA_PIN,
        .scl_io_num = RTC_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &icfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    // Wake cause
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wake cause: %d | state=%s | interval=%u min", (int)cause, state_str(g_state), g_interval_min);

    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        uint8_t st=0;
        (void)ds3231_read_status(&st);
        ESP_LOGI(TAG, "[ALARM] woke; A1F=%u", (unsigned)(st & 1U));
        (void)ds3231_clear_a1f();
        vTaskDelay(pdMS_TO_TICKS(2));
        uint8_t nh=0, nm=0;
        (void)ensure_next_alarm(g_interval_min, &nh, &nm);
    }

    // Behavior
    if (g_state == STATE_UNPAIRED) {
        ESP_LOGI(TAG, "[FLOW] UNPAIRED → broadcasting discover");
        send_discover();
        vTaskDelay(pdMS_TO_TICKS(1500)); // allow response window
    }

    if (g_state == STATE_DEPLOYED) {
        ESP_LOGI(TAG, "[FLOW] DEPLOYED → send data, arm next alarm, sleep");
        send_sensor();
        uint8_t h=0, m=0;
        esp_err_t ok = ensure_next_alarm(g_interval_min, &h, &m);
        if (ok != ESP_OK){
            ESP_LOGW(TAG, "[A1] arm failed, falling back to every minute");
            ds3231_every_minute();
        }
        enable_alarm_gpio_wakeup();
        go_deep_sleep(); // never returns
    }

    ESP_LOGI(TAG, "[FLOW] Not deployed yet → idle 5s");
    vTaskDelay(pdMS_TO_TICKS(5000));
}
