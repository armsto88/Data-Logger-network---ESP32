# Mechanical Design Document — Ultrasonic Anemometer Node

## System Architecture Summary

The ultrasonic anemometer mechanical assembly is a three-part housing that preserves a fixed roof-reflection acoustic geometry while integrating environmental sensing hardware for field deployment.

Primary mechanical elements:

- Head: ultrasonic transducer pod structure and acoustic launch geometry
- Roof: reflective acoustic surface and PAR sensor mounting region
- Belly: sealed electronics compartment with PCB standoffs and cable management

This geometry supports reciprocal TOF measurements along controlled paths:

`pod -> roof -> opposite pod`

---

## Key Subsystems and Purpose

1. Ultrasonic transmit geometry: sets repeatable launch angle for burst propagation.
2. Ultrasonic receive geometry: preserves first-arrival path and reduces obstruction effects.
3. MCU/electronics enclosure: protects node electronics and maintains serviceability.
4. Power/environment sealing features: support outdoor operation and cable ingress protection.
5. Aerodynamic housing form: reduces self-disturbance to local wind field.
6. Measurement geometry lock: ensures firmware constants remain valid over builds.

---

## Housing Architecture

### Head

The head carries transducer pods at fixed tilt and spacing to enforce deterministic acoustic path geometry.

Design goals:

- maintain clear line-of-flight to reflector
- minimize near-field obstruction near pod bores
- keep pod orientation stable under vibration and thermal cycling

### Roof

The roof functions as the acoustic reflector plane and includes integration space for optical environmental sensing (PAR region).

Current reflector guidance from beam-trace diagnostics:

- flat reflector retained as baseline
- weak focusing alternatives (`dish`, `cone`) currently not selected

### Belly

The belly is the sealed electronics chamber for PCB, power, and interconnects.

Design goals:

- weather protection
- robust mount points/standoffs
- wiring containment and strain relief

Assumption / To Be Verified:

- Final gasket and venting strategy for long-duration field humidity control.

## Ultrasonic Measurement Geometry

Current validated geometry snapshot:

- Path length: `L = 146.7 mm`
- Pod tilt: `~32.3 deg`
- Projection factor: `p = |sin(32.3 deg)| = 0.5344`
- Nominal no-wind TOF: `t0 ~ 427 us` (20 deg C assumption)

Expected reciprocal TOF split:

- 1 m/s -> `~1.33 us`
- 5 m/s -> `~6.65 us`
- 10 m/s -> `~13.3 us`

Mechanical implication:

- Small geometric drift can shift microsecond-scale timing; pod tilt, reflector height, and spacing must be controlled tightly between builds.

## Mechanical Constraints Supporting Firmware Timing

To support first-arrival timing precision:

- maintain stable pod-to-roof and pod-to-pod geometry
- avoid adding structures that increase near-path reflections
- keep standoff and roof-feature interference low around computed acoustic footprints
- preserve alignment validated in FreeCAD TOF diagnostics

Observed diagnostics context (from current workflow):

- standoff interactions are non-zero in Monte-Carlo ray trace
- roof footprint overlap with PAR regions is non-trivial
- comparator gating/windowing in firmware remains necessary

## Environmental and Deployment Objectives

Mechanical design targets for ecological field use:

- minimize aerodynamic disturbance around measurement volume
- protect electronics from rain, dust, and UV exposure
- allow integration with broader sensor node systems
- support repeatable assembly and maintenance workflows

Assumption / To Be Verified:

- Full ingress rating target (for example IP class) for production enclosure.

## Mechanical-to-Hardware Interface Notes

- Transducer mounting and routing must preserve low-noise RX behavior.
- Cable and connector placement must not intrude into acoustic paths.
- Structural features near the roof should be validated against beam-trace impact before release.
- Any change to pod tilt, reflector height, or pod radius requires TOF constant regeneration and firmware update.

## Revision Control Guidance

When mechanical geometry changes:

1. Re-run `FreeCAD/MACRO.py` diagnostics.
2. Update TOF constants (`L`, `p`, `t0`) in documentation and firmware.
3. Re-check low-wind split detectability versus timer jitter budget.
4. Revalidate reflector/interference metrics before field deployment.
