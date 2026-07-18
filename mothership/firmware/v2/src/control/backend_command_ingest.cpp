#include "control/backend_command_ingest.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>

namespace {

static constexpr const char* kStateNamespace = "backend_ctrl";
static constexpr const char* kStateA = "state_a";
static constexpr const char* kStateB = "state_b";
static constexpr uint32_t kStateMagic = 0x464D4354UL;  // FMCT
static constexpr uint16_t kStateVersion = 1;
static constexpr size_t kMaxRawResponseBytes = 12288;
static constexpr uint32_t kMinTrustedUnix = 1704067200UL;  // 2024-01-01
static constexpr uint32_t kMaxTrustedUnix = 4102444800UL;  // 2100-01-01

struct ControlStateRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
  uint32_t lastCommandCursor;
  uint8_t remoteManagementEnabled;
  uint8_t reserved[3];
  uint32_t checksum;
};

static ControlStateRecord gState{};

uint32_t checksumFor(ControlStateRecord record) {
  record.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(record); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool validRecord(const ControlStateRecord& record) {
  return record.magic == kStateMagic && record.version == kStateVersion &&
         record.size == sizeof(ControlStateRecord) &&
         record.remoteManagementEnabled <= 1 &&
         record.checksum == checksumFor(record);
}

bool generationNewer(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

bool readRecord(Preferences& prefs, const char* key, ControlStateRecord& out) {
  if (prefs.getBytesLength(key) != sizeof(out)) return false;
  return prefs.getBytes(key, &out, sizeof(out)) == sizeof(out) &&
         validRecord(out);
}

bool persistState(uint32_t cursor, bool enabled) {
  ControlStateRecord candidate{};
  candidate.magic = kStateMagic;
  candidate.version = kStateVersion;
  candidate.size = sizeof(candidate);
  candidate.generation = gState.generation + 1U;
  candidate.lastCommandCursor = cursor;
  candidate.remoteManagementEnabled = enabled ? 1 : 0;
  candidate.checksum = checksumFor(candidate);
  const char* key = (candidate.generation & 1U) ? kStateA : kStateB;

  Preferences prefs;
  if (!prefs.begin(kStateNamespace, false)) return false;
  const bool wrote = prefs.putBytes(key, &candidate, sizeof(candidate)) ==
                     sizeof(candidate);
  prefs.end();
  if (!wrote || !prefs.begin(kStateNamespace, true)) return false;
  ControlStateRecord verify{};
  const bool verified = readRecord(prefs, key, verify) &&
                        memcmp(&verify, &candidate, sizeof(candidate)) == 0;
  prefs.end();
  if (!verified) return false;
  gState = candidate;
  return true;
}

bool advanceCursor(uint32_t sequence) {
  if (sequence <= gState.lastCommandCursor) return true;
  return persistState(sequence, gState.remoteManagementEnabled != 0);
}

bool plausibleUnix(uint32_t value) {
  return value >= kMinTrustedUnix && value < kMaxTrustedUnix;
}

bool readU32(JsonVariantConst value, uint32_t& out) {
  if (!value.is<uint32_t>()) return false;
  out = value.as<uint32_t>();
  return true;
}

bool readU16(JsonVariantConst value, uint16_t& out) {
  uint32_t n = 0;
  if (!readU32(value, n) || n > UINT16_MAX) return false;
  out = static_cast<uint16_t>(n);
  return true;
}

bool readU8(JsonVariantConst value, uint8_t& out) {
  uint32_t n = 0;
  if (!readU32(value, n) || n > UINT8_MAX) return false;
  out = static_cast<uint8_t>(n);
  return true;
}

bool validDashboardCommandId(const char* id) {
  if (!id || strlen(id) != CMD_ID_LEN - 1 || id[0] != 'D') return false;
  for (size_t i = 1; i < CMD_ID_LEN - 1; ++i) {
    const char c = id[i];
    const bool base64url = (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!base64url) return false;
  }
  return true;
}

bool extractJsonObject(const String& raw, String& json) {
  if (raw.length() == 0 || raw.length() > kMaxRawResponseBytes) return false;
  const int start = raw.indexOf('{');
  if (start < 0) return false;

  bool inString = false;
  bool escaped = false;
  int depth = 0;
  for (size_t i = static_cast<size_t>(start); i < raw.length(); ++i) {
    const char c = raw[i];
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') inString = true;
    else if (c == '{') depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) {
        json = raw.substring(start, i + 1);
        return true;
      }
      if (depth < 0) return false;
    }
  }
  return false;
}

bool envelopePresent(JsonDocument& doc) {
  return doc["controlProtocolVersion"].is<uint32_t>() ||
         doc["commands"].is<JsonArrayConst>() ||
         doc["nextCursor"].is<uint32_t>();
}

struct StagedCommand {
  uint32_t sequence = 0;
  DispatchBatchItem dispatch{};
};

}  // namespace

const char* backendIngestStatusStr(BackendIngestStatus status) {
  switch (status) {
    case BackendIngestStatus::LEGACY_NO_ENVELOPE: return "LEGACY_NO_ENVELOPE";
    case BackendIngestStatus::EMPTY: return "EMPTY";
    case BackendIngestStatus::PROCESSED: return "PROCESSED";
    case BackendIngestStatus::REMOTE_DISABLED: return "REMOTE_DISABLED";
    case BackendIngestStatus::MALFORMED: return "MALFORMED";
    case BackendIngestStatus::UNSUPPORTED_PROTOCOL: return "UNSUPPORTED_PROTOCOL";
    case BackendIngestStatus::TOO_LARGE: return "TOO_LARGE";
    case BackendIngestStatus::PERSIST_FAILED: return "PERSIST_FAILED";
    default: return "UNKNOWN";
  }
}

void backendControlInit() {
  gState = {};
  gState.magic = kStateMagic;
  gState.version = kStateVersion;
  gState.size = sizeof(gState);

  Preferences prefs;
  if (!prefs.begin(kStateNamespace, true)) return;
  ControlStateRecord a{}, b{};
  const bool aValid = readRecord(prefs, kStateA, a);
  const bool bValid = readRecord(prefs, kStateB, b);
  prefs.end();
  if (aValid && bValid) gState = generationNewer(b.generation, a.generation) ? b : a;
  else if (aValid) gState = a;
  else if (bValid) gState = b;
}

uint32_t backendControlLastCommandCursor() {
  return gState.lastCommandCursor;
}

bool backendControlRemoteManagementEnabled() {
  return gState.remoteManagementEnabled != 0;
}

bool backendControlSetRemoteManagementEnabled(bool enabled) {
  if (enabled == backendControlRemoteManagementEnabled()) return true;
  return persistState(gState.lastCommandCursor, enabled);
}

void backendControlResetForTest() {
  Preferences prefs;
  if (prefs.begin(kStateNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  gState = {};
  gState.magic = kStateMagic;
  gState.version = kStateVersion;
  gState.size = sizeof(gState);
}

String backendControlStatusJson() {
  CommandResult results[CMD_MAX_RESULTS]{};
  const uint8_t count = dispatcherRecentResults(results, CMD_MAX_RESULTS);
  String json;
  json.reserve(180 + count * 80);
  json += "{\"controlProtocolVersion\":";
  json += String(BACKEND_CONTROL_PROTOCOL_VERSION);
  json += ",\"stateRevision\":";
  json += String(dispatcherRevision());
  json += ",\"lastCommandCursor\":";
  json += String(gState.lastCommandCursor);
  json += ",\"lastChangeSource\":\"";
  json += dispatcherLastChangeSource() == SRC_DASHBOARD ? "DASHBOARD" : "LOCAL_UI";
  json += "\",\"remoteManagementEnabled\":";
  json += backendControlRemoteManagementEnabled() ? "true" : "false";
  json += ",\"results\":[";
  for (uint8_t i = 0; i < count; ++i) {
    if (i) json += ',';
    json += "{\"cmdId\":\"";
    json += results[i].cmdId;
    json += "\",\"outcome\":\"";
    json += cmdOutcomeStr(results[i].outcome);
    json += "\",\"revision\":";
    json += String(results[i].assignedRevision);
    json += '}';
  }
  json += "]}";
  return json;
}

BackendIngestResult backendIngestUploadResponse(
    const String& responseBody, uint32_t rtcNowUnix, bool rtcTrustworthy,
    BackendCommandResolver resolver, BackendCommandExecutor executor) {
  BackendIngestResult result{};
  // This snapshot is the compare-and-set authority for the entire response.
  // Individual accepted commands may advance the live revision later, but no
  // command in this envelope is compared against that moving value.
  result.initialStateRevision = dispatcherRevision();
  result.persistedCursor = gState.lastCommandCursor;
  if (responseBody.length() > kMaxRawResponseBytes) {
    result.status = BackendIngestStatus::TOO_LARGE;
    return result;
  }

  String json;
  if (!extractJsonObject(responseBody, json)) {
    // The deployed backend historically returned an empty/plain-text body.
    // That remains a harmless success response, not a control parse failure.
    result.status = responseBody.indexOf('{') < 0
        ? BackendIngestStatus::LEGACY_NO_ENVELOPE
        : BackendIngestStatus::MALFORMED;
    return result;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(
      doc, json.c_str(), json.length(), DeserializationOption::NestingLimit(8));
  if (error || !doc.is<JsonObject>()) {
    result.status = BackendIngestStatus::MALFORMED;
    return result;
  }
  if (!envelopePresent(doc)) {
    result.status = BackendIngestStatus::LEGACY_NO_ENVELOPE;
    return result;
  }

  uint32_t protocol = 0;
  if (!readU32(doc["controlProtocolVersion"], protocol)) {
    result.status = BackendIngestStatus::MALFORMED;
    return result;
  }
  if (protocol != BACKEND_CONTROL_PROTOCOL_VERSION) {
    result.status = BackendIngestStatus::UNSUPPORTED_PROTOCOL;
    return result;
  }
  if (!readU32(doc["serverTimeUnix"], result.serverTimeUnix) ||
      !plausibleUnix(result.serverTimeUnix) ||
      !readU32(doc["nextCursor"], result.responseNextCursor) ||
      !doc["commands"].is<JsonArrayConst>()) {
    result.status = BackendIngestStatus::MALFORMED;
    return result;
  }

  JsonArrayConst commands = doc["commands"].as<JsonArrayConst>();
  if (commands.size() > BACKEND_MAX_COMMANDS_PER_RESPONSE) {
    result.status = BackendIngestStatus::TOO_LARGE;
    return result;
  }
  result.commandCount = static_cast<uint8_t>(commands.size());

  // Validate ordering and nextCursor before causing any mutation.
  uint32_t previousSequence = 0;
  bool first = true;
  for (JsonObjectConst item : commands) {
    uint32_t sequence = 0;
    if (!readU32(item["sequence"], sequence) || sequence == 0 ||
        (!first && sequence <= previousSequence)) {
      result.status = BackendIngestStatus::MALFORMED;
      return result;
    }
    first = false;
    previousSequence = sequence;
  }
  if ((!first && result.responseNextCursor < previousSequence) ||
      result.responseNextCursor < gState.lastCommandCursor) {
    result.status = BackendIngestStatus::MALFORMED;
    return result;
  }

  if (commands.size() == 0) {
    result.status = BackendIngestStatus::EMPTY;
    return result;
  }
  if (!backendControlRemoteManagementEnabled()) {
    result.status = BackendIngestStatus::REMOTE_DISABLED;
    return result;
  }
  if (!resolver || !executor) {
    result.status = BackendIngestStatus::MALFORMED;
    return result;
  }

  const uint32_t validationNow = plausibleUnix(result.serverTimeUnix)
      ? result.serverTimeUnix
      : (rtcTrustworthy ? rtcNowUnix : 0);

  Serial.printf("[CONTROL] envelope protocol=%lu commands=%u nextCursor=%lu initialRevision=%lu\n",
                static_cast<unsigned long>(protocol),
                static_cast<unsigned>(result.commandCount),
                static_cast<unsigned long>(result.responseNextCursor),
                static_cast<unsigned long>(result.initialStateRevision));

  StagedCommand staged[BACKEND_MAX_COMMANDS_PER_RESPONSE]{};
  uint8_t stagedCount = 0;
  char seenIds[BACKEND_MAX_COMMANDS_PER_RESPONSE][CMD_ID_LEN]{};
  uint8_t seenIdCount = 0;

  // Parse and resolve the complete set before the dispatcher or node registry
  // is allowed to change. Terminal semantic failures become durable outcomes;
  // malformed identity/order/envelope failures abort the whole response.
  for (JsonObjectConst item : commands) {
    const uint32_t sequence = item["sequence"].as<uint32_t>();
    const char* commandId = item["commandId"].as<const char*>();
    if (!validDashboardCommandId(commandId)) {
      result.status = BackendIngestStatus::MALFORMED;
      return result;
    }
    for (uint8_t i = 0; i < seenIdCount; ++i) {
      if (strncmp(seenIds[i], commandId, CMD_ID_LEN) == 0) {
        result.status = BackendIngestStatus::MALFORMED;
        return result;
      }
    }
    strlcpy(seenIds[seenIdCount++], commandId, CMD_ID_LEN);

    // A fully persisted sequence is already acknowledged even if a stale
    // response is replayed after the bounded result ring rotates.
    if (sequence <= gState.lastCommandCursor) {
      ++result.replayedCount;
      continue;
    }

    StagedCommand& entry = staged[stagedCount++];
    entry.sequence = sequence;
    Command& requested = entry.dispatch.command;
    strlcpy(requested.cmdId, commandId, CMD_ID_LEN);
    requested.source = SRC_DASHBOARD;
    entry.dispatch.outcome = OUT_INVALID;

    const char* type = item["type"].as<const char*>();
    uint32_t expectedRevision = 0, issuedAt = 0, expiresAt = 0;
    const char* nodeId = item["target"]["nodeId"].as<const char*>();
    const bool nodeIdShapeOk = nodeId && nodeId[0] &&
                               strlen(nodeId) < CMD_NODEID_LEN;
    const bool baseFieldsOk = type &&
        strcmp(type, "SET_NODE_CONFIG") == 0 &&
        readU32(item["expectedStateRevision"], expectedRevision) &&
        readU32(item["issuedAtUnix"], issuedAt) && plausibleUnix(issuedAt) &&
        readU32(item["expiresAtUnix"], expiresAt) && plausibleUnix(expiresAt) &&
        expiresAt > issuedAt && nodeIdShapeOk &&
        item["payload"].is<JsonObjectConst>();
    if (!baseFieldsOk) continue;

    requested.type = CMD_SET_NODE_CONFIG;
    requested.expectedRevision = expectedRevision;
    strlcpy(requested.payload.nodeId, nodeId, CMD_NODEID_LEN);
    JsonObjectConst payload = item["payload"].as<JsonObjectConst>();

    uint8_t targetState = 0;
    if (!readU8(payload["targetState"], targetState) ||
        (targetState != 0 && targetState != 2 && targetState != 3)) continue;
    requested.payload.targetState = targetState;
    requested.configFields |= CFG_FIELD_TARGET_STATE;

    if (!payload["wakeIntervalMin"].isNull()) {
      uint8_t wake = 0;
      if (!readU8(payload["wakeIntervalMin"], wake) || wake < 1 || wake > 240)
        continue;
      requested.payload.wakeIntervalMin = wake;
      requested.configFields |= CFG_FIELD_WAKE_INTERVAL;
    }
    if (!payload["sensorMask"].isNull()) {
      uint16_t sensorMask = 0;
      if (!readU16(payload["sensorMask"], sensorMask)) continue;
      requested.payload.sensorMask = sensorMask;
      requested.configFields |= CFG_FIELD_SENSOR_MASK;
    }

    const bool timeOk = validationNow == 0 ||
                        (validationNow <= expiresAt &&
                         issuedAt <= validationNow + 300UL);
    if (!timeOk) continue;

    // Idempotency takes precedence on retry: the durable original outcome is
    // replayed and its stored payload is used for any registry repair.
    if (dispatcherKnownCmd(commandId)) {
      entry.dispatch.outcome = OUT_ACCEPTED;
      continue;
    }
    if (expectedRevision != result.initialStateRevision) {
      entry.dispatch.outcome = OUT_REVISION_CONFLICT;
      continue;
    }

    Command resolved{};
    CmdOutcome rejection = OUT_INVALID;
    if (!resolver(requested, resolved, rejection)) {
      entry.dispatch.outcome = rejection == OUT_REVISION_CONFLICT
          ? OUT_REVISION_CONFLICT : OUT_INVALID;
      continue;
    }
    entry.dispatch.command = resolved;
    entry.dispatch.outcome = OUT_ACCEPTED;
  }

  // A response may not assign two fresh desired states to one node. This is a
  // terminal per-command validation failure, so both IDs receive an outcome
  // while unrelated nodes in the same response remain eligible.
  for (uint8_t i = 0; i < stagedCount; ++i) {
    if (staged[i].dispatch.outcome != OUT_ACCEPTED ||
        dispatcherKnownCmd(staged[i].dispatch.command.cmdId)) continue;
    for (uint8_t j = i + 1; j < stagedCount; ++j) {
      if (staged[j].dispatch.outcome == OUT_ACCEPTED &&
          !dispatcherKnownCmd(staged[j].dispatch.command.cmdId) &&
          strncmp(staged[i].dispatch.command.payload.nodeId,
                  staged[j].dispatch.command.payload.nodeId,
                  CMD_NODEID_LEN) == 0) {
        staged[i].dispatch.outcome = OUT_INVALID;
        staged[j].dispatch.outcome = OUT_INVALID;
      }
    }
  }
  for (uint8_t i = 0; i < stagedCount; ++i) {
    Serial.printf("[CONTROL] staged seq=%lu id=%s node=%s decision=%s\n",
                  static_cast<unsigned long>(staged[i].sequence),
                  staged[i].dispatch.command.cmdId,
                  staged[i].dispatch.command.payload.nodeId,
                  cmdOutcomeStr(staged[i].dispatch.outcome));
  }

  if (stagedCount == 0) {
    result.status = BackendIngestStatus::PROCESSED;
    return result;
  }

  DispatchBatchItem batch[BACKEND_MAX_COMMANDS_PER_RESPONSE]{};
  CommandResult decisions[BACKEND_MAX_COMMANDS_PER_RESPONSE]{};
  for (uint8_t i = 0; i < stagedCount; ++i) batch[i] = staged[i].dispatch;
  if (!dispatcherSubmitBatch(batch, stagedCount,
                             result.initialStateRevision, decisions)) {
    result.status = BackendIngestStatus::PERSIST_FAILED;
    return result;
  }
  Serial.printf("[CONTROL] batch dispatcher durable count=%u revision=%lu\n",
                static_cast<unsigned>(stagedCount),
                static_cast<unsigned long>(dispatcherRevision()));

  // The batch decision is durable. Now persist/verify each accepted desired
  // node configuration. Cursor-loss replay uses the dispatcher's stored
  // payload, so a reset during this loop repairs rather than increments twice.
  for (uint8_t i = 0; i < stagedCount; ++i) {
    CommandResult stored{};
    if (!dispatcherResultFor(batch[i].command.cmdId, &stored)) {
      result.status = BackendIngestStatus::PERSIST_FAILED;
      return result;
    }
    if (decisions[i].outcome == OUT_REPLAY) {
      ++result.replayedCount;
    } else if (stored.outcome == OUT_INVALID ||
               stored.outcome == OUT_REVISION_CONFLICT) {
      ++result.rejectedCount;
    }

    if (stored.outcome == OUT_ACCEPTED) {
      Command applyCommand = batch[i].command;
      if (decisions[i].outcome == OUT_REPLAY &&
          !dispatcherCommandForResult(batch[i].command.cmdId, &applyCommand)) {
        result.status = BackendIngestStatus::PERSIST_FAILED;
        return result;
      }
      BackendCommandApplyResult applied = executor(applyCommand);
      if (!applied.durable || !applied.applied ||
          !dispatcherEnsureDurable()) {
        result.status = BackendIngestStatus::PERSIST_FAILED;
        return result;
      }
      Serial.printf("[CONTROL] desired durable seq=%lu node=%s version=%u\n",
                    static_cast<unsigned long>(staged[i].sequence),
                    applyCommand.payload.nodeId,
                    static_cast<unsigned>(applied.wireConfigVersion));
    }
    ++result.processedCount;
  }

  const uint32_t oldCursor = gState.lastCommandCursor;
  const uint32_t committedSequence = staged[stagedCount - 1].sequence;
  if (!advanceCursor(committedSequence)) {
    result.status = BackendIngestStatus::PERSIST_FAILED;
    return result;
  }
  result.persistedCursor = gState.lastCommandCursor;
  Serial.printf("[CONTROL] cursor durable %lu->%lu\n",
                static_cast<unsigned long>(oldCursor),
                static_cast<unsigned long>(result.persistedCursor));

  result.status = BackendIngestStatus::PROCESSED;
  return result;
}
