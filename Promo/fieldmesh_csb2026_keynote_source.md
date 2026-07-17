# FieldMesh CSB 2026 Keynote — Project Source File

## Purpose
Develop a 15-minute keynote presentation for the Conference on Solar Energy and Biodiversity 2026 (CSB 2026) on **FieldMesh**, a low-cost field data-logging system for fine-scale environmental monitoring in solar farms.

This source file should guide all future ChatGPT work on the talk: narrative development, slide structure, speaker notes, visuals, diagrams, and refinement of scientific framing.

---

## Conference context

**Event:** Conference on Solar Energy and Biodiversity 2026 — “Managing Impacts and Promoting Solutions”  
**Dates:** 28–29 October 2026  
**Location:** NEWCAP Center, Paris, France  
**Conference focus:** The interactions between solar energy development and biodiversity, bringing together researchers, industry professionals, policymakers and other stakeholders to share knowledge, explore challenges, and develop science-based solutions for integrating biodiversity into solar projects.  
**Official site:** https://www.conference-solar-biodiversity.org/

The talk should feel practical, applied, and solutions-focused. It should sit at the intersection of biodiversity science, solar farm management, field monitoring, and open/affordable technology.

---

## Presentation format

**Format:** 15-minute keynote  
**Target length:** Approximately 12–15 slides  
**Tone:** Clear, confident, practical, scientific but accessible  
**Audience:** Mixed audience of ecologists, solar developers, land managers, consultants, researchers, policy/industry stakeholders, and biodiversity professionals.  
**Primary goal:** Show why fine-scale, consistent microclimate monitoring is needed in solar farms, and how FieldMesh was developed as a direct response to that need.

The talk should avoid sounding like a product pitch. It should tell a scientific and practical story: a literature gap was identified, a monitoring problem became clear, and FieldMesh is one attempt to make better data collection possible at the scale needed.

---

## Core thesis

Solar farms create spatially complex microclimates, but current monitoring approaches are often too inconsistent, too sparse, too expensive, or too difficult to repeat across sites. FieldMesh was developed to make fine-scale, reliable, and affordable environmental monitoring more practical for solar biodiversity research and site management.

---

## Research origin

The presentation is grounded in a review paper co-authored by the presenter:

**Paper:** *A mosaic of microclimates: biodiversity outcomes and wildlife habitat potential in large-scale solar facilities*  
**Journal:** Biological Reviews  
**DOI:** https://onlinelibrary.wiley.com/doi/10.1002/brv.70171

### Key paper framing to use
The review synthesised current knowledge on microclimates within photovoltaic solar facilities and identified major research needs. It found that solar facilities can create spatially heterogeneous microclimates, affecting variables such as temperature, humidity, wind speed, soil moisture, albedo, and photosynthetically active radiation. However, generalisation is limited by small sample sizes, varied sensor placement, inconsistent study designs, and differences in reporting. The review highlights the need for more standardised microclimate research and reporting across solar facilities.

### Important framing
FieldMesh should be introduced as a response to the methodological gap identified in the review:

> “If we want to understand how solar farms affect biodiversity, we first need to understand the fine-scale environmental conditions that organisms actually experience.”

---

## Problem statement

Solar farms are not uniform landscapes. Beneath panels, between rows, along edges, in restored grassland patches, under different vegetation treatments, and across management zones, conditions can vary substantially.

However, monitoring this variation is difficult because:

- Standard weather stations are too coarse and often too expensive to deploy densely.
- Commercial environmental monitoring systems can be costly and may not scale across many points.
- Many ecological studies use different sensor types, heights, placements, sampling intervals, and reporting formats.
- Remote solar sites often lack mains power and reliable internet.
- Field systems must survive weather, low power, intermittent connectivity, and limited maintenance.
- Data loss or sensor failure can silently undermine interpretation.

The result is a data gap: we often discuss solar farm biodiversity outcomes without enough consistent, fine-scale environmental data to explain the mechanisms behind them.

---

## One-sentence FieldMesh description

**FieldMesh brings reliable, detailed environmental data back from hard-to-reach field sites, keeps it safe even when the internet fails, and makes it available through a simple web dashboard — without requiring users to be technicians.**

---

## System overview

FieldMesh has two main parts:

### 1. Sensor nodes
Small, battery-powered environmental monitoring units placed around a site. Each node wakes briefly on its own schedule, records data, transmits to the hub, and returns to deep sleep or full power-off. This maximises battery life and supports unattended deployment.

### 2. Field hub
An on-site hub that collects readings from nodes over a short-range wireless link, stores a permanent local copy, and syncs accumulated data to the cloud when cellular or internet connectivity is available. The hub also hosts the local setup interface used in the field.

### 3. Web dashboard
Once data reaches the cloud, users can view live and historical data from a phone, tablet, or laptop in a browser. No specialist software or mobile app is required.

---

## What FieldMesh measures

Each node is designed to build a rounded picture of local environmental conditions:

- **Air:** temperature and relative humidity just above ground or canopy level
- **Light:** spectral light data across visible wavelengths, not just brightness
- **Soil:** soil moisture and soil temperature at two probe depths
- **Wind:** wind speed; wind direction is in development
- **Device health:** battery state, connection status, reporting status, and sensor health

Emphasise that device health is part of the ecological data workflow. A missing reading should not be a mystery. The system should help users distinguish ecological patterns from technical failure.

---

## Development status

FieldMesh is in active development.

Built and working on prototype hardware:

- Core firmware
- Sensor-node wake, read, transmit, sleep cycle
- Hub-based node discovery, pairing, naming, and deployment
- Local hub interface via Wi-Fi
- Local data logging
- Cloud sync
- Web dashboard
- Prototype hardware deployments/testing

Still being finished:

- Wind direction channel
- Final production hardware run
- Further enclosure refinement
- Sensor calibration and validation workflows
- Broader field validation

Use honest language. Do not overstate production readiness. The system is a working prototype moving toward field-ready deployment.

---

## Design principles

### 1. Dense monitoring should be affordable
Fine-scale monitoring needs many points, not one perfect station. FieldMesh prioritises deployability, cost control, and scalability.

### 2. Field systems should be usable by ecologists
Setup should not require firmware flashing, command-line tools, or specialist electronics knowledge.

### 3. Data should survive bad connectivity
Remote sites cannot rely on constant internet. Data must be stored locally first, then synced later.

### 4. Low power matters
Nodes are designed to spend almost all their time asleep or powered off, allowing long deployment periods from batteries.

### 5. Trust requires health metadata
Battery, reporting, and sensor status should be captured alongside environmental readings.

### 6. Monitoring should become repeatable
The system should help standardise how microclimate data are collected across sites, treatments, and studies.

---

## Suggested keynote narrative arc

### Act 1 — The ecological question
Solar farms are expanding quickly, and they are increasingly expected to deliver biodiversity outcomes as well as renewable energy. But biodiversity outcomes depend on the conditions organisms experience at fine scales.

### Act 2 — The evidence gap
The review paper showed that solar facilities can create mosaics of microclimates, but the literature is difficult to compare because monitoring methods are inconsistent. We do not yet have enough standardised, fine-scale, repeatable monitoring.

### Act 3 — The practical barrier
The missing data are not missing because ecologists do not care. They are missing because field monitoring is hard: cost, power, connectivity, sensor placement, maintenance, and data reliability all get in the way.

### Act 4 — The system response
FieldMesh was developed to reduce those barriers: many low-cost sensor nodes, one hub, local-first data storage, delayed cloud sync, simple setup, and a dashboard designed for field users.

### Act 5 — What this enables
With systems like FieldMesh, solar biodiversity research can move from isolated measurements to repeatable environmental context: comparing panel shade, row gaps, vegetation treatments, edges, restoration zones, and management interventions across sites.

### Act 6 — Closing message
Better biodiversity decisions need better environmental context. If solar farms are mosaics, our monitoring systems need to be able to see the mosaic.

---

## Proposed slide outline

### Slide 1 — Title
**FieldMesh: Fine-scale environmental monitoring for solar biodiversity landscapes**  
Subtitle: From a literature gap to a practical field data-logging system

Visual: Solar farm landscape with overlaid sensor nodes / environmental gradients.

### Slide 2 — The biodiversity promise of solar farms
Solar farms are increasingly discussed not only as energy infrastructure, but as managed landscapes with biodiversity potential.

Key point: To manage biodiversity well, we need to understand the physical conditions created by solar infrastructure and vegetation management.

### Slide 3 — Solar farms are microclimate mosaics
Panels, shade, vegetation, soil, wind exposure, row spacing, and edge effects create local variation.

Visual: Simple conceptual diagram showing under-panel, between-row, edge, and open reference locations.

### Slide 4 — What the review found
The review identified effects on temperature, humidity, wind speed, soil moisture, albedo, and light/PAR, but also showed that findings are hard to generalise.

Key phrase: “The problem is not only what we measure — it is how consistently we measure it.”

### Slide 5 — The monitoring gap
Current studies differ in sensor placement, sampling frequency, variable selection, reporting standards, and spatial design.

Visual: “one weather station” versus “distributed field network”.

### Slide 6 — Why the gap exists
Monitoring at this scale is hard because of cost, power, connectivity, maintenance, weatherproofing, calibration, and data reliability.

This slide should create empathy: the gap is practical, not just scientific.

### Slide 7 — FieldMesh: design response
Introduce FieldMesh as a practical response: low-cost nodes, one hub, local-first logging, delayed sync, browser dashboard.

Visual: System architecture diagram.

### Slide 8 — Sensor nodes
Explain what each node measures and why these variables matter for microclimate and habitat interpretation.

Include: air, light spectrum, soil, wind, device health.

### Slide 9 — Field hub and data flow
Show wake → measure → transmit → confirm → store locally → sync to cloud → dashboard.

Emphasise: no silent data loss; internet can fail without losing readings.

### Slide 10 — Field setup experience
Show how the hub hosts its own Wi-Fi setup page: pair nodes, name them, deploy them, set recording intervals.

Key point: designed for ecologists and field teams, not only engineers.

### Slide 11 — Dashboard and interpretation
Show dashboard concept: live values, trends, battery status, node health, and comparison across locations.

Key point: trust in data depends on knowing which nodes are healthy.

### Slide 12 — What this enables in solar biodiversity research
Examples:

- Compare under-panel, between-row, edge, and reference conditions
- Monitor vegetation management treatments
- Link microclimate to plant, invertebrate, bird, bat, or reptile outcomes
- Support habitat suitability modelling
- Evaluate restoration and operational management over time

### Slide 13 — Current status and next steps
Be transparent: working prototype, active development, wind direction in progress, final hardware run, calibration/validation and field deployments ahead.

### Slide 14 — Bigger vision
Standardised, affordable monitoring could support shared datasets across solar farms and make biodiversity claims more evidence-based.

### Slide 15 — Closing
**If solar farms are mosaics, our monitoring systems need to see the mosaic.**

---

## Visual style direction

The presentation should feel:

- Scientific but not sterile
- Field-based and practical
- Clean, modern, and credible
- More like applied ecology + open technology than a corporate product deck

Suggested visual language:

- Solar farm landscapes
- Simple environmental gradient diagrams
- Field hardware photos or renders
- Node/hub/data-flow diagrams
- Dashboard screenshots
- Minimal text per slide
- Strong speaker notes carrying the detail

Avoid:

- Dense technical schematics too early
- Overly promotional sales language
- Too many bullet-heavy slides
- Claiming the system is fully commercial/finished

---

## Key phrases to reuse

- “The microclimate is where infrastructure meets ecology.”
- “A single weather station cannot describe a mosaic.”
- “The monitoring gap is not only scientific — it is practical.”
- “FieldMesh was built to make dense monitoring realistic.”
- “Local-first data logging means bad signal does not equal lost data.”
- “Device health is part of data trust.”
- “If solar farms are mosaics, our monitoring systems need to see the mosaic.”

---

## Possible title options

1. **FieldMesh: Seeing the microclimate mosaic inside solar farms**
2. **From review gap to field system: building FieldMesh for solar biodiversity monitoring**
3. **Monitoring the mosaic: low-cost microclimate networks for solar biodiversity landscapes**
4. **FieldMesh: Practical microclimate monitoring for biodiversity-positive solar farms**
5. **Beyond the weather station: fine-scale monitoring for solar farm biodiversity**

Recommended title: **Monitoring the mosaic: FieldMesh for fine-scale solar biodiversity data**

---

## Speaker positioning

Presenter: Tom Armstrong  
Perspective: Ecologist and conservation technologist working across field ecology, software, electronics, and environmental monitoring systems.

The talk should position the presenter as someone who saw a methodological problem through research, then started building the practical infrastructure needed to address it.

Do not over-frame this as “I invented the solution.” Instead frame it as:

> “This is one attempt to make the kind of monitoring we are asking for in the literature more feasible in the field.”

---

## Technical details to include selectively

Use only when they help the story:

- Low-power node architecture
- Scheduled wake/measure/transmit/sleep cycle
- Hub-based coordination
- Local permanent storage
- Store-and-forward cloud sync
- Browser-based dashboard
- Short-range wireless node-to-hub communication
- Battery and connection status monitoring
- Sensor modularity
- Open/low-cost hardware approach

Do not overload the keynote with circuit-level detail unless specifically developing a technical appendix.

---

## Claims to be careful with

Avoid saying:

- “FieldMesh solves microclimate monitoring.”
- “The system is fully validated.”
- “It is production ready.”
- “It is the cheapest/best system.”
- “It guarantees standardisation.”

Prefer:

- “FieldMesh is being developed to reduce practical barriers.”
- “The prototype is working, with validation and production refinement ongoing.”
- “The aim is to make dense monitoring more realistic.”
- “Systems like this can support more consistent monitoring designs.”

---

## Strong closing paragraph option

The review that started this work made one thing clear: solar farms are not single, uniform environments. They are mosaics of shade, heat, moisture, airflow, vegetation, and management. But if our monitoring is sparse, inconsistent, or too expensive to repeat, we cannot see that mosaic clearly enough to manage it. FieldMesh is my attempt to close part of that gap — not by making monitoring more complex, but by making dense, reliable environmental data easier to collect, easier to trust, and easier to use.

---

## Future ChatGPT task instructions

When helping develop this keynote:

1. Keep the narrative grounded in the review paper and the monitoring consistency gap.
2. Make the system feel like a practical scientific response, not a commercial sales pitch.
3. Use simple, confident language.
4. Prioritise visual storytelling over text-heavy slides.
5. Keep a clear 15-minute structure.
6. Include speaker notes where useful.
7. Be honest about active development status.
8. Emphasise affordability, local-first reliability, field usability, and standardisation potential.
9. Link technical features back to ecological research value.
10. Use the “mosaic” metaphor as the main narrative thread.
