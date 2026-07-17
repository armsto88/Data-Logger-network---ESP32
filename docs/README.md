# Documentation Index

**Last reviewed:** 2026-07-17

**Repository-wide Markdown audit:** 2026-07-17

This folder contains cross-system FieldMesh documentation. Hardware- and firmware-specific documents also live in [mothership/docs/](../mothership/docs/), [node/docs/](../node/docs/), [sensors/](../sensors/), and [hardware/](../hardware/).

The 2026-07-17 audit scanned every Markdown file outside generated build/dependency folders. Distinct hardware results, protocol contracts, calibration evidence, and dated implementation records were retained. Repetitive legacy agent files were merged into one dated archive, byte-identical OpenFOAM case READMEs were removed in favour of their shared instructions, and a broken V2 bring-up pointer was replaced by a direct index link.

## Start here

- [FIELDMESH_OVERVIEW.md](FIELDMESH_OVERVIEW.md) - concise system overview.
- [FIELDMESH_FIRMWARE_FLOW_SUMMARY_2026-07-03.md](FIELDMESH_FIRMWARE_FLOW_SUMMARY_2026-07-03.md) - current node, mothership, sync, power, and data flow.
- [FIELDMESH_FAQ.md](FIELDMESH_FAQ.md) - field deployment and operation questions.
- [FIRMWARE_FUNCTIONALITY_FOR_ECOLOGISTS.md](FIRMWARE_FUNCTIONALITY_FOR_ECOLOGISTS.md) - non-technical end-to-end explanation.
- [ADDING_A_NEW_SENSOR_CHECKLIST.md](ADDING_A_NEW_SENSOR_CHECKLIST.md) - mandatory sensor-package integration checklist.
- [mothership OTA plan](../mothership/docs/FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md) - staged mothership and node firmware update design (see §0 for build/proven status).
- [FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md](FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md) - front/backend handoff: displaying firmware info, config controls (interval/pause/undeploy), and dashboard-initiated mothership OTA, with data shapes + an LLM prompt.

## Protocols and data contracts

- [FIELDMESH_COORDINATED_SYNC_PROTOCOL.md](FIELDMESH_COORDINATED_SYNC_PROTOCOL.md)
- [FIELDMESH_NODE_CONFIG_PROTOCOL.md](FIELDMESH_NODE_CONFIG_PROTOCOL.md)
- [FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md](FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md)
- [FIELDMESH_PAYLOAD_REFERENCE.md](FIELDMESH_PAYLOAD_REFERENCE.md)
- [V2_SNAPSHOT_MIGRATION_PLAN.md](V2_SNAPSHOT_MIGRATION_PLAN.md)
- [FIELDMESH_NEW_DATA_FIELDS_BRIEF.md](FIELDMESH_NEW_DATA_FIELDS_BRIEF.md)

## Deployment, validation, and incident records

- [FIELDMESH_DEPLOYMENT_LOG.md](FIELDMESH_DEPLOYMENT_LOG.md)
- [MULTI_NODE_VALIDATION_2026-05-05.md](MULTI_NODE_VALIDATION_2026-05-05.md)
- [WEEKLY_UPDATE_2026-07-07.md](WEEKLY_UPDATE_2026-07-07.md)
- [AS7341_EXTENDED_METADATA_NULL_FIX_2026-07-04.md](AS7341_EXTENDED_METADATA_NULL_FIX_2026-07-04.md)
- [FIRMWARE_BUG_SOIL_REGISTRATION_2026-07-04.md](FIRMWARE_BUG_SOIL_REGISTRATION_2026-07-04.md)
- [OPS_ALERT_SOIL_SENSOR_REGISTRATION_BUG_2026-07-04.md](OPS_ALERT_SOIL_SENSOR_REGISTRATION_BUG_2026-07-04.md)
- [BACKEND_FRONTEND_SPECTRAL_HANDOFF_2026-07-04.md](BACKEND_FRONTEND_SPECTRAL_HANDOFF_2026-07-04.md)
- [BACKEND_SPECTRAL_INGEST_PROMPT_2026-07-04.md](BACKEND_SPECTRAL_INGEST_PROMPT_2026-07-04.md)
- [BACKEND_FRONTEND_SPECTRAL_ECOLOGY_IMPLEMENTATION_PROMPT_2026-07-04.md](BACKEND_FRONTEND_SPECTRAL_ECOLOGY_IMPLEMENTATION_PROMPT_2026-07-04.md)

The apparently overlapping spectral and soil documents were retained because they serve different audiences: causal firmware record, operations alert, backend contract, and implementation brief. Their line-level content is not duplicated.

## Cloud, dashboard, and onboarding

- [FIELDMESH_SUPABASE_MIGRATION_PLAN.md](FIELDMESH_SUPABASE_MIGRATION_PLAN.md)
- [FIELDMESH_SUPABASE_SCHEMA_CONFIRMED.md](FIELDMESH_SUPABASE_SCHEMA_CONFIRMED.md)
- [FIELDMESH_SUPABASE_FIRMWARE_INTEGRATION.md](FIELDMESH_SUPABASE_FIRMWARE_INTEGRATION.md)
- [SUPABASE_FIRMWARE_INTEGRATION_TASK.md](SUPABASE_FIRMWARE_INTEGRATION_TASK.md)
- [FIELDMESH_DASHBOARD_DESIGN.md](FIELDMESH_DASHBOARD_DESIGN.md)
- [FIELDMESH_USER_ONBOARDING_BRIEF.md](FIELDMESH_USER_ONBOARDING_BRIEF.md)
- [FIELDMESH_SPATIAL_LOCATION_PLAN.md](FIELDMESH_SPATIAL_LOCATION_PLAN.md)
- [MOTHERSHIP_APP_BACKEND_INTEGRATION_BRIEF_2026-06-12.md](MOTHERSHIP_APP_BACKEND_INTEGRATION_BRIEF_2026-06-12.md)

## Product content and UI design

- [FIELDMESH_ABOUT_CONTENT.md](FIELDMESH_ABOUT_CONTENT.md)
- [FIELDMESH_ABOUT_SECTION_DESIGN.md](FIELDMESH_ABOUT_SECTION_DESIGN.md)
- [FIELDMESH_FIELD_UI_REDESIGN.md](FIELDMESH_FIELD_UI_REDESIGN.md)
- [FIELDMESH_UI_STYLE_GUIDE_2026.md](FIELDMESH_UI_STYLE_GUIDE_2026.md)
- [FIELDMESH_WEBSITE_COPY_AMENDED_2026-07-14.md](FIELDMESH_WEBSITE_COPY_AMENDED_2026-07-14.md)
- [MOTHERSHIP_UI_REDESIGN_PROMPT.md](MOTHERSHIP_UI_REDESIGN_PROMPT.md)

## Native app planning

- [NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md](NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md) - master native-app concept and schema reference.
- [NATIVE_APP_INTEGRATION_V2.md](NATIVE_APP_INTEGRATION_V2.md) - BLE integration design.
- [NATIVE_APP_INTEGRATION_STATUS_2026-04-13.md](NATIVE_APP_INTEGRATION_STATUS_2026-04-13.md) - dated implementation status.
- [PHASE_C_APP_INTEGRATION_CHECKLIST.md](PHASE_C_APP_INTEGRATION_CHECKLIST.md) - phase acceptance checklist.
- [SONNET_HANDOFF_NODE_COMM_WORKFLOW.md](SONNET_HANDOFF_NODE_COMM_WORKFLOW.md) - historical node communication handoff.

These documents were not merged because the master specification, transport design, dated status, checklist, and handoff are different artefacts with distinct traceability value.

## Historical and reference material

- [concept_overview.md](concept_overview.md) - earlier lifecycle/dashboard snapshot; retained because later documents still cite its terminology and historical state.
- [FutureRoadMap.md](FutureRoadMap.md) - forward-looking roadmap.
- [FIRMWARE_AND_HARDWARE_NOTES.md](FIRMWARE_AND_HARDWARE_NOTES.md) - dated engineering notebook already consolidated from earlier source files.
- [AGENT_MODEL_SETUP.md](AGENT_MODEL_SETUP.md) - current agent model-routing note.
- [archive/LEGACY_AGENT_WORKFLOW_2026-05-24.md](archive/LEGACY_AGENT_WORKFLOW_2026-05-24.md) - consolidated 2026-04-12 to 2026-05-24 Positron-era workflow and native-app work package.
- [archive/manuscript/](archive/manuscript/) - manuscript material.

## Documentation rules

- Give incident reports, test results, handoffs, and point-in-time plans an ISO date (`YYYY-MM-DD`) in the filename or document header.
- When merging documents, record the original dates, consolidation date, and source filenames in the destination.
- Do not delete a dated hardware observation merely because a later design supersedes it; mark its status and link forward.
- Prefer one canonical procedure plus links from indexes over copied README content in every generated/case folder.
- Verify relative Markdown links after file moves or consolidation.
