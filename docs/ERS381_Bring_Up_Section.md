## 4.4 Bring-up and commissioning workflow

To support reproducibility across research teams with varying electronics experience, system bring-up was formalised as a staged commissioning workflow conducted before ecological deployment. The objective of bring-up was not sensor calibration against reference-grade instruments, but verification that each node and the mothership could operate as an integrated measurement system under realistic field constraints. This process reduced deployment risk, improved traceability of early failures, and provided a consistent baseline for interpreting subsequent data.

### 4.4.1 Bring-up goals

Bring-up focused on five practical goals:
1. Confirming electrical and communication integrity of each assembled node.
2. Verifying that core sensors produced plausible, stable outputs.
3. Confirming reliable node discovery, pairing, and deployment through the mothership interface.
4. Verifying scheduled wake/sync behaviour and data logging continuity.
5. Recording a repeatable pass/fail record for each unit prior to field use.

### 4.4.2 Staged procedure

Bring-up was performed in four stages.

**Stage A: Hardware and power integrity check**
- Visual inspection of solder joints, connector seating, enclosure seals, and cable routing.
- Verification of supply rails and regulator behaviour under expected load.
- Confirmation of microcontroller boot, serial output, and stable reset behaviour.

**Stage B: Sensor and interface verification**
- I2C bus scan and device detection checks for connected digital sensors.
- Analog channel checks for expected range and response direction (including soil channels where fitted).
- Functional response checks using simple environmental stimuli (e.g., hand shading for PAR response, ambient thermal change for air temperature/humidity).
- For ultrasonic wind hardware, confirmation of acquisition pathway functionality and timing stability under bench conditions.

**Stage C: Network bring-up and control-path validation**
- Node discovery from the mothership dashboard.
- Pairing and deployment transitions verified through the node lifecycle states.
- Confirmation that schedule commands (wake/sync intervals) propagated correctly and were retained after reset.
- Time synchronisation checks between mothership and nodes.

**Stage D: Logging and persistence checks**
- Verification that measurements were transmitted and written to central CSV logs.
- Confirmation that records included expected node/sensor metadata.
- Power-cycle test to confirm deterministic restart and state persistence.
- Short unattended run to check for missed cycles, stalled logging, or communication dropouts.

### 4.4.3 Pass criteria

A unit was considered bring-up complete only when all of the following were satisfied:
- Node could be discovered, paired, deployed, and reverted without manual firmware intervention.
- Core sensors produced plausible non-null signals over repeated acquisition cycles.
- Data packets were received and archived by the mothership without sustained packet-loss behaviour.
- Scheduled operation persisted across reset/power interruption.
- No recurring watchdog resets, bus lockups, or storage write failures were observed during the commissioning window.

### 4.4.4 Failure handling and iteration

Failures detected during bring-up were classified into three categories:
- **Assembly issues** (e.g., wiring, connector, solder defects)
- **Configuration issues** (e.g., addressing, schedule settings, node state mismatches)
- **Firmware/runtime issues** (e.g., communication edge cases, queue/logging behaviour)

Units failing any stage were removed from deployment, corrected, and re-tested from Stage A. This full re-run approach avoided partial acceptance of units with unresolved upstream issues.

### 4.4.5 Documentation outputs from bring-up

Each unit generated a brief commissioning record containing:
- Hardware identifier and firmware build information.
- Date/time of bring-up and operator initials.
- Stage outcomes (A-D), pass/fail status, and observed anomalies.
- Corrective actions applied (if any).
- Final deployment readiness decision.

This record created an auditable link between physical unit history and field data streams, improving interpretation of early deployment behaviour and supporting transparent replication by other groups.

### 4.4.6 Scope of interpretation

The bring-up process establishes functional readiness and systems integration confidence. It does not replace formal metrological calibration, uncertainty quantification, or long-duration durability testing. Those activities remain separate components of full validation and are addressed in the discussion of limitations and future work.
