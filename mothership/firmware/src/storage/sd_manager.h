#pragma once
#include "FS.h"
#include "SD.h"
#include "SPI.h"

void setupSD();
bool logCSVRow(const String& row);
bool createCSVHeader();
String getCSVStats();
