#pragma once

#include <stddef.h>

// Fixed CSV schemas used by the flash logger and upload queue. Keep the
// legacy header so a firmware upgrade can drain already-queued 25-column rows
// without deleting or shifting them. New rows always use the 30-column shape;
// old rows simply have no values for the five trailing AS7341 fields.

static constexpr const char* kLegacyCSVHeader25 =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2";

static constexpr const char* kCurrentCSVHeader30 =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2,"
    "spectral_clear,spectral_nir,spectral_gain,spectral_integration_ms,"
    "spectral_saturated";

static constexpr size_t kLegacyCSVColumnCount = 25;
static constexpr size_t kCurrentCSVColumnCount = 30;
