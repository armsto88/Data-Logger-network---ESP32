# Mothership Documentation

Design notes and review documents for the ESP32-WROOM mothership hub.

## PCB and Power

- [MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md](MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md) — power architecture, RTC wake cycles, boost converter, pin allocation
- [MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md](MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md) — consolidated PCB schematic review with critical checks
- [MOTHERSHIP_PCB_SCHEMATIC_RECONCILIATION.md](MOTHERSHIP_PCB_SCHEMATIC_RECONCILIATION.md) — schematic vs design note signal name reconciliation
- [MT3608_BROWNOUT_DESIGN_NOTE.md](MT3608_BROWNOUT_DESIGN_NOTE.md) — boost converter brownout behaviour analysis

## LTE Backhaul

- [MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md](MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md) — LTE modem integration and backhaul strategy
- [MOTHERSHIP_A7670G_MODEM_BRINGUP_NOTES.md](MOTHERSHIP_A7670G_MODEM_BRINGUP_NOTES.md) — A7670G hardware bring-up: power, PWRKEY, UART level shifting, SIM, RF, ESD, firmware checklist