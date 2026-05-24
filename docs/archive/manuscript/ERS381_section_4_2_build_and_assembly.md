## 4.2 Build and Assembly

This section describes how the node and mothership units were built for initial testing. The build process focused on reproducibility, low-cost replication, and a clear separation between sensing geometry and serviceable electronics.

### 4.2.1 PCB Design and Fabrication

Custom printed circuit boards (PCBs) were designed in EasyEDA for both node and mothership roles. The node board included the ESP32 microcontroller, power regulation, RTC and sensor interfaces, analog pathways, ultrasonic transmit/receive (TX/RX) circuitry, and connectors for modular expansion. The mothership board included ESP32-based communication, timekeeping, and logging interfaces for network coordination and data aggregation.

Boards were fabricated through JLCPCB using standard prototype workflows. Components were selected to be widely available and affordable, which reduced supply risk and supported repeat builds across multiple units.

To support independent replication, circuit schematics, PCB layout files, bill of materials (BOM), and fabrication outputs (for example, Gerber and drill files) were prepared as supplementary artefacts in the project repository.

Where specific manufacturer process settings (for example, copper weight, solder mask options, or panelisation choices) were not consistently recorded across all prototype iterations, they should be treated as implementation details, not protocol-critical requirements.

### 4.2.2 Electronics Assembly

Before first power-up, assembled boards were visually inspected for solder bridges, incomplete joints, and polarity-sensitive placement errors. Particular attention was given to diode orientation, regulator placement, continuity in the ultrasonic TX/RX path, and sensor connector orientation.

After connectors and sensor leads were installed, initial electrical checks were performed on a bench supply. These checks verified expected supply rails, basic boot behaviour, and serial console output from the microcontroller. Only boards that passed these checks were moved to enclosure integration.

Where V1 assembly rework was required (including polarity and footprint-related issues identified during early bring-up), corrected units were re-checked from first power and rail verification before further integration.

### 4.2.3 Housing Design and Fabrication

The enclosure was designed in FreeCAD and manufactured using fused deposition modelling (FDM) 3D printing. This approach supported rapid design iteration, low-cost reproduction, and fabrication without specialised tooling.

The housing architecture followed a head-roof-belly layout. The head contained the ultrasonic sensing geometry and transducer pod interfaces. The roof provided shielding and mounted the PAR/diffuser assembly through integrated standoffs and seating features. The belly enclosed the electronics and battery, with features for gasket sealing, internal cable management, and board mounting.

CAD source files and STL outputs were treated as reproducibility artefacts and were prepared as supplementary materials in the project repository.

Where print-process parameters (for example, filament type, nozzle size, and exact layer settings) differed between prototype runs, they were treated as tunable fabrication choices rather than fixed methodological requirements.

### 4.2.4 Mechanical Assembly

Mechanical assembly started by installing the node PCB and battery in the belly compartment, followed by seating ultrasonic transducers in the angled pod features of the head assembly. The PAR sensor was mounted in the roof-diffuser subassembly using dedicated mount geometry.

Internal wiring was routed through internal channels and standoff paths so cables stayed outside the primary ultrasonic measurement volume. The roof/head assembly was then coupled to the belly to complete the enclosure.

The architecture was assembled so electronics could be serviced without full disassembly of the wind sensing geometry.

Where exact fastener specifications and torque settings were not systematically logged for all prototypes, assembly was guided by fit integrity and repeatable seating rather than fixed torque targets.

### 4.2.5 External Sensors and Cable Routing

External sensor interfaces were implemented as modular expansion ports to support variable deployment payloads. Cable glands or connector interfaces were positioned on lower enclosure flat-pad regions to keep cable penetrations stable and separated from the ultrasonic sensing head.

External cable routing was managed to avoid intrusion into the acoustic measurement region and to reduce local flow disturbance near the ultrasonic path. In this workflow, sensor and cable placement were treated as part of measurement design, not only as mechanical assembly.

Where site-specific external sensor placement protocols were not fully standardised, deployment teams were advised to document probe position, cable path, and orientation at installation time for interpretation of microclimate gradients.

### 4.2.6 Sealing and Weather Resistance

Weather resistance was implemented through a sealed lower electronics compartment, a continuous silicone cord gasket seated in the enclosure groove, and controlled cable-entry points via glands/connectors.

Sealing quality was assessed during assembly and commissioning through fit checks and short-duration operation. Formal ingress testing to an IP-standard protocol was not performed in this phase, and no formal waterproof or dustproof rating is claimed.

Accordingly, enclosure performance should be interpreted as field-oriented protective sealing rather than certified environmental hardening.

### 4.2.7 Pre-Commissioning Checks

Before full network bring-up, each assembled unit underwent a pre-commissioning check sequence:

1. Visual inspection of assembly integrity, connector seating, and cable strain relief.
2. Verification of expected supply rails under controlled power input.
3. Confirmation of microcontroller boot behaviour and serial output.
4. Basic communication checks across core buses and wireless control pathways.

These checks established minimum functional readiness and provided the transition to the staged bring-up procedure described in Section 4.3.
