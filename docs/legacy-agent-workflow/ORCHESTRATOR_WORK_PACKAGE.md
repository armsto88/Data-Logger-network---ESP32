# Orchestrator Work Package (Researcher + Builder + Reviewer)

Status: Execution-ready
Primary reference: docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md
Audience: Orchestrator, PM, agent team leads

## 1. Purpose

This work package converts the master native-app specification into spawnable, dependency-aware tasks for:
- Researcher agents
- Builder agents
- Reviewer agents

It is designed for immediate orchestration and PM tracking.

## 2. Ground Rules

1. The source of truth is docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md.
2. Firmware-coupled schemas must align with:
- firmware/nodes/shared/protocol.h
- firmware/nodes/shared/sensors.h
- firmware/nodes/sensor-node/src/storage/local_queue.h
3. No schema changes without Protocol Change Request artifact.
4. Every completed build task requires:
- Test evidence
- Reviewer sign-off
- PM traceability update

## 3. Workstream Map

WS-01: Product + requirements consolidation
WS-02: Protocol and integration research
WS-03: App architecture scaffold
WS-04: Core command pipeline and transport
WS-05: Fleet UX screens
WS-06: Exports and diagnostics
WS-07: Reliability/security hardening
WS-08: QA/review and release readiness

## 4. Dependency Graph (High-Level)

1. WS-01 and WS-02 start first and can run in parallel.
2. WS-03 depends on WS-01 and WS-02 outputs.
3. WS-04 depends on WS-03.
4. WS-05 depends on WS-03 and WS-04.
5. WS-06 depends on WS-04 and partially on WS-05.
6. WS-07 depends on WS-04, WS-05, and WS-06.
7. WS-08 runs continuously but final sign-off depends on all prior workstreams.

## 5. Spawn-Ready Task Cards

## 5.1 Research Tasks

R-01 Protocol parity audit
- Goal:
  - Confirm app schema parity with firmware payload model (`sensorId`, `sensorType`, `sensorLabel`, `qualityFlags`).
- Inputs:
  - docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md
  - firmware/nodes/shared/protocol.h
- Outputs:
  - Protocol parity report
  - Gap list
  - Protocol Change Requests (if needed)
- Acceptance:
  - All mismatches classified as fix-now, acceptable-deviation, or blocked.

R-02 BLE platform behavior research
- Goal:
  - Identify iOS/Android BLE differences affecting request/ack/result/event pipeline.
- Inputs:
  - Master spec
- Outputs:
  - Platform risk report
  - Recommended implementation constraints
- Acceptance:
  - Actionable recommendations for builders and reviewers.

R-03 Export workflow research
- Goal:
  - Confirm file save/share constraints and safe defaults on both mobile platforms.
- Outputs:
  - Export implementation recommendations
  - Platform-specific caveats

R-04 Reliability test strategy research
- Goal:
  - Define test scenarios for intermittent connectivity and command retries.
- Outputs:
  - Reliability test matrix draft

## 5.2 Build Tasks

B-01 App scaffold and module layout
- Depends on:
  - R-01, R-02
- Goal:
  - Create architecture skeleton matching spec module structure.
- Outputs:
  - Initial source tree
  - Build/run instructions
- Acceptance:
  - App builds on target platforms
  - Module boundaries match specification

B-02 Typed schema model implementation
- Depends on:
  - B-01, R-01
- Goal:
  - Implement all canonical envelope/message models with validation.
- Acceptance:
  - Parser/serialization tests pass
  - Sensor identity fields fully represented

B-03 Command state machine + correlation tracking
- Depends on:
  - B-02
- Goal:
  - Implement deterministic command lifecycle:
    - queued -> sent -> acknowledged -> completed/failed
- Acceptance:
  - State machine tests pass
  - Correlation IDs persisted and visible in logs

B-04 BLE transport adapter
- Depends on:
  - B-03, R-02
- Goal:
  - Implement BLE request/notify pipeline with framing and reassembly.
- Acceptance:
  - Transport integration tests pass

B-05 Local persistence layer
- Depends on:
  - B-02, B-03
- Goal:
  - Implement local DB tables, migrations, and repository APIs.
- Acceptance:
  - Migration tests pass
  - Offline cache and command logs recover correctly

B-06 Dashboard and node list UI
- Depends on:
  - B-04, B-05
- Goal:
  - Build dashboard + node list/detail with sensor identity rendering.
- Acceptance:
  - UI integration tests pass
  - Node detail shows sensorId/type/label correctly

B-07 Actions UI (discover/pair/deploy/revert/unpair/schedule/time)
- Depends on:
  - B-04, B-06
- Goal:
  - Implement operator action flows with confirmations and feedback.
- Acceptance:
  - Action flow tests pass
  - Destructive actions gated by confirm UI

B-08 Export and diagnostics UI
- Depends on:
  - B-04, B-05, R-03
- Goal:
  - Implement export creation/download/share and diagnostics export.
- Acceptance:
  - Export flow tests pass

B-09 Security/reliability hardening
- Depends on:
  - B-07, B-08, R-04
- Goal:
  - Implement retries, backoff, stale-command rejection, and resilience behavior.
- Acceptance:
  - Reliability and negative-path tests pass

B-10 PM traceability outputs
- Depends on:
  - All prior build tasks
- Goal:
  - Generate requirement-to-implementation evidence and unresolved debt list.
- Acceptance:
  - PM package complete

## 5.3 Review Tasks

V-01 Protocol/schema conformance review
- Trigger:
  - After B-02 and after major protocol-touching changes
- Acceptance:
  - No unauthorized schema drift

V-02 Architecture and code quality review
- Trigger:
  - After B-06/B-07
- Acceptance:
  - Layering boundaries respected
  - No critical architectural anti-patterns

V-03 Reliability/security review
- Trigger:
  - After B-09
- Acceptance:
  - Retry/recovery and safety behaviors meet spec

V-04 Release readiness review
- Trigger:
  - After B-10
- Acceptance:
  - Full sign-off package produced

## 6. Required Artifacts by Workstream

WS-01 artifacts:
- Requirement baseline confirmation
- Scope boundary memo

WS-02 artifacts:
- Protocol parity report
- BLE platform risk report
- Export platform report
- Reliability test strategy

WS-03..WS-07 artifacts:
- Source code
- Tests
- Build instructions
- Integration notes

WS-08 artifacts:
- Reviewer findings log
- Final approval disposition
- Release readiness checklist

PM governance artifacts (always updated):
- Decision log
- Risk register
- Open issues register
- Requirement Traceability Matrix

## 7. Quality Gates

Gate A: Requirements parity
- Every FR/NFR mapped to implementation + test evidence.

Gate B: Schema parity
- Firmware-aligned fields preserved end-to-end.

Gate C: Reliability
- Connectivity loss/retry/recovery scenarios validated.

Gate D: Security/safety
- Destructive actions and stale/replay protections validated.

Gate E: Observability
- Logs and diagnostics sufficient for field incident triage.

## 8. Orchestrator Spawn Snippets

Research spawn template:
```text
Role: Researcher
Task: <insert task id and title>
Inputs: docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md + relevant firmware files
Output required:
1) Findings
2) Recommended option
3) Risks
4) Decisions needed from PM
5) Follow-up tasks for builder/reviewer
```

Builder spawn template:
```text
Role: Builder
Task: <insert task id and title>
Requirements:
- Follow docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md exactly
- Maintain firmware schema parity
Output required:
1) Code changes
2) Tests
3) Build/run instructions
4) Requirement traceability notes
```

Reviewer spawn template:
```text
Role: Reviewer
Task: <insert task id and title>
Review priorities:
1) Requirement parity
2) Schema conformance
3) Reliability/security
4) Test adequacy
Output required:
1) Findings by severity
2) Blocking issues
3) Sign-off decision
```

## 9. PM Weekly Status Format

1. Completed this period
- Task IDs
- Acceptance evidence

2. In progress
- Task IDs
- Current blockers

3. Risks and issues
- New risks
- Mitigation status

4. Decisions needed
- Decision IDs
- Deadline and impact if delayed

5. Next execution window
- Planned task IDs
- Assigned roles

## 10. Final Handoff Checklist

1. All task cards closed with evidence.
2. All quality gates passed.
3. Reviewer sign-off complete.
4. PM governance artifacts current.
5. Release readiness checklist approved.

---

This file is the execution wrapper for the master specification and is intended to be directly consumed by an agent orchestrator and PM.
