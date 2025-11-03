#include "sd_manager.h"
#include <SD.h>

void setupSD() {
    // Initialize SD card
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("SD Card failed to initialize!");
    } else {
        Serial.println("SD Card initialized.");
    }
}

bool logCSVRow(const String& row) {
    // Append row to CSV file on SD card
    File file = SD.open("/datalog.csv", FILE_APPEND);
    if (!file) return false;
    file.println(row);
    file.close();
    return true;
}
