# Native App Full Concept and Schemas (OpenClaw Handoff)

Status: Design specification for implementation handoff
Scope: Complete native app concept for mothership and node operations, including schema definitions, UX baseline, and build recommendations

Audience:
- Builder LLM / implementation agent
- Engineering handoff reviewers
- Firmware + app integrators

Document intent:
- Be implementation-directive, not just descriptive.
- Minimize ambiguity for autonomous code generation.
- Keep app design aligned with real firmware behavior and constraints.

## 0. Orchestrator + PM Handoff Layer (Critical)

This document is also the master handoff to an agent orchestrator and PM workflow.

The orchestrator must treat this as:
- Product requirements source.
- Technical integration contract.
- Delivery governance checklist.

The PM must be able to use this document alone to:
- Scope workstreams.
- Assign researcher/builder/reviewer agent tasks.
- Track readiness and completion without requiring additional architecture interviews.

### 0.1 Required outcomes for the orchestrator

The orchestrator must coordinate agent outputs that produce all of the following:
- Finalized product requirements package.
- Technical architecture package.
- Build implementation package.
- Validation and review package.
- PM tracking package (risks, assumptions, decisions, open issues).

### 0.2 Agent roles

Minimum role set:
- Researcher agents:
  - Clarify unknowns, investigate platform constraints, and produce evidence-backed recommendations.
- Builder agents:
  - Implement app architecture, features, and tests according to this specification.
- Reviewer agents:
  - Perform code quality, security, and requirement-parity reviews.

Optional role set:
- Integration agent:
  - Focus on app <-> firmware schema parity and protocol validation.
- QA automation agent:
  - Expand and maintain automated test coverage.

### 0.3 Non-negotiable governance rules

- No schema drift from firmware source of truth without explicit decision log entry.
- No silent assumption changes.
- Every changed requirement must update this document and the decision log.
- Every completed build task must include test evidence.
- Reviewer sign-off is required before task closure.

### 0.4 Definition of Ready (DoR) for any implementation task

A task is ready only when all are present:
- Requirement reference section in this document.
- Input and expected output artifacts defined.
- Acceptance criteria stated and testable.
- Dependencies and coupling points identified.
- Open questions either resolved or explicitly documented.

### 0.5 Definition of Done (DoD) for any implementation task

A task is done only when all are true:
- Implementation merged and builds cleanly.
- Required tests pass.
- Reviewer findings resolved or accepted with documented rationale.
- Documentation updated.
- PM status package updated (decision/risk/open-issue deltas).

## 1. Project Overview

This native app is the primary operator interface for a distributed ESP32 environmental logging system:
- Mothership: central controller and data concentrator
- Nodes: sensor devices that sample, queue locally, and sync to mothership
- Transport split:
  - BLE between app and mothership for control and status
  - ESP-NOW between nodes and mothership for sensor transport
  - WiFi AP on mothership as optional fallback and diagnostics path

The app replaces day-to-day dependence on the web UI while preserving feature parity and operational safety.

Background context:
- System evolved from single-sensor node assumptions to a standardized multi-sensor node profile.
- Node firmware now treats each channel as a stable logical sensor slot with:
  - sensorId (stable machine key)
  - sensorType (category)
  - sensorLabel (human-readable channel name)
  - value
  - nodeTimestamp
  - qualityFlags
- Node queues samples locally and flushes during sync windows.
- Mothership remains web-capable during migration, but app is target primary operator interface.

Standard node profile for v1 app assumptions:
- SHT41 (air temperature + humidity)
- AS734x (PAR)
- Ultrasonic wind input (when available)
- 2x VH-5 probes via ADS1115 (A0-A3): soil moisture x2 + soil temperature x2
- 2x AUX I2C expansion ports for optional add-on sensors

## 2. Product Goals

- Full operational parity with current web workflow
- Reliable field operation with intermittent connectivity
- Fast critical actions: discover, pair, deploy, schedule, sync, export
- Clear status visibility across fleet and per-node health
- Low cognitive load for technical field operators
- Strong command traceability and deterministic behavior

## 3. Non-Goals

- No cloud-first dependency required for basic operation
- No mandatory user account system in v1
- No remote OTA orchestration in initial release
- No timeline commitments in this specification

## 3.1 Scope Boundaries for PM Control

In scope:
- Native app parity for core mothership workflows.
- BLE command/control with deterministic state machine.
- Offline-first local persistence and recoverability.
- Sensor identity fidelity (`sensorId`, `sensorType`, `sensorLabel`).

Out of scope (unless later approved):
- Mandatory cloud backend dependencies.
- User-account/paywall systems.
- Full OTA fleet orchestration.

## 3.2 Dependency Map (PM/Orchestrator)

Critical coupling dependencies:
- Firmware protocol structs and constants.
- Mothership command handlers.
- Node telemetry semantics.

Operational dependencies:
- BLE platform behavior differences (iOS vs Android).
- File export/share behavior per mobile OS.
- Device-specific BLE reliability constraints in field environments.

## 4. Users and Usage Context

Primary users:
- Field technician
- Operations lead
- Research/engineering user validating sensors

Operating conditions:
- Outdoor deployments
- Variable signal quality
- Need for quick actions with minimal taps
- Need for exportable logs and auditable command history

## 5. Core Workflows (Required)

1. Connect to mothership
- Scan for mothership BLE advertisements
- Identify target mothership by device name and hardware ID
- Establish session and retrieve system status

2. Discovery and node intake
- Trigger discovery window
- Show discovered nodes and metadata
- Pair selected nodes

3. Deploy and runtime control
- Deploy node(s)
- Set wake interval (sampling cadence)
- Set sync interval and alignment phase
- Revert/unpair when required

4. Health and diagnostics
- Fleet summary and per-node status
- Queue pressure indicators
- Last contact and time quality flags
- Error and warning feed

5. Data export
- Request CSV export package from mothership
- Save and share from app
- Optional local preview of latest rows

## 6. Native App Functional Requirements

FR-01 Fleet dashboard
- Display mothership identity, RTC status, storage status
- Display node counts by state: discovered, paired, deployed, offline

FR-02 Node list and filtering
- Search by node ID and name
- Filter by state, sensor type, health severity

FR-03 Node detail
- Show node metadata and current config
- Show last seen, battery (if available), queue depth, oldest unsynced age
- Show latest sensor values and quality flags

FR-04 Discovery
- Trigger discovery command
- Stream discovery results in near real time
- Prevent duplicate rows for same node

FR-05 Pair/deploy/revert/unpair
- Execute command with explicit confirmation for destructive actions
- Surface deterministic result status

FR-06 Scheduling controls
- Fleet-level wake interval command
- Fleet-level sync interval command with phase anchor
- Optional per-node override support in schema

FR-07 Time management
- Set mothership RTC from phone time
- Manual date/time entry fallback

FR-08 Export and logs
- Request CSV export from mothership
- Show export status and file size
- Save/share file via native share sheet

FR-09 Command history
- Record all command attempts and outcomes locally
- Include correlation IDs for troubleshooting

FR-10 Offline resilience
- Cache latest fleet snapshot locally
- Queue app-initiated commands when disconnected (optional setting)

## 7. Non-Functional Requirements

NFR-01 Reliability
- Command operations must be idempotent where possible
- Retry policy with bounded attempts and clear user feedback

NFR-02 Performance
- Initial dashboard render in under 2 seconds from warm local cache
- Action acknowledgement feedback in under 500 ms when transport is available

NFR-03 Safety
- High-impact commands require user confirmation
- Failsafe default behavior on malformed responses

NFR-04 Observability
- Structured app logs with correlation IDs
- Exportable diagnostic bundle from app

NFR-05 Maintainability
- Shared domain models across screens
- Clear separation of UI, state, transport, repository layers

## 8. System Architecture

## 8.1 Logical layers (app)

- Presentation layer
  - Screens, view models, UI state
- Domain layer
  - Use-cases: discoverNodes, applySchedule, deployNode, exportCsv
- Data layer
  - BLE transport adapter
  - Optional WiFi fallback adapter
  - Local database/cache

## 8.2 Device-side alignment

- Mothership firmware exposes command handlers equivalent to web actions
- BLE command dispatcher maps app command -> existing internal service function
- ESP-NOW behavior remains unchanged for node data plane

## 8.3 Source of Truth Rules (Critical)

Builder must treat these as authoritative:
- Wire/message structures in:
  - firmware/nodes/shared/protocol.h
- Node registry and sensor slot behavior in:
  - firmware/nodes/shared/sensors.h
  - firmware/nodes/sensor-node/src/sensors/sensors.cpp
- Queue semantics and persisted sample model in:
  - firmware/nodes/sensor-node/src/storage/local_queue.h
  - firmware/nodes/sensor-node/src/storage/local_queue.cpp

Builder must not:
- Invent payload fields not represented by firmware unless placed under clearly optional app-local metadata.
- Collapse sensorLabel and sensorType into one field.
- Assume cloud connectivity for core operations.

Orchestrator must enforce:
- Any agent proposing schema changes must produce a "Protocol Change Request" artifact with impact assessment.
- Reviewer agent must explicitly verify no unauthorized schema drift.

## 9. Canonical Data Schemas

All schemas below are transport-neutral canonical models used in app domain and serialized over BLE payloads.

## 9.1 Common envelope schema

```json
{
  "schemaVersion": "1.0",
  "messageType": "command_request",
  "command": "discover_nodes",
  "correlationId": "d428f3d7-1241-4d7b-909f-b793f0a613a7",
  "source": "mobile_app",
  "target": "mothership",
  "timestampUnix": 1775941500,
  "payload": {}
}
```

Required fields:
- schemaVersion: string
- messageType: enum
- correlationId: UUID string
- timestampUnix: integer seconds
- payload: object

## 9.2 MessageType enum

- command_request
- command_ack
- command_result
- event
- status_snapshot
- error

## 9.3 Command catalog

Required commands:
- get_status
- set_time
- discover_nodes
- set_wake_interval
- set_sync_interval
- list_nodes
- node_config_apply
- node_revert
- node_unpair
- export_csv_request

## 9.4 Command payload schemas

### get_status request payload

```json
{}
```

### get_status result payload

```json
{
  "mothership": {
    "id": "MSHIP_001",
    "firmwareVersion": "2.1.0",
    "rtcUnix": 1775941500,
    "rtcHealth": "ok",
    "apEnabled": true,
    "espnowChannel": 1,
    "sdHealthy": true
  },
  "fleet": {
    "totalNodes": 24,
    "discovered": 5,
    "paired": 18,
    "deployed": 16,
    "offline": 3
  },
  "schedule": {
    "wakeIntervalMinutes": 5,
    "syncIntervalMinutes": 60,
    "phaseUnix": 1775941200
  }
}
```

### set_time request payload

```json
{
  "unixTime": 1775941500,
  "source": "phone_time"
}
```

### discover_nodes request payload

```json
{
  "scanWindowSeconds": 10,
  "includeKnownNodes": false
}
```

### discover_nodes event payload

```json
{
  "node": {
    "nodeId": "ENV_001",
    "nodeType": "MULTI_ENV_V1",
    "friendlyName": "North Plot Env",
    "state": "discovered",
    "lastSeenUnix": 1775941512,
    "rssi": -66
  }
}
```

### set_wake_interval request payload

```json
{
  "intervalMinutes": 5,
  "scope": "fleet",
  "nodeIds": []
}
```

### set_sync_interval request payload

```json
{
  "syncIntervalMinutes": 60,
  "phaseUnix": 1775941200,
  "scope": "fleet",
  "nodeIds": []
}
```

### list_nodes result payload

```json
{
  "nodes": [
    {
      "nodeId": "ENV_001",
      "friendlyName": "North Plot Env",
      "nodeType": "MULTI_ENV_V1",
      "state": "deployed",
      "lastSeenUnix": 1775941490,
      "wakeIntervalMinutes": 5,
      "syncIntervalMinutes": 60,
      "sensors": [
        {
          "sensorId": 1001,
          "sensorType": "AIR_TEMP",
          "sensorLabel": "AIR_TEMP"
        },
        {
          "sensorId": 1002,
          "sensorType": "HUMIDITY",
          "sensorLabel": "AIR_RH"
        },
        {
          "sensorId": 1101,
          "sensorType": "PAR",
          "sensorLabel": "PAR"
        },
        {
          "sensorId": 2001,
          "sensorType": "SOIL_VWC",
          "sensorLabel": "SOIL1_VWC"
        }
      ],
      "queue": {
        "pendingRecords": 42,
        "oldestPendingAgeSeconds": 720,
        "storageFullEvents": 0,
        "droppedUnsynced": 0
      },
      "health": {
        "severity": "warning",
        "flags": ["TIME_UNCERTAIN"]
      }
    }
  ]
}
```

### node_config_apply request payload

```json
{
  "nodeId": "ENV_001",
  "friendlyName": "North Plot Env",
  "targetState": "deployed",
  "wakeIntervalMinutes": 5,
  "syncIntervalMinutes": 60,
  "phaseUnix": 1775941200
}
```

### node_revert request payload

```json
{
  "nodeId": "ENV_001"
}
```

### node_unpair request payload

```json
{
  "nodeId": "ENV_001"
}
```

### export_csv_request payload

```json
{
  "range": {
    "fromUnix": 1775855100,
    "toUnix": 1775941500
  },
  "nodeIds": ["ENV_001", "ENV_002"],
  "sensorTypes": ["AIR_TEMP", "HUMIDITY", "PAR", "WIND", "SOIL_VWC", "SOIL_TEMP"],
  "sensorIds": [1001, 1002, 1101, 1201, 2001, 2002, 2003, 2004],
  "format": "csv"
}
```

### node telemetry event payload (required for streaming UI)

```json
{
  "nodeId": "ENV_001",
  "sensorId": 2003,
  "sensorType": "SOIL_TEMP",
  "sensorLabel": "SOIL1_TEMP",
  "value": 18.42,
  "nodeTimestamp": 1775941602,
  "qualityFlags": 0,
  "receivedUnix": 1775941603
}
```

### export_csv_result payload

```json
{
  "fileName": "mothership_export_2026-04-12_1200.csv",
  "mimeType": "text/csv",
  "sizeBytes": 85120,
  "transferMode": "chunked",
  "chunkCount": 9,
  "checksumSha256": "7ec4..."
}
```

## 9.5 Acknowledgement and result schema

### command_ack

```json
{
  "correlationId": "d428f3d7-1241-4d7b-909f-b793f0a613a7",
  "accepted": true,
  "receivedUnix": 1775941501,
  "message": "Command accepted"
}
```

### command_result

```json
{
  "correlationId": "d428f3d7-1241-4d7b-909f-b793f0a613a7",
  "ok": true,
  "completedUnix": 1775941502,
  "message": "Discovery started",
  "payload": {}
}
```

## 9.6 Error schema

```json
{
  "correlationId": "d428f3d7-1241-4d7b-909f-b793f0a613a7",
  "ok": false,
  "error": {
    "code": "NODE_NOT_FOUND",
    "category": "validation",
    "retryable": false,
    "message": "Node ENV_001 does not exist",
    "details": {
      "nodeId": "ENV_001"
    }
  }
}
```

Error code set (minimum):
- INVALID_COMMAND
- INVALID_PAYLOAD
- UNAUTHORIZED
- BUSY
- TIMEOUT
- NODE_NOT_FOUND
- STORAGE_ERROR
- RTC_ERROR
- EXPORT_ERROR
- INTERNAL_ERROR

## 9.7 Node and fleet state enums

Node state:
- discovered
- paired
- deployed
- reverted
- offline

Health severity:
- ok
- warning
- critical

RTC health:
- ok
- uncertain
- invalid

## 10. BLE GATT Schema (Recommended)

Use one custom service with three characteristics.

Service UUID:
- 6f880001-2d0f-4aa0-8f9e-8f8b7e5a0001

Characteristics:
- Request Write Char UUID:
  - 6f880002-2d0f-4aa0-8f9e-8f8b7e5a0001
  - Properties: Write, Write Without Response
- Response Notify Char UUID:
  - 6f880003-2d0f-4aa0-8f9e-8f8b7e5a0001
  - Properties: Notify
- Status Notify Char UUID:
  - 6f880004-2d0f-4aa0-8f9e-8f8b7e5a0001
  - Properties: Notify

Payload framing:
- UTF-8 JSON chunks with sequence framing header
- Header fields:
  - messageId
  - chunkIndex
  - chunkCount
  - payloadLength
- Reassembly at both ends before JSON parse

## 11. Local App Storage Schema

Use SQLite for deterministic offline behavior.

## 11.1 Tables

Table: mothership_snapshot
- id TEXT PRIMARY KEY
- firmware_version TEXT
- rtc_unix INTEGER
- rtc_health TEXT
- ap_enabled INTEGER
- espnow_channel INTEGER
- sd_healthy INTEGER
- updated_unix INTEGER

Table: nodes
- node_id TEXT PRIMARY KEY
- friendly_name TEXT
- node_type TEXT
- state TEXT
- last_seen_unix INTEGER
- wake_interval_minutes INTEGER
- sync_interval_minutes INTEGER
- phase_unix INTEGER
- pending_records INTEGER
- oldest_pending_age_seconds INTEGER
- storage_full_events INTEGER
- dropped_unsynced INTEGER
- health_severity TEXT
- health_flags_json TEXT
- updated_unix INTEGER

Table: command_log
- correlation_id TEXT PRIMARY KEY
- command TEXT
- request_json TEXT
- status TEXT
- accepted INTEGER
- ok INTEGER
- error_code TEXT
- message TEXT
- created_unix INTEGER
- completed_unix INTEGER

Table: event_log
- event_id TEXT PRIMARY KEY
- event_type TEXT
- payload_json TEXT
- created_unix INTEGER

Table: decisions_log
- decision_id TEXT PRIMARY KEY
- title TEXT
- context TEXT
- decision TEXT
- rationale TEXT
- impact TEXT
- owner TEXT
- created_unix INTEGER

Table: risk_register
- risk_id TEXT PRIMARY KEY
- title TEXT
- description TEXT
- likelihood TEXT
- severity TEXT
- mitigation TEXT
- owner TEXT
- status TEXT
- updated_unix INTEGER

Table: open_issues
- issue_id TEXT PRIMARY KEY
- title TEXT
- description TEXT
- blocking INTEGER
- owner TEXT
- status TEXT
- updated_unix INTEGER

Table: export_files
- export_id TEXT PRIMARY KEY
- file_name TEXT
- path TEXT
- checksum_sha256 TEXT
- size_bytes INTEGER
- created_unix INTEGER

## 11.2 Status enum in command_log

- queued
- sent
- acknowledged
- completed
- failed
- expired

## 12. State Management Model (App)

State slices:
- connectivityState
- mothershipState
- fleetState
- commandState
- exportState
- uiState

Recommended behavior:
- Optimistic UI only for reversible toggles
- Pessimistic UI for destructive operations (unpair/revert)
- Every command tracked by correlationId from creation to completion

PM traceability requirement:
- Every user-visible operation must be traceable to:
  - command request
  - command ack/result
  - final UI state transition

## 13. UX and Design Parameters (Clean, Simple)

Design direction:
- Minimal, high-contrast, utility-first interface
- One primary action per screen section
- Heavy emphasis on state legibility over decoration

Visual system:
- Color tokens:
  - Background: #F7F8FA
  - Surface: #FFFFFF
  - Primary: #0B5FFF
  - Success: #1E8E3E
  - Warning: #B26A00
  - Critical: #C62828
  - Text Primary: #101418
  - Text Secondary: #5C6670
- Typography:
  - Headings: Inter SemiBold
  - Body: Inter Regular
  - Numeric metrics: JetBrains Mono Medium
- Spacing scale:
  - 4, 8, 12, 16, 24, 32
- Radius:
  - Cards/buttons: 10
- Shadows:
  - Subtle only on elevated surfaces; avoid heavy blur

Interaction rules:
- Touch targets minimum 44x44
- Confirm dialogs for destructive actions
- Inline error text near failing control
- Global toast for command completion
- Loading placeholders for all data cards

Accessibility baseline:
- WCAG AA contrast minimum
- Dynamic type support
- Screen reader labels for all action controls

## 14. Information Architecture

Primary tabs:
- Dashboard
- Nodes
- Actions
- Exports
- Settings

Screen requirements:
- Dashboard:
  - Mothership card, fleet summary, active schedules, critical alerts
- Nodes:
  - Search/filter list, quick state chips, tap-through details
- Node Detail:
  - Identity, config, health, queue pressure, actions panel
- Actions:
  - Discovery, fleet schedule commands, RTC sync actions
- Exports:
  - Create export, transfer progress, saved files list
- Settings:
  - Transport options, diagnostics export, app behavior toggles

## 15. Security and Trust Model

Recommended controls:
- BLE pairing gate before control commands
- App-side operation PIN for destructive actions (optional but recommended)
- Command origin tagging in firmware logs
- Replay protection using correlationId and timestamp window
- Reject stale command timestamps beyond configurable threshold

## 16. Build Recommendations

Recommended primary stack: Flutter
- Pros:
  - Single codebase for iOS and Android
  - Mature BLE ecosystem (flutter_reactive_ble)
  - Strong UI consistency and rapid iteration
  - Good offline database options (drift/sqflite)

Alternative stack: React Native
- Pros:
  - JS/TS ecosystem familiarity
  - Fast iteration for teams with web background
- Tradeoff:
  - BLE integration complexity can be higher depending on module choices

Alternative stack: Native Swift/Kotlin
- Pros:
  - Maximum platform fidelity and BLE control
- Tradeoff:
  - Two codebases and slower parity work

Recommended app architecture pattern:
- Clean architecture with feature modules
- MVVM-style presentation state
- Repository pattern for transport/storage abstraction

Suggested package set (Flutter):
- flutter_reactive_ble
- freezed + json_serializable
- riverpod
- drift or sqflite
- uuid
- share_plus

## 17. Firmware Requirements to Support App

Required firmware capabilities:
- BLE service with request/response notify pipeline
- CorrelationId passthrough from request to result
- Deterministic command ack and result semantics
- Node list/status serialization aligned to schemas above
- Chunked export transfer and checksum metadata
- Structured error code responses

Strongly recommended:
- Unified command handlers shared by web and BLE transport
- Rate limiting for sensitive operations
- Persistent command/event logging on mothership for diagnostics

## 18. Validation and Acceptance Criteria

App is acceptable when all are true:
- Can connect to mothership over BLE and retrieve status
- Can discover/pair/deploy/revert/unpair nodes
- Can set wake and sync schedules
- Can set RTC and verify update
- Can export CSV and save/share successfully
- Can show queue pressure and health indicators per node
- Can recover gracefully after connection loss without data corruption
- Can render and filter by sensorId/sensorType/sensorLabel without ambiguity

Additional PM acceptance gates:
- Requirement parity gate:
  - Each FR/NFR is mapped to implementation files and test cases.
- Security gate:
  - Sensitive actions have explicit confirmation and stale/replay protections.
- Reliability gate:
  - Retry/backoff and offline recovery validated with test evidence.
- Observability gate:
  - Logs and diagnostics export can reconstruct command lifecycle failures.

## 19. OpenClaw Handoff Package

Provide this package to OpenClaw:
- This document
- Current firmware command/protocol definitions
- Sample message captures for each command/result/error
- UI wireframe screenshots for each primary screen
- BLE test checklist and expected outcomes
- Known constraints and unresolved risks list

PM package additions (required):
- Requirement Traceability Matrix (RTM)
- Decision log
- Risk register
- Open issues register
- Release readiness checklist

## 20. Suggested Implementation Order (No Timeline)

1. Establish canonical schema models and validators in app
2. Implement BLE transport + message framing + correlation tracking
3. Implement command engine with ack/result state machine
4. Build Dashboard and Nodes screens from local cached models
5. Add command actions: discovery, schedules, node actions, RTC
6. Add exports workflow and file handoff
7. Add diagnostics surfaces and log export
8. Harden retries, error handling, and accessibility compliance

## 21. Builder LLM Execution Contract

Builder output must include, at minimum:
- App architecture scaffold (chosen stack) with feature modules
- Typed domain models for all schemas in this document
- Transport layer with request/ack/result/event handling
- Local persistence layer with migrations
- UI flows for Dashboard, Nodes, Actions, Exports, Settings
- Error handling and retry middleware
- Basic test coverage for command pipeline and model parsing

Builder must produce these artifact categories:
- Source code
- Build/run instructions
- Environment/config files
- Tests
- Integration notes for firmware coupling points

Builder must enforce these coding constraints:
- Use typed models for payloads (no untyped map-only parsing in core domain)
- Keep correlationId lifecycle traceable in logs and persisted command history
- Separate transport DTOs from UI view models
- Keep destructive actions behind explicit confirmations

Builder must provide PM-facing implementation evidence:
- List of completed requirements with file references.
- List of deferred items and reasons.
- Known technical debt created by implementation decisions.

## 22. Builder Prompt Template (Paste-Ready)

Use this as the top-level instruction to a builder LLM:

```text
Build a production-grade mobile app (Flutter preferred) for an ESP32 environmental logging system.

You MUST implement exactly the schemas and behavior in docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md.

Hard constraints:
1) Offline-first core operation (no cloud required for core workflows).
2) BLE control channel with request/ack/result/event model.
3) Node telemetry identity must include sensorId + sensorType + sensorLabel.
4) Preserve deterministic command tracking via correlationId.
5) Include automated tests for schema parsing, command state transitions, and retry logic.

Required deliverables:
1) Complete source tree.
2) Run/build instructions.
3) Config/env setup.
4) Tests and test instructions.
5) Integration notes showing how each app command maps to firmware command handlers.

Do not invent firmware endpoints beyond this spec. Mark any uncertainty explicitly as TODO with rationale.
```

## 22.1 Researcher Prompt Template (Paste-Ready)

```text
You are a research agent supporting a mobile app implementation for an ESP32 field logging system.

Primary references:
1) docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md
2) firmware/nodes/shared/protocol.h
3) firmware/nodes/shared/sensors.h

Research objectives:
1) Identify implementation risks and unknowns for your assigned topic.
2) Provide evidence-backed recommendations.
3) Output decisions needed from PM/orchestrator.

Output format:
1) Findings
2) Options (pros/cons)
3) Recommended option
4) Risks if not adopted
5) Required follow-up tasks for builders/reviewers
```

## 22.2 Reviewer Prompt Template (Paste-Ready)

```text
You are a reviewer agent for the native app implementation.

Review priorities:
1) Requirement parity with docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md
2) Protocol/schema correctness versus firmware source of truth
3) Reliability, security, and offline behavior
4) Test coverage adequacy

Output format:
1) Findings by severity (critical/high/medium/low)
2) File-level references
3) Missing tests
4) Blockers to sign-off
5) Sign-off decision (approve/approve with conditions/reject)
```

## 22.3 Orchestrator Prompt Template (Paste-Ready)

```text
You are the orchestrator for a multi-agent app delivery team.

Use docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md as master specification.

Your responsibilities:
1) Decompose work into researcher, builder, and reviewer tasks.
2) Enforce Definition of Ready and Definition of Done.
3) Maintain traceability across requirements, implementation, tests, and decisions.
4) Escalate unresolved blockers and protocol ambiguities.

Required outputs:
1) Task graph with dependencies
2) Assignment plan by agent role
3) Progress report with completed/blocked/in-review statuses
4) Updated risk and issue registers
5) Final delivery checklist for PM sign-off
```

## 23. Minimum File/Module Structure (Recommended)

Target module layout:
- app/core
- app/features/connectivity
- app/features/dashboard
- app/features/nodes
- app/features/actions
- app/features/exports
- app/features/settings
- app/data/transport
- app/data/storage
- app/domain/models
- app/domain/usecases
- app/test

Minimum tests:
- Envelope/message parser tests
- Command state machine tests (queued -> sent -> acknowledged -> completed/failed)
- Retry/backoff behavior tests
- Local DB migration tests
- Sensor identity display tests (sensorId/type/label)

## 24. PM Operating Checklist

Use this checklist to run delivery with agent teams:

1. Confirm scope boundaries and source-of-truth files.
2. Approve orchestrator task graph and ownership.
3. Ensure researcher findings are converted to explicit implementation decisions.
4. Ensure each builder task has acceptance tests before coding begins.
5. Require reviewer sign-off per workstream.
6. Verify RTM completeness before declaring feature-complete.
7. Review unresolved risks/issues and approve disposition.
8. Approve final release-readiness checklist.

## 25. Required Delivery Artifacts (Final Handoff)

The final handoff package must include all artifacts below:

- Product and requirements artifacts:
  - Final spec (this document)
  - Requirement Traceability Matrix

- Technical artifacts:
  - Architecture overview and module map
  - Protocol/schema implementation notes
  - Integration notes for firmware coupling points

- Implementation artifacts:
  - Source code
  - Build/run instructions
  - Configuration files

- Quality artifacts:
  - Test plan
  - Test results and evidence
  - Reviewer findings and resolutions

- Governance artifacts:
  - Decision log
  - Risk register
  - Open issues list
  - Release readiness checklist

---

This specification is intentionally detailed to function as a direct engineering handoff. If needed, a companion document can be generated with concrete wireframe layouts and a complete API test matrix based on these schemas.
