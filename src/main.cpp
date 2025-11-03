#include "config.h"
#include "rtc_manager.h"
#include "sd_manager.h"
#include "espnow_manager.h"

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    setupRTC();
    setupSD();
    setupESPNOW();
    Serial.println("Mother Ship: Startup complete");
}

void loop() {
    // Main event loop: periodically check for ESP-NOW, log data if received.
    // Placeholder for low-power sleep, RTC interrupt handling, and data logging.
    espnow_loop(); // checks ESP-NOW, processes incoming data
    delay(100);    // Reduce CPU spin (replace with sleep logic in production)
}
