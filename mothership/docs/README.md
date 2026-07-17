# Mothership Documentation

**Last reviewed:** 2026-07-17

Design, bring-up, implementation, and review documents for the ESP32-WROOM mothership hub.

## Firmware architecture and implementation

- [FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md](FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md) - detailed signed OTA and backend-queued control plan, including local/dashboard state mirroring and mothership-to-node distribution (see §0 for build/proven status).
- [MOTHERSHIP_STATUS_REPORTING_PLAN.md](MOTHERSHIP_STATUS_REPORTING_PLAN.md) - tiered plan to report firmware identity, OTA state, control revision, and per-node firmware/desired-vs-applied into the cloud upload payload.
- [MOTHERSHIP_V1_FIRMWARE_PLAN.md](MOTHERSHIP_V1_FIRMWARE_PLAN.md) - wake-gated firmware architecture.
- [MOTHERSHIP_V1_DEPLOYMENT_UI_PLAN.md](MOTHERSHIP_V1_DEPLOYMENT_UI_PLAN.md) - deployment UI behaviour and state plan.
- [MOTHERSHIP_V2_ON_DEVICE_UI_UPGRADE_PLAN.md](MOTHERSHIP_V2_ON_DEVICE_UI_UPGRADE_PLAN.md) - current on-device UI upgrade plan.
- [MOTHERSHIP_V1_MODEM_UPLOAD_PLAN.md](MOTHERSHIP_V1_MODEM_UPLOAD_PLAN.md) - modem upload implementation plan.
- [MOTHERSHIP_V1_ROBUSTNESS_FIX_PLAN.md](MOTHERSHIP_V1_ROBUSTNESS_FIX_PLAN.md) - reliability fixes, acceptance plan, and the dated 2026-06-22 execution record consolidated on 2026-07-17.

## Bring-up and LTE

- [MOTHERSHIP_V1_BRINGUP_RESULTS_2026-06-19.md](MOTHERSHIP_V1_BRINGUP_RESULTS_2026-06-19.md) - hardware bring-up evidence.
- [MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md](MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md) - LTE hardware and backhaul strategy.
- [MOTHERSHIP_A7670G_MODEM_BRINGUP_NOTES.md](MOTHERSHIP_A7670G_MODEM_BRINGUP_NOTES.md) - A7670G power, UART, SIM, RF, and firmware bring-up.

## PCB and power

- [MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md](MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md) - power architecture, RTC wake cycles, boost converter, and pins.
- [MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md](MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md) - dated schematic review.
- [MOTHERSHIP_PCB_SCHEMATIC_RECONCILIATION.md](MOTHERSHIP_PCB_SCHEMATIC_RECONCILIATION.md) - design-note and schematic signal reconciliation.
- [MOTHERSHIP_PCB_PREORDER_CHECKS.md](MOTHERSHIP_PCB_PREORDER_CHECKS.md) - manufacturing/preorder gate extracted from the review.
- [MT3608_BROWNOUT_DESIGN_NOTE.md](MT3608_BROWNOUT_DESIGN_NOTE.md) - boost-converter brownout analysis.

Point-in-time reviews, reconciliations, and preorder gates remain separate because they record different dates and decision stages.
