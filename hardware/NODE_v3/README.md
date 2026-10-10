# Node v3 hardware

This directory contains fabrication releases for the ESP32 Node v3 board.

## Releases

| Release | Contents | Review |
|---|---|---|
| `revisions/2026-10-09` | Field-proven EasyEDA Pro baseline, Gerbers, BOM, and pick-and-place data | [Ultrasonic and 22 V removal plan](revisions/2026-10-09/REVIEW.md) |
| Repository-root files dated `2026-06-28` | Earlier Gerber, BOM, and pick-and-place export retained for history | Not reviewed here |

The supplied source and manufacturing files in the dated revision were copied
without modification. The review records how to remove the ultrasonic TX/RX
and 22 V circuitry from a new working copy; it does not modify the archived,
field-proven baseline.

Do not overwrite the baseline project. After completing the removal, store the
new EasyEDA source and regenerated manufacturing outputs in a separate dated
revision and verify them against the removal checklist before fabrication.
