#pragma once

#include <Arduino.h>
#include "command_dispatcher.h"

static constexpr uint8_t BACKEND_CONTROL_PROTOCOL_VERSION = 2;
static constexpr uint8_t BACKEND_MAX_COMMANDS_PER_RESPONSE = 8;

struct BackendCommandApplyResult {
  CommandResult command{};
  bool durable = false;
  bool applied = false;
  uint16_t wireConfigVersion = 0;
};

using BackendCommandExecutor = BackendCommandApplyResult (*)(const Command&);
using BackendIntervalExecutor = BackendCommandApplyResult (*)(const Command&);
using BackendCommandResolver = bool (*)(const Command& requested,
                                        Command& resolved,
                                        CmdOutcome& rejection);

enum class BackendIngestStatus : uint8_t {
  LEGACY_NO_ENVELOPE = 0,
  EMPTY,
  PROCESSED,
  REMOTE_DISABLED,
  MALFORMED,
  UNSUPPORTED_PROTOCOL,
  TOO_LARGE,
  PERSIST_FAILED,
};

enum class BackendIngestRejection : uint8_t {
  NONE = 0,
  RESPONSE_TOO_LARGE,
  JSON_NOT_FOUND,
  JSON_PARSE_FAILED,
  ENVELOPE_INVALID,
  PROTOCOL_UNSUPPORTED,
  COMMAND_LIMIT,
  SEQUENCE_ORDER,
  CURSOR_INVALID,
  REMOTE_DISABLED,
  CALLBACK_MISSING,
  COMMAND_ID_INVALID,
  COMMAND_SCHEMA_INVALID,
  MIXED_GLOBAL_BATCH,
  DISPATCH_PERSIST_FAILED,
  DESIRED_PERSIST_FAILED,
  CURSOR_PERSIST_FAILED,
  RELEASE_ID_INVALID,
  OTA_ALREADY_PENDING,
};

const char* backendIngestStatusStr(BackendIngestStatus status);
const char* backendIngestRejectionStr(BackendIngestRejection rejection);

struct BackendIngestResult {
  BackendIngestStatus status = BackendIngestStatus::LEGACY_NO_ENVELOPE;
  uint8_t commandCount = 0;
  uint8_t processedCount = 0;
  uint8_t replayedCount = 0;
  uint8_t rejectedCount = 0;
  uint32_t serverTimeUnix = 0;
  uint32_t responseNextCursor = 0;
  uint32_t initialStateRevision = 0;
  uint32_t persistedCursor = 0;
  BackendIngestRejection rejection = BackendIngestRejection::NONE;
};

// Load the checksummed A/B control-state record. Defaults are cursor=0 and
// remoteManagementEnabled=false when no valid record exists.
void     backendControlInit();
uint32_t backendControlLastCommandCursor();
bool     backendControlRemoteManagementEnabled();
bool     backendControlSetRemoteManagementEnabled(bool enabled);
void     backendControlResetForTest();
bool     backendControlRecordDiagnostics(const BackendIngestResult& result,
                                         uint32_t responseBytes,
                                         uint32_t timestampUnix);

// Canonical serializer for both local GET /api/control and cloud status.control.
String backendControlStatusJson();

// Parse the optional command envelope from either a pure JSON body or the raw
// modem HTTP/URC response. Data-upload durability is intentionally external;
// callers invoke this only after handling a successful HTTP 200 upload.
// Executor for a lone CMD_DEPLOY_RELEASE command: durably stage the requested
// releaseId (via dispatcherSubmit for CAS/idempotency + the OTA release store),
// returning durable/applied like the other executors. Optional — a nullptr
// simply rejects DEPLOY_RELEASE commands as unsupported on this build.
using BackendReleaseExecutor = BackendCommandApplyResult (*)(const Command&);

BackendIngestResult backendIngestUploadResponse(
    const String& responseBody, uint32_t rtcNowUnix, bool rtcTrustworthy,
    BackendCommandResolver resolver, BackendCommandExecutor executor,
    BackendIntervalExecutor intervalExecutor = nullptr,
    BackendReleaseExecutor releaseExecutor = nullptr);
