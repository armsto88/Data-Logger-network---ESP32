# Handoff prompt — backend/frontend verification for the HardwareX manuscript

**Paste this ahead of `FIELDMESH_HARDWAREX_MANUSCRIPT_2026-07-26.md` when opening the backend/frontend repo.**

---

## Context

The attached document is a draft manuscript for *HardwareX* (Elsevier) describing the FieldMesh microclimate
monitoring platform. It was written and fact-checked against the **firmware** repository
(`Data-Logger-network---ESP32`), where every device-side claim has been verified against source.

The manuscript contains claims about the **cloud dashboard and backend** that could not be verified there, because
that code lives in this repository. Those claims are marked `[UNVERIFIED]` — see **§6.6 Cloud dashboard and remote
management**, and the pointers in §2.4 (soil calibration), §6.5 (upload), and §7.6 (timebase).

**This document is not a specification and not a change request.** Nothing here should be implemented, refactored, or
"made to match". The task is the opposite: tell me what this repository *actually does today*, so the manuscript can
be corrected to match reality, or the claim removed. A published paper that overstates what the system does is worse
than a paper that claims less.

## What I need back

For each item below: **confirmed / partly true / not true / not implemented**, with a file-and-line pointer where it
exists. Where a claim is wrong, give me the accurate one-sentence version I can drop into the manuscript.

**A. §6.6 dashboard claims**

1. Projects are containers for one field deployment, private to the owner's account, enforced by Postgres row-level security.
2. A FieldHub is registered by entering or scanning its MAC; the dashboard confirms the match by displaying a hardware code of the form `FieldMesh-<last 6 of MAC>`.
3. A per-hub connection key is generated with a QR code, stored **hashed**, and can be re-minted and re-provisioned if lost or leaked.
4. The dashboard never contacts a device directly — it records durable intent that the hub pulls on its next scheduled check-in.
5. Remote commands run **Queued → Accepted → Applied**, and can be cancelled while still Queued.
6. A sleeping node is not counted as changed until it acknowledges.
7. A paused node displays as *Paused* (not a fault), and shows *Resuming* for one interval after resume.
8. A project-wide recording-interval change is a single coordinated transition that preserves the sync anchor so no readings are lost.
9. Undeploy is local-only by design and is not exposed remotely.
10. Hub self-update over LTE is exposed/managed from the dashboard.

**B. Data-path claims made in the firmware-side text**

11. **Soil moisture** — the node logs raw sensor volts and the **backend** performs the volts → volumetric water content conversion, so calibration curves can be updated without reflashing nodes. Is that conversion implemented? If yes, what curve, and is it per-probe or global? If no, say so — the manuscript currently presents deferred calibration as a design feature.
12. **Spectral metadata** — Clear, NIR, gain, integration time, and the saturation flag arrive and persist as finite values (there was a null-metadata defect fixed 2026-07-04 on the firmware side).
13. **Ingest shape** — the hub posts a flat JSON array of readings plus `{meta, status}`, keyed by mothership UUID and device MAC, with 400/401 treated as non-retryable. Confirm the endpoint contract still matches.
14. **Timebase** — this one matters most. The manuscript states that the hub's DS3231, set from the operator's browser in **UTC**, is the sole clock authority, and that **backend-supplied time never overrides the hub RTC**. A backend `serverTimeUnix` field previously carried *local* time and desynchronised the fleet by two hours. Confirm the backend no longer asserts an authoritative time, and that any human-facing local-time display happens purely in the presentation layer.

**C. Scope question for me to decide**

15. How much of the cloud path can be honestly evidenced today? The firmware side is bench-validated at the transport level (AT handshake, SIM, registration, chunked HTTPS, and a full 1.3 MB cloud-OTA image download), but there is **no completed field-upload campaign** in the firmware repo. If the backend has ingest logs from real deployed hardware, tell me the date range and volume — that would let §6.5 and §7.5 say more. If not, §6.6 will likely be cut from the paper and the on-site CSV log presented as the primary data product.

## Hard constraints

- **Do not open-source or publish this repository.** The backend and dashboard are deliberately staying closed. The published artefacts are the PCB designs, the printed sensor housings, and the device firmware only. Do not add licence files, deposit anything, or reference this repo's contents in the manuscript beyond what I ask for above.
- **Do not change any device-facing contract** to make a manuscript claim true. The firmware is the fixed side here; if the two disagree, I fix the manuscript.
- **Do not treat the manuscript's wording as the intended design.** Some of it is my compression of firmware behaviour and may be subtly wrong about the backend.

## Output format

A short markdown report — numbered to match the list above — with:

- a verdict per item,
- a file/line pointer where the behaviour lives,
- a corrected one-sentence claim wherever mine is wrong,
- and a flat list at the end of any claim I should simply delete.

No code changes, no PRs.
