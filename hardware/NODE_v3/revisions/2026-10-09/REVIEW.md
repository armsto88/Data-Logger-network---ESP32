# Node v3 ultrasonic and 22 V removal plan — 2026-10-09 baseline

- **Review date:** 2026-10-09
- **EasyEDA source:** `NODE_ProPrj_NODE_V3_2026-10-09.epro2`
- **Release directory:** `hardware/NODE_v3/revisions/2026-10-09`
- **Baseline status:** Reported field-proven and working

**Recommendation for this pass:** preserve this release unchanged as the
known-good baseline. Make the deletions below in a new EasyEDA project copy,
then submit a new source/BOM/CPL/Gerber set for a removal-only comparison.

The scope is deliberately limited to:

1. the ultrasonic transmit chain;
2. the ultrasonic receive chain;
3. the 22 V converter and its enable circuit; and
4. the two 22 V sensor-supply branches that become unusable when the 22 V rail
   is removed.

This review does **not** assess or redesign the circuits that remain. In
particular, it does not change the 3.3 V or 5 V regulators, charger, power
latch, RTC, USB interface, ADC signal inputs, I2C expansion, or auxiliary reed
wind input.

## Baseline integrity

The four supplied files were copied byte-for-byte into this release directory.

| Check | Result |
|---|---|
| EasyEDA source archive | Pass; ZIP structure tests successfully |
| Gerber archive | Pass; all 17 members test successfully |
| Copper/drill set | Four copper layers, top/bottom mask, top paste, top/bottom silk, outline, PTH, NPTH, and via drill data present |
| BOM | 103 groups and 331 unique references |
| Pick-and-place | 331 unique references; exact reference-set match to the BOM |
| Source PCB | 331 components and 331 unique IDs; exact unique-ID match to the BOM |
| Assembly side | All 331 placements are top-side |

| File | SHA-256 |
|---|---|
| `NODE_BOM_Board1_PCB1_2026-10-09.csv` | `c7339d9492b1f3b86b2f0fbfe956de930a6b1f6533fb00ff043c2a09640fde88` |
| `NODE_PickAndPlace_PCB1_2026_10_09.csv` | `55df2a648c2dfb119f9d7197b62a48c9e4a2f1668d9c5c9b9b4063d7c366b61e` |
| `NODE_ProPrj_NODE_V3_2026-10-09.epro2` | `1713a1c492eb72fe7e650674b6c31dcb561eb1002ed45b65786aa43dfb0bef30` |
| `NODE_Gerber_PCB1_2026-10-09.zip` | `4f04bba4c7f9eb9765b55f9f3b317c3fa599cc47bda87dca39257a58e5a50767` |

## Removal boundary

The baseline contains 331 placed references. The exact removal set below has
159 references, leaving an expected 172 references in both the BOM and
pick-and-place export.

| Block | Remove | Expected references removed |
|---|---|---:|
| 22 V converter and enable | Entire 22 V block on `POWER _REG:` | 20 |
| Ultrasonic TX | Entire `ULTRASONIC_V2_TX` circuit plus the MCU-side `TX_PWM` test point | 68 |
| Ultrasonic RX | Entire `ULTRASONIC_V2_RX` circuit | 53 |
| 22 V sensor branches | Both 22 V high-side branches on `ADC`; retain their 5 V alternatives | 18 |
| **Total** |  | **159** |

Do not run annotation or renumber the retained components during this work.
Stable references make the removal-only comparison auditable.

## Instructions in EasyEDA Pro

### 1. Create the working revision

1. Import/open `NODE_ProPrj_NODE_V3_2026-10-09.epro2`.
2. Immediately save a copy under a new project/revision name.
3. Do not edit or overwrite the archived file in this directory.
4. Before changing anything, confirm that all nine schematic pages and `PCB1`
   are present.

### 2. Remove the 22 V converter and enable circuit

On `POWER _REG:`, remove only the complete block labelled `22 V`. Keep the
neighbouring `3.3 V` and `5 V` blocks intact.

Delete these 20 references:

```text
C1, C2, C81, C86, C129
D7
L1
R70, R71, R170, R171, R172, R173
U1, U2, U22, U49
22V, EN_22, TX_22_EN
```

`U1` and `U2` are 22 uF capacitors despite their `U` designators. `C1` and
`C81` are on `VSYS`, but they are the local input capacitors inside the 22 V
converter block; do not confuse them with the retained system and regulator
decoupling elsewhere.

Remove the associated schematic labels and PCB copper for:

```text
22V_SYS
EN_22
TX_22V_EN_N
```

Do not remove the 5 V converter (`U21`, `L4`, `D6`, `R72`, `R73`, and its
capacitors). `5V_SYS` is still used by the retained sensor interfaces.

### 3. Remove the ultrasonic transmit chain

On `ULTRASONIC_V2_TX`, remove the entire functional circuit. After its
components have been removed from the design, the empty schematic page may
also be deleted.

Delete these 68 references; `TX_PWM` is the test point located on the `MCU`
sheet and is included here because it serves only this chain:

```text
C111-C113
CN11-CN14
D10-D14
Q16, Q17, Q21, Q22, Q26-Q35
R5-R9, R11, R111-R114, R116-R139, R182
U46
GND1, PWM_5v, TP3, TP4, TX_pulse, TX_PWM
```

Remove all resulting TX-only labels, routing, and vias, including:

```text
TX_PWM, PWM_5V, TX_PULSE, HS_GATE
DRV_N, DRV_E, DRV_S, DRV_W
REL_N, REL_E, REL_S, REL_W
DRV_GATE_N, DRV_GATE_E, DRV_GATE_S, DRV_GATE_W
TD_N_A, TD_N_B, TD_E_A, TD_E_B
TD_S_A, TD_S_B, TD_W_A, TD_W_B
```

This removes the four transducer connectors `CN11-CN14`, the 5 V gate driver,
the 22 V pulse switch, all four directional drivers, and all four damping
drivers.

### 4. Remove the ultrasonic receive chain

On `ULTRASONIC_V2_RX`, remove the entire functional circuit. After its
components have been removed from the design, the empty schematic page may
also be deleted.

Delete these 53 references:

```text
C4-C6, C15, C99-C110, C130, C131
D8, D9
R1-R4, R100-R109, R152, R174-R176, R178, R179
SJ6, SJ7
U42, U43, U44, U50, U51
GND2, RX, RX_EN, RX_IN, TOF, VREF
```

This removes the 74HC4052 mux, protection network, both TLV9062 amplifier
stages, mid-rail reference, comparator, blanking gates, configuration jumpers,
and receiver-only test points.

Remove the resulting RX-only labels, routing, and vias, including:

```text
RX_N_A, RX_N_B, RX_E_A, RX_E_B
RX_S_A, RX_S_B, RX_W_A, RX_W_B
MUX_A, MUX_B
RX_HOT, RX_COLD, RX_IN
ST1_IN, ST1_NEG, ST1_OUT, ST2_IN, ST2_NEG
RX_AMP, COMP_RAW, RX_WINDOW_EN, TOF_EDGE, VREF
```

#### Shared-signal exception: retain the auxiliary reed input

Do **not** blindly delete the whole `RX_EN_N` net. On the `MUX` sheet it is
also connected to the independent auxiliary reed-wind input through `SJ8`, and
`R177` provides its pull-up on the `MCU` sheet.

For this removal-only pass:

- keep `SJ8`;
- keep the connector currently named `edaimagecopyoccupy`;
- keep `R177`;
- keep the GPIO4 connection; and
- remove only the `RX_EN` test point and the branches that ran to `U42` and
  `U50`.

The surviving net can be renamed from `RX_EN_N` to an auxiliary-wind name in a
later review. Leaving the name unchanged for this pass avoids mixing a
functional change into the deletion work.

### 5. Remove the two remaining 22 V sensor branches

The `ADC` sheet uses `22V_SYS` in two selectable high-side sensor supplies.
Those branches cannot remain after the converter is removed. Delete these 18
references:

```text
D16, D18, D19, D20
Q18, Q19, Q20, Q36
R141-R148
SW11, SW12
```

Channel 1 must retain its existing 5 V path:

```text
5V_SYS -> D17 -> V_SENS_01 -> CN15
```

Channel 2 must retain its existing 5 V path:

```text
5V_SYS -> D15 -> V_SENS_02 -> CN16
```

Keep `D15`, `D17`, `CN15`, `CN16`, `U30`, the ADC input filters, and the
`A1_V`/`A2_V` test points. Remove the now-unused `MAIN_SW_01`, `MAIN_SW_02`,
`SEL_MAIN_01`, and `SEL_MAIN_02` labels and copper.

This step is a deletion only. Any later decision about sensor supply voltage,
diode drop, connector assignment, or protection belongs to the next review.

### 6. Disconnect the now-unused MCU control signals

On the `MCU` sheet, remove the obsolete net labels from these ESP32 pins and
mark the pins intentionally unconnected. Do not reassign them in this pass.

| ESP32 pin | Remove label |
|---:|---|
| GPIO5 | `TX_22V_EN_N` |
| GPIO13 | `DRV_W` |
| GPIO14 | `DRV_S` |
| GPIO16 | `MUX_A` |
| GPIO17 | `MUX_B` |
| GPIO21 | `REL_S` |
| GPIO22 | `REL_W` |
| GPIO25 | `TX_PWM` |
| GPIO26 | `DRV_N` |
| GPIO27 | `DRV_E` |
| GPIO32 | `REL_E` |
| GPIO33 | `REL_N` |
| GPIO34 | `TOF_EDGE` |

Keep GPIO4 and its present `RX_EN_N` connection because of the auxiliary reed
input described above.

### 7. Synchronise and clean the PCB

Run the schematic-to-PCB update only after the schematic removals are complete.
Inspect the change list before accepting it.

The removal-only update should:

- remove exactly 159 placed references;
- add no components;
- change no retained component values or footprints; and
- leave 172 placed references.

If the update proposes retained-part replacements, reference changes, or
unrelated net changes, cancel it and resolve the schematic mismatch first.

After accepting the component removals:

1. Delete every stale trace, via, copper region, and silkscreen item belonging
   only to the removed nets.
2. Re-pour copper zones and confirm that no zone remains assigned to a removed
   net.
3. Keep the board outline and all retained component placement unchanged for
   this pass. The empty area can be repacked later, after the deletion diff is
   verified.
4. Run EasyEDA ERC and DRC.
5. Inspect the board in 2D and 3D to ensure all four ultrasonic connectors and
   both 22 V switch areas are gone.

## Acceptance checklist for the next export

- [ ] The archived 2026-10-09 baseline remains byte-for-byte unchanged.
- [ ] The new project contains no `ULTRASONIC_V2_TX` or `ULTRASONIC_V2_RX`
      functional circuitry.
- [ ] `U22` and all `22V_SYS` generation/distribution circuitry are absent.
- [ ] `CN11-CN14` are absent.
- [ ] The two 22 V ADC-sheet branches are absent.
- [ ] `D17` still supplies `V_SENS_01` from `5V_SYS`.
- [ ] `D15` still supplies `V_SENS_02` from `5V_SYS`.
- [ ] `SJ8`, `R177`, the auxiliary reed connector, and GPIO4 remain connected.
- [ ] No retained power, charging, ADC, I2C, USB, RTC, or latch circuitry was
      intentionally changed.
- [ ] The new BOM and pick-and-place files contain exactly 172 unique
      references and have identical reference sets.
- [ ] None of the 159 references listed in this document appears in the new
      BOM, pick-and-place file, or PCB.
- [ ] No removed net name remains in the schematic or PCB, except the retained
      shared name `RX_EN_N`.
- [ ] ERC and DRC reports contain no new errors caused by dangling labels,
      stale copper, or unconnected retained inputs.
- [ ] New source, BOM, pick-and-place, and Gerber files are stored in a new
      dated revision for comparison before any remaining-circuit review.

## Explicitly deferred

The following work is intentionally deferred until the removal-only revision
passes the checklist:

- electrical review of the retained circuits;
- reassignment of freed ESP32 GPIOs;
- renaming or redesigning the auxiliary reed input;
- changes to the retained 5 V sensor supplies;
- component movement, board shrinking, or connector relocation;
- firmware cleanup; and
- fabrication approval for the revised board.
