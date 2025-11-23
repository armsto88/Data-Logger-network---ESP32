#include "sd_manager.h"
#include "config.h"

// Desired CSV header matching espnow_manager:
// Timestamp,Node_ID,Node_Name,MAC_Address,Sensor_Type,Value
static const char* CSV_HEADER =
    "timestamp,node_id,node_name,mac,event_type,sensor_type,value,meta";

// Forward decl for internal helper
static bool ensureCSVHeader();

// Forward decl for exported function (matches header)
bool createCSVHeader();

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

    // Ensure CSV exists and has the correct header
    if (!ensureCSVHeader()) {
        Serial.println("⚠️ Failed to ensure CSV header; logging may not work as expected");
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

    uint64_t cardSize = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);
}

bool logCSVRow(const String& row) {
    File file = SD.open("/datalog.csv", FILE_APPEND);
    if (!file) {
        Serial.println("Failed to open datalog.csv for writing");
        return false;
    }

    if (file.println(row)) {
        Serial.println("Data logged: " + row);
        file.close();
        return true;
    } else {
        Serial.println("⚠️ Failed to write CSV row");
        file.close();
        return false;
    }
}

// Exported function – matches sd_manager.h
bool createCSVHeader() {
    File file = SD.open("/datalog.csv", FILE_WRITE);
    if (!file) {
        Serial.println("Failed to create CSV file");
        return false;
    }

    file.println(CSV_HEADER);
    file.close();

    Serial.println("✅ CSV header created:");
    Serial.println(CSV_HEADER);
    return true;
}

// Internal helper: ensure datalog.csv exists and has the expected header.
// If it exists with an old/mismatched header, it is renamed to
// /datalog_legacy.csv and a new file is created.
static bool ensureCSVHeader() {
    if (!SD.exists("/datalog.csv")) {
        Serial.println("datalog.csv not found, creating with new header");
        return createCSVHeader();
    }

    File file = SD.open("/datalog.csv", FILE_READ);
    if (!file) {
        Serial.println("Failed to open existing datalog.csv for header check");
        return false;
    }

    // Read first line
    String firstLine = file.readStringUntil('\n');
    firstLine.trim();
    file.close();

    if (firstLine == String(CSV_HEADER)) {
        Serial.println("✅ Existing datalog.csv has correct header");
        return true;
    }

    // Header mismatch → migrate old file
    Serial.println("⚠️ Existing datalog.csv header mismatch:");
    Serial.print("   Found:    '"); Serial.print(firstLine);   Serial.println("'");
    Serial.print("   Expected: '"); Serial.print(CSV_HEADER);  Serial.println("'");

    // Try to rename old file
    if (SD.exists("/datalog_legacy.csv")) {
        Serial.println("⚠️ datalog_legacy.csv already exists, will overwrite it");
        SD.remove("/datalog_legacy.csv");
    }

    if (!SD.rename("/datalog.csv", "/datalog_legacy.csv")) {
        Serial.println("❌ Failed to rename old datalog.csv → datalog_legacy.csv");
        return false;
    }

    Serial.println("↪ Renamed old datalog.csv to datalog_legacy.csv");
    return createCSVHeader();
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
