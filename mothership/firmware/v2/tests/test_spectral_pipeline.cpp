#include <Arduino.h>

#include "protocol.h"
#include "storage/csv_schema.h"
#include "storage/flash_logger.h"
#include "storage/json_payload.h"

namespace {

bool expect(bool condition, const char* message) {
  Serial.printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
  return condition;
}

size_t csvColumnCount(const String& row) {
  size_t count = row.length() > 0 ? 1 : 0;
  for (size_t i = 0; i < row.length(); ++i) {
    if (row[i] == ',') ++count;
  }
  return count;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== V2 spectral metadata pipeline regression ===");

  constexpr size_t kReadingCount = 13;
  uint8_t wire[sizeof(node_snapshot_v2_t) + kReadingCount * sizeof(v2_reading_t)]{};
  auto* header = reinterpret_cast<node_snapshot_v2_t*>(wire);
  strncpy(header->command, "NODE_SNAPSHOT2", sizeof(header->command) - 1);
  strncpy(header->nodeId, "PAR_TEST", sizeof(header->nodeId) - 1);
  header->nodeTimestamp = 1783166400UL;
  header->seqNum = 42;
  header->sensorCount = kReadingCount;
  header->protocolVersion = NODE_PROTOCOL_VERSION;

  auto* readings = reinterpret_cast<v2_reading_t*>(wire + sizeof(node_snapshot_v2_t));
  const v2_reading_t fixture[kReadingCount] = {
      {SENSOR_ID_SPECTRAL_415, 1800.0f},
      {SENSOR_ID_SPECTRAL_445, 2600.0f},
      {SENSOR_ID_SPECTRAL_480, 3100.0f},
      {SENSOR_ID_SPECTRAL_515, 4200.0f},
      {SENSOR_ID_SPECTRAL_555, 5100.0f},
      {SENSOR_ID_SPECTRAL_590, 4700.0f},
      {SENSOR_ID_SPECTRAL_630, 3900.0f},
      {SENSOR_ID_SPECTRAL_680, 2500.0f},
      {SENSOR_ID_SPECTRAL_CLEAR, 12000.0f},
      {SENSOR_ID_SPECTRAL_NIR, 6800.0f},
      {SENSOR_ID_SPECTRAL_GAIN, 4.0f},
      {SENSOR_ID_SPECTRAL_ATIME, 50.04f},
      {SENSOR_ID_SPECTRAL_SAT, 0.0f},
  };
  memcpy(readings, fixture, sizeof(fixture));

  bool ok = true;
  DecodedSnapshot decoded{};
  ok &= expect(decodeV2(wire, sizeof(wire), decoded), "V2 wire packet decodes");
  ok &= expect(decoded.readingCount == kReadingCount, "all 13 spectral readings survive decode");
  ok &= expect(decoded.find(SENSOR_ID_SPECTRAL_CLEAR) &&
               *decoded.find(SENSOR_ID_SPECTRAL_CLEAR) == 12000.0f,
               "Clear ID 1109 survives decode");
  ok &= expect(decoded.find(SENSOR_ID_SPECTRAL_NIR) &&
               *decoded.find(SENSOR_ID_SPECTRAL_NIR) == 6800.0f,
               "NIR ID 1110 survives decode");

  String row;
  ok &= expect(formatDecodedSnapshotCSVRow(decoded, row), "decoded snapshot formats as CSV");
  ok &= expect(csvColumnCount(row) == kCurrentCSVColumnCount, "CSV row has 30 columns");
  ok &= expect(row.endsWith("12000.000,6800.000,4.000,50.040,0.000"),
               "CSV columns 25-29 are the five numeric metadata values");

  String chunk = String(kCurrentCSVHeader30) + "\n" + row + "\n";
  JsonPayload json = buildJsonUpload(chunk, 1, "spectral-pipeline-test", nullptr,
                                     header->nodeTimestamp);
  ok &= expect(json.ok && json.rowCount == 1, "JSON payload builds one reading");
  ok &= expect(json.body.indexOf("\"spectral_clear\":12000.000") >= 0,
               "JSON contains numeric spectral_clear");
  ok &= expect(json.body.indexOf("\"spectral_nir\":6800.000") >= 0,
               "JSON contains numeric spectral_nir");
  ok &= expect(json.body.indexOf("\"spectral_gain\":4.000") >= 0,
               "JSON contains numeric spectral_gain");
  ok &= expect(json.body.indexOf("\"spectral_integration_ms\":50.040") >= 0,
               "JSON contains numeric spectral_integration_ms");
  ok &= expect(json.body.indexOf("\"spectral_saturated\":0.000") >= 0,
               "JSON contains numeric spectral_saturated");
  ok &= expect(json.csvBytesConsumed == row.length() + 1,
               "cursor consumption excludes the synthetic CSV header");

  // Reproduce the production multi-chunk boundary: after the first POST, the
  // returned byte count is applied to the physical data portion. The next
  // build must begin exactly at row 2 without skipping into it.
  String secondRow = row;
  secondRow.replace(",42,", ",43,");
  String twoRowChunk = String(kCurrentCSVHeader30) + "\n" + row + "\n" +
                       secondRow + "\n";
  JsonPayload firstChunk = buildJsonUpload(twoRowChunk, 1, "cursor-test", nullptr,
                                           header->nodeTimestamp);
  const uint32_t firstDataOffset = strlen(kCurrentCSVHeader30) + 1;
  const String remainingData = twoRowChunk.substring(
      firstDataOffset + firstChunk.csvBytesConsumed);
  const String secondChunkCsv = String(kCurrentCSVHeader30) + "\n" + remainingData;
  JsonPayload secondChunk = buildJsonUpload(secondChunkCsv, 1, "cursor-test", nullptr,
                                            header->nodeTimestamp);
  ok &= expect(firstChunk.csvBytesConsumed == row.length() + 1,
               "first multi-chunk cursor stops exactly after row 1");
  ok &= expect(secondChunk.ok && secondChunk.rowCount == 1 &&
               secondChunk.body.indexOf("\"seqNum\":43") >= 0,
               "second multi-chunk build starts at the complete next row");

  // The deployed queue already begins with a fragment left by the old
  // over-advancing purge. It must be consumed locally without being emitted,
  // while the following complete row remains uploadable.
  const String truncated = "3.551,23.300,58.000,1.000,2.000,3.000,4.000\n";
  const String recoveryCsv = String(kCurrentCSVHeader30) + "\n" + truncated +
                             row + "\n";
  JsonPayload recovered = buildJsonUpload(recoveryCsv, 100, "recovery-test", nullptr,
                                          header->nodeTimestamp);
  ok &= expect(recovered.ok && recovered.rowCount == 1 &&
               recovered.body.indexOf("\"nodeId\":\"PAR_TEST\"") >= 0,
               "truncated leading row is skipped and the next full row uploads");
  ok &= expect(recovered.csvBytesConsumed == truncated.length() + row.length() + 1,
               "recovery cursor consumes the fragment and complete uploaded row");

  Serial.printf("=== RESULT: %s ===\n", ok ? "PASS" : "FAIL");
}

void loop() {
  delay(5000);
}
