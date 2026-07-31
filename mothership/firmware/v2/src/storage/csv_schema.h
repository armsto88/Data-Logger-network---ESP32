#pragma once

#include <stddef.h>

// Fixed CSV schemas used by the flash logger and upload queue. Keep the legacy
// headers so a firmware upgrade can drain already-queued rows without deleting
// or shifting them. New rows always use the current shape; old rows simply have
// no values for the trailing columns added since they were written.
//
// Columns are only ever APPENDED, never reordered or inserted, so every existing
// column index stays valid and a short row is still positionally correct. The
// JSON builder stops at the row's own field count, so a legacy row uploads
// cleanly with the newer keys simply absent.
//
// 25 -> 30: the five extended AS7341 outputs.
// 30 -> 31: deploymentEpoch — which deployment the sample was taken under.
//           0 means legacy/unresolved and is never a valid deployed epoch.

static constexpr const char* kLegacyCSVHeader25 =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2";

static constexpr const char* kLegacyCSVHeader30 =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2,"
    "spectral_clear,spectral_nir,spectral_gain,spectral_integration_ms,"
    "spectral_saturated";

static constexpr const char* kCurrentCSVHeader31 =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2,"
    "spectral_clear,spectral_nir,spectral_gain,spectral_integration_ms,"
    "spectral_saturated,deploymentEpoch";

static constexpr size_t kLegacyCSVColumnCount   = 25;
static constexpr size_t kLegacyCSVColumnCount30 = 30;
static constexpr size_t kCurrentCSVColumnCount  = 31;
