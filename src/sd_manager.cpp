#include "sd_manager.h"
#include "config.h"

void setupSD() {
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("SD Card initialization failed!");
        return;
    }
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        return;
    }
    
    Serial.println("SD Card initialized successfully");
    
    // Create CSV header if file doesn't exist
    if (!SD.exists("/datalog.csv")) {
        createCSVHeader();
    }
    
    // Print card info
    Serial.print("SD Card Type: ");
    if (cardType == CARD_MMC) {
        Serial.println("MMC");
    } else if (cardType == CARD_SD) {
        Serial.println("SDSC");
    } else if (cardType == CARD_SDHC) {
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }
    
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);
}

bool logCSVRow(const String& row) {
    File file = SD.open("/datalog.csv", FILE_APPEND);
    if (!file) {
        Serial.println("Failed to open file for writing");
        return false;
    }
    
    file.println(row);
    file.close();
    
    Serial.println("Data logged: " + row);
    return true;
}

bool createCSVHeader() {
    File file = SD.open("/datalog.csv", FILE_WRITE);
    if (!file) {
        Serial.println("Failed to create CSV file");
        return false;
    }
    
    file.println("Timestamp,Node_ID,MAC_Address,Sensor_Type,Value");
    file.close();
    
    Serial.println("✅ CSV header created");
    return true;
}

String getCSVStats() {
    if (!SD.exists("/datalog.csv")) {
        return "No data file found";
    }
    
    File file = SD.open("/datalog.csv");
    if (!file) {
        return "Cannot read data file";
    }
    
    int lineCount = 0;
    while (file.available()) {
        if (file.read() == '\n') {
            lineCount++;
        }
    }
    file.close();
    
    // Subtract 1 for header line
    int dataLines = lineCount > 0 ? lineCount - 1 : 0;
    return String(dataLines) + " data records";
}