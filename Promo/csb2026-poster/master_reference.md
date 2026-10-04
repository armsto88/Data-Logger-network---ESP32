# FieldMesh — CSB 2026 Poster Master Reference

**Project:** FieldMesh  
**Presenter:** Tom Armstrong  
**Event:** Conference on Solar Energy and Biodiversity 2026 (CSB 2026)  
**Conference theme:** Managing Impacts and Promoting Solutions  
**Dates:** 28–29 October 2026  
**Location:** NEWCAP Center, Paris, France  
**Current presentation format:** **A0 portrait poster**  
**Poster dimensions:** **84.1 cm wide × 118.9 cm high**  
**Current status:** Working content and design brief

---

# 1. Current poster requirements

The presentation was originally being developed as a 15-minute keynote, but the confirmed format is now an **A0 portrait poster presentation**.

## Poster specifications

- A0 format
- Portrait orientation
- 84.1 cm width × 118.9 cm height
- No specific conference template
- No poster ID needs to be printed on the poster itself; the organising committee will place the assigned number above the poster panel
- No video recording is required
- Poster authors are expected to stand by their posters during the poster sessions
- Main poster session: **Tuesday 28 October 2026 evening**
- Posters remain visible on **Wednesday 29 October 2026**
- The organisers plan to print posters onsite free of charge
- Final poster must be supplied as a **PDF**

## Submission timing supplied by the organisers

The supplied conference brief states:

- final version for onsite printing must be submitted by **14 October 2026**
- elsewhere in the same brief it also says: “we kindly request to receive all final version of your posters by August 20”

These two dates conflict. The October 14 deadline appears to be the operative deadline in the newer wording, but this discrepancy should be confirmed before final submission.

## Submission routes

- Upload through the ExOrdo platform, or
- Email the poster PDF to `info@conference-solar-biodiversity.org`
- Suggested email subject format:
  **[CSB 2026 – POSTER ID XX]**

---

# 2. Core purpose of the poster

The poster should show how **FieldMesh** grew from a scientific monitoring gap identified through a review of microclimate research in solar facilities.

It should not read like a product advertisement.

The desired story is:

**Review → monitoring gap → design requirements → FieldMesh → validation → potential field deployment**

The poster should sit at the intersection of:

- biodiversity science
- solar-farm management
- fine-scale environmental monitoring
- field ecology
- affordable/open technology
- repeatable monitoring methods

The audience is expected to include:

- ecologists
- researchers
- biodiversity professionals
- consultants
- land managers
- solar developers
- industry representatives
- policy stakeholders

The tone should be:

- scientific
- practical
- accessible
- transparent
- solutions-focused
- technically credible without being circuit-level

---

# 3. Preferred title and central message

## Preferred title

# Monitoring the mosaic

Possible subtitle:

**FieldMesh for fine-scale environmental monitoring in solar farms**

Alternative supporting subtitle:

**From a research gap to a modular field monitoring system**

## Core closing message

> **If solar farms are mosaics, our monitoring systems need to see the mosaic.**

This is the central narrative phrase and should ideally appear prominently at the bottom of the poster.

---

# 4. Research origin

The work is grounded in the review paper co-authored by the presenter:

**A mosaic of microclimates: biodiversity outcomes and wildlife habitat potential in large-scale solar facilities**  
*Biological Reviews*  
DOI: https://onlinelibrary.wiley.com/doi/10.1002/brv.70171

## Research framing

The review synthesised current knowledge on microclimates within photovoltaic solar facilities.

Solar facilities can create spatially heterogeneous environmental conditions involving variables such as:

- temperature
- humidity
- wind
- soil moisture
- albedo
- light / photosynthetically active radiation

However, the poster should **not spend much space explaining the ecological effects of each microclimate variable**.

The preferred lead-in is the **monitoring gap**.

The key outcome from the review for this poster is that generalisation across studies is difficult because monitoring methods vary.

Important sources of inconsistency include:

- sensor type
- sensor placement
- sensor height
- sampling interval
- spatial design
- reporting approach
- small sample sizes

## Key research-led framing

The project began with the observation that the literature asks for better, more consistent, fine-scale monitoring, but implementing that monitoring in the field is practically difficult.

A useful sentence:

> **The problem is not only what we measure — it is how consistently and practically we can measure it.**

---

# 5. The monitoring problem

Solar farms are not spatially uniform.

Potentially different conditions may occur:

- beneath panels
- between panel rows
- at panel edges
- at site edges
- in reference areas
- under different vegetation treatments
- in grazing zones
- in mown areas
- in restored vegetation
- in other management zones

But dense monitoring is difficult because of:

- equipment cost
- power requirements
- unreliable connectivity
- field maintenance
- weather exposure
- calibration requirements
- sensor failure
- silent data loss
- setup complexity
- the difficulty of scaling one monitoring point into many monitoring points

A single conventional weather station may provide useful site-scale context but cannot necessarily describe the fine-scale spatial mosaic across a solar facility.

---

# 6. Design requirements derived from the gap

The literature gap became a practical design brief.

## Distributed

Use many monitoring points to capture variation across the site.

## Consistent / repeatable

Support consistent:

- sensor configurations
- recording intervals
- deployment procedures
- health metadata
- data formats

## Reliable

Data should survive:

- poor mobile coverage
- intermittent internet
- power interruptions
- temporary communication failures

## Scalable

Increasing the number of measurement points should not require duplicating all complexity at every location.

Affordability is important mainly because **fine-scale monitoring requires many points**.

The goal is not “cheap sensors for their own sake”.

A useful framing:

> **Fine-scale monitoring needs many points, and many points need to remain affordable and manageable.**

---

# 7. FieldMesh concept

## Core concept

> **Many simple sensor nodes. One coordinating field hub.**

FieldMesh distributes the measurements while centralising much of the network complexity.

## Sensor nodes

Sensor nodes are distributed around the site.

They:

1. power on on a scheduled interval
2. initialise
3. measure environmental conditions
4. transmit data to the hub
5. confirm successful delivery
6. **power off completely**

### Critical technical correction

**The nodes do not use deep sleep between measurements.**

They are **completely powered off**.

An independent timing/power system restores power at the next scheduled recording interval.

This distinction is crucial and must remain consistent in all poster text and diagrams.

Preferred wording:

> **The lowest-power operating state is not sleep — it is off.**

or

> **Nodes are completely powered off between measurements.**

Avoid:

- “deep sleep”
- “sleep mode”
- moon/sleep visual metaphors for the node cycle

A suitable visual timeline is:

**OFF ━━━━━ ON: measure + transmit ━ OFF ━━━━━ ON ━ OFF**

---

# 8. Field hub

The field hub is the coordinating point for the network.

It:

- receives readings from sensor nodes
- coordinates node discovery and deployment
- stores a permanent local copy of measurements
- hosts the field setup interface
- synchronises accumulated data to the cloud when connectivity is available

A key architectural principle is that individual nodes do not each require:

- their own cellular modem
- their own cloud connection
- their own user interface
- their own complete data-management system

This is the reasoning behind:

> **Distribute the measurements, centralise the complexity.**

---

# 9. Local-first data storage

One of the strongest poster messages should be:

# Bad signal should not mean lost data

The intended data path is approximately:

**Power on → Measure → Transmit → Confirm → Store locally → Synchronise → View**

The hub stores measurements locally before or independently of cloud availability.

If mobile or internet connectivity fails:

- measurements remain stored locally
- cloud access is delayed
- data collection can continue
- synchronisation can occur later

Preferred wording:

> **Connectivity affects when data arrive — not whether they survive.**

or

> **Bad signal should delay data, not destroy it.**

This should be a prominent visual element in the central FieldMesh architecture diagram.

---

# 10. Device health and data trust

Environmental readings alone are not enough.

FieldMesh also tracks health/status information such as:

- battery state
- last report time
- connection status
- reporting status
- sensor health

The scientific reason for including these data is interpretability.

A gap in a dataset should not leave the user wondering whether:

- the environment changed
- the battery failed
- the radio link failed
- the sensor stopped responding
- the device stopped reporting

Key phrase:

> **A missing reading should not be a mystery.**

Another useful phrase:

> **Device health is part of the ecological data workflow.**

This links engineering reliability directly to scientific data quality.

---

# 11. Current sensor package

The current prototype uses **consumer-grade sensors**.

This is intentional because dense deployments can become financially unrealistic if every monitoring point requires high-cost reference-grade instrumentation.

## Current measurements

### Air
- temperature
- relative humidity

### Light
- visible-spectrum / spectral light conditions

### Soil
- soil moisture
- soil temperature
- measurements at two depths

### Wind
- wind speed
- wind direction is still in development

## Important modularity statement

**FieldMesh is not limited to this sensor package.**

The node architecture is intended to be modular.

Different projects may use:

- different sensors
- higher-grade sensors
- project-specific sensors
- alternative combinations of sensors

Potential examples that could be shown illustratively, but should not be implied to be currently integrated unless confirmed:

- PAR
- rainfall
- leaf wetness
- surface temperature
- additional soil sensors
- acoustic sensors

Preferred framing:

> **FieldMesh is a data-logging platform, not a fixed sensor package.**

---

# 12. Consumer-grade sensors and scientific validation

This is an important part of the poster.

The poster should not imply that consumer-grade sensors are equivalent to research/reference-grade instruments.

The rationale is:

- lower-cost sensors can make dense spatial replication realistic
- scientific usefulness depends on understanding their limitations
- affordability must be paired with transparent validation

## Required validation themes

- calibration
- comparison against reference instruments
- drift testing
- uncertainty assessment
- transparent reporting of limitations
- longer field deployments

Preferred key messages:

> **Low-cost sensors still need high standards.**

> **Low-cost monitoring is only useful when its uncertainty is visible.**

> **Affordability should not come at the cost of transparency.**

A good simple visual is:

**Consumer-grade sensor → calibration → reference comparison → field validation → usable data**

The intended trade-off can be shown as:

**More monitoring points ↔ known accuracy and limitations**

---

# 13. Field setup and usability

FieldMesh is intended to be deployable by ecologists and field teams rather than requiring an engineer at every installation.

## Local field interface

The hub creates its own local Wi-Fi network.

A user can connect through a normal browser using:

- phone
- tablet
- laptop

The setup interface can support tasks such as:

- node discovery
- pairing
- naming
- setting recording intervals
- checking sensor status
- checking connection status
- deployment

No external internet connection should be required for field setup.

No specialist command-line workflow should be required for normal deployment.

No dedicated mobile app is required.

Key message:

> **Deployment should not require an engineer.**

Another useful phrase:

> **The system should hide technical complexity without hiding the condition of the network.**

Friendly node names such as:

- Under-panel north
- Row gap 3
- Grassland reference

are useful to show in the interface/demo, but they do not need to be a major poster message.

---

# 14. Browser dashboard

Once the data are synchronised, users can access the dashboard through a standard web browser.

The dashboard can be accessed from:

- phone
- tablet
- laptop

Potential dashboard information includes:

- environmental trends
- differences between nodes
- battery status
- reporting status
- device health
- recent and historical measurements

The poster should emphasise **simple browser access**, rather than making the dashboard appear to be a complex analysis platform.

Preferred framing:

> **No specialist software required.**

> **The complexity should sit behind the interface — not in front of the user.**

More advanced analysis can still occur through exported data outside the dashboard.

---

# 15. Repeatability and comparability

One of the core goals is to make chosen monitoring designs easier to repeat.

FieldMesh does **not** prescribe one universal study design.

It should not be claimed to “guarantee standardisation”.

Instead, the platform could support greater consistency in:

- sensor configurations
- recording intervals
- deployment procedures
- health metadata
- data formats

## Within one solar farm

The same framework could compare:

- under-panel locations
- between-row locations
- site edges
- reference areas
- vegetation treatments
- grazing areas
- mowing treatments
- restoration zones

## Across multiple solar farms

The same monitoring framework could be repeated at multiple sites.

Preferred framing:

> **The aim is not one fixed study design — it is a platform that makes chosen designs easier to reproduce.**

> **Repeatability is valuable within a study, and comparability becomes valuable across studies.**

---

# 16. Hypothetical hybrid deployment

The preferred poster ending is a **hypothetical solar-farm installation** rather than an abstract “future vision” section.

This should combine both:

## Research use

Replicated nodes could compare:

- under-panel conditions
- between-row conditions
- edges
- reference areas

The same sensors and recording intervals could be used across repeated locations.

## Management use

Additional nodes could monitor:

- grazing
- mowing
- restoration
- vegetation treatments
- other management interventions

The same FieldMesh network, data structure and health workflow could support both research and management.

Preferred wording:

> **One monitoring framework. Two complementary uses.**

> **Research can explain patterns. Monitoring can help track management responses.**

The hypothetical deployment graphic should ideally be one of the two dominant visuals on the poster.

---

# 17. Current development status

FieldMesh is in active development.

The poster must be transparent that it is a **working prototype**, not a finished commercial or fully validated system.

## Phase 1 — Core system

### Working / complete in the current prototype

- node power-on / measure / transmit / power-off cycle
- core firmware
- node-to-hub communication
- node discovery
- pairing
- naming
- deployment workflow
- local hub interface via Wi-Fi
- local data logging
- cloud synchronisation
- browser dashboard
- prototype hardware testing/deployment work

## Phase 2 — Scientific validation

### In progress / required

- sensor calibration
- comparison with reference instruments
- drift testing
- uncertainty testing
- longer field deployments
- broader field validation

## Phase 3 — Field-ready refinement

### Next-stage work

- complete wind-direction channel
- further enclosure refinement
- final/next production hardware run
- repeatability testing across sites
- continued operational reliability testing

Preferred framing:

> **The architecture is working. The next stages focus on evidence, reliability and deployment.**

or

> **Building the system was the first step. Establishing what its data can reliably support is the next.**

Avoid claims such as:

- fully validated
- production ready
- cheapest
- best
- solves microclimate monitoring
- guarantees standardisation

---

# 18. Condensed poster narrative

For the A0 poster, the 13-slide keynote narrative should be reduced to approximately five or six visual blocks.

A preferred structure is:

## Top — Title and thesis

**Monitoring the mosaic**  
FieldMesh for fine-scale environmental monitoring in solar farms

One-sentence context:
FieldMesh is being developed in response to a monitoring-consistency gap identified in solar-farm microclimate research.

---

## Block 1 — The monitoring gap

Key points:

- solar-farm microclimate studies are difficult to compare
- sensor types differ
- placement differs
- sampling intervals differ
- spatial designs differ
- reporting methods differ
- dense monitoring is constrained by cost, power, connectivity and maintenance

Key line:

> **The challenge is not only what we measure — but how consistently and practically we can measure it.**

---

## Block 2 — The FieldMesh system

This should probably be the largest visual block.

Core statement:

> **Many simple sensor nodes. One coordinating field hub.**

Main visual:

**Multiple sensor nodes → field hub → local storage → cloud → browser dashboard**

Important callouts:

- nodes fully power off between measurements
- local-first data storage
- data can survive poor connectivity
- field setup through local Wi-Fi
- browser-based access
- device-health monitoring

Key line:

> **Bad signal should delay data — not destroy it.**

---

## Block 3 — Modular sensing and validation

Current consumer-grade sensor package:

- air temperature
- relative humidity
- spectral light
- soil moisture
- soil temperature at two depths
- wind speed
- wind direction in development

Key messages:

- current prototype uses consumer-grade sensors
- validation is essential
- platform is modular
- platform is not limited to the current sensor set

Key line:

> **Low-cost sensors still need high standards.**

---

## Block 4 — Hypothetical deployment

Large visual showing a solar farm with monitoring locations.

Research comparison points:

- under panel
- between rows
- edge
- reference

Management points:

- grazing
- mowing
- restoration
- vegetation treatments

Both feed into the same hub.

This block should visually demonstrate the “mosaic” idea.

---

## Block 5 — Repeatability

Show consistency:

### Within a solar farm
repeat monitoring across microhabitats and treatments

### Across solar farms
repeat the same framework at multiple sites

Potential visual icons:

- sensor configuration
- recording interval
- deployment method
- health metadata
- data format

Key line:

> **The aim is not to prescribe one fixed study design — but to make chosen designs easier to reproduce.**

---

## Block 6 — Current status and next steps

A compact roadmap:

**Working prototype → Scientific validation → Field-ready refinement**

Working:
- node operation
- communication
- local storage
- local configuration
- cloud synchronisation
- dashboard

Validation/refinement:
- calibration
- reference comparison
- drift/uncertainty testing
- longer deployments
- wind direction
- enclosure refinement
- production hardware refinement

---

## Bottom — Closing message

# If solar farms are mosaics, our monitoring systems need to see the mosaic.

Include:

- Tom Armstrong
- affiliation(s), once finalised
- contact details
- QR code to project page/repository if available

---

# 19. Current draft poster copy

## Title

# Monitoring the mosaic

## FieldMesh for fine-scale environmental monitoring in solar farms

**From a research gap to a modular field monitoring system**

Tom Armstrong

---

## THE MONITORING GAP

Solar-farm microclimate studies are often difficult to compare.

Studies vary in:

- sensor types
- sensor placement
- recording intervals
- spatial design
- reporting methods

Dense monitoring is also constrained by cost, power, connectivity and field maintenance.

**The challenge is not only what we measure — but how consistently and practically we can measure it.**

---

## FIELDMESH

### Many simple sensor nodes. One coordinating field hub.

FieldMesh is being developed as a modular platform for distributed environmental monitoring.

**Sensor nodes** collect measurements at multiple locations across a site.

**The field hub** coordinates the network, stores a permanent local copy of the data and synchronises it when connectivity is available.

**The browser dashboard** provides access to environmental measurements and device-health information without specialist software.

### Local-first data flow

**Power on → Measure → Transmit → Confirm → Store locally → Synchronise → View**

Nodes are **completely powered off between measurements**.

The hub stores measurements locally so internet failure does not automatically mean data loss.

**Bad signal should delay data — not destroy it.**

---

## ACCESSIBLE SENSORS — WITH TRANSPARENT VALIDATION

The current prototype uses consumer-grade sensors to make dense deployment more realistic.

Current measurements include:

**Air** — temperature and relative humidity  
**Light** — visible-spectrum conditions  
**Soil** — moisture and temperature at two depths  
**Wind** — wind speed; direction is in development

The node platform is modular and is **not limited to this sensor package**.

Low-cost sensors still require high scientific standards.

**Calibration, reference comparison, drift testing and transparent reporting of uncertainty are essential.**

---

## MAKING MONITORING EASIER TO REPEAT

FieldMesh is being developed to support consistent:

**sensor configurations · recording intervals · deployment procedures · health metadata · data formats**

### Within a solar farm

Compare environmental conditions:

**under panels · between rows · at edges · in reference areas · across management treatments**

### Across solar farms

Repeat the same monitoring framework at multiple sites.

**The aim is not to prescribe one fixed study design — but to make chosen designs easier to reproduce.**

---

## A HYPOTHETICAL DEPLOYMENT

One coordinated network could support both research and management.

### Research

Replicated nodes could compare:

**under-panel · between-row · edge · reference conditions**

### Management

Additional nodes could track:

**grazing · mowing · restoration · vegetation treatments**

The same network, data structure and device-health workflow could support both purposes.

**Research can explain patterns. Monitoring can help track management responses.**

---

## CURRENT STATUS

### Working prototype

- core node operation
- node-to-hub communication
- local data storage
- field configuration interface
- cloud synchronisation
- browser dashboard

### Validation and refinement

- sensor calibration and reference comparison
- drift and uncertainty testing
- longer field deployments
- wind-direction integration
- enclosure refinement
- production hardware refinement

**The architecture is working. The next step is establishing what its data can reliably support.**

---

# If solar farms are mosaics, our monitoring systems need to see the mosaic.

---

# 20. Poster visual strategy

The poster should be understood quickly during a conference reception.

A reader should be able to grasp the main story in approximately 2–3 minutes.

The presenter will be standing beside the poster, so the poster does **not** need to explain every technical detail independently.

## Recommended visual hierarchy

### Largest visual 1 — FieldMesh architecture

Show:

- multiple nodes in different solar-farm locations
- one field hub
- local storage
- intermittent cloud connection
- browser dashboard

This visual should carry:

- distributed sensing
- centralised coordination
- local-first storage
- delayed synchronisation
- browser access

### Largest visual 2 — Hypothetical solar-farm deployment

Show:

- arrays/panel rows
- under-panel nodes
- between-row nodes
- edge locations
- reference location
- management treatments
- hub position
- research vs management monitoring markers

### Secondary visuals

- OFF / ON / OFF node power cycle
- consumer-grade sensor → validation pathway
- current status roadmap
- icons for repeatability dimensions
- dashboard screenshot
- hardware photographs

---

# 21. Poster layout direction

Current preferred emphasis:

**approximately 40% scientific gap / rationale**  
**approximately 60% FieldMesh system / application**

The exact split can be reduced further toward FieldMesh if needed.

## Potential A0 portrait layout

### Header
- title
- subtitle
- presenter / affiliation
- strong solar-farm image

### Upper left
- monitoring gap

### Upper / centre
- large FieldMesh architecture diagram

### Upper right
- modular sensing + validation

### Middle / lower centre
- large hypothetical deployment diagram

### Lower left
- repeatability within and across sites

### Lower right
- current status and validation roadmap

### Footer
- closing mosaic statement
- contact
- QR code

---

# 22. Visual style

The poster should feel:

- scientific but not sterile
- field-based
- ecological
- modern
- credible
- practical
- visually clear from a distance

Preferred imagery:

- solar-farm landscapes
- real FieldMesh hardware photographs
- simple network diagrams
- field-deployment maps
- dashboard screenshots
- clean environmental icons

Avoid:

- dense circuit schematics
- large blocks of prose
- overly promotional product language
- excessive technical jargon
- too many small panels
- tiny text
- claims beyond current validation status

Potential palette:

- dark green
- muted solar/grass green
- white
- light grey
- restrained blue for data/cloud elements
- earth tones or orange for management-treatment markers

---

# 23. Key phrases worth retaining

- **Monitoring the mosaic**
- **The microclimate is where infrastructure meets ecology.**
- **A single weather station cannot describe a mosaic.**
- **The monitoring gap is not only scientific — it is practical.**
- **Many simple sensor nodes. One coordinating field hub.**
- **Distribute the measurements, centralise the complexity.**
- **FieldMesh was built to make dense monitoring realistic.**
- **Local-first data logging means bad signal does not equal lost data.**
- **Connectivity affects when data arrive — not whether they survive.**
- **A missing reading should not be a mystery.**
- **Device health is part of data trust.**
- **The lowest-power operating state is not sleep — it is off.**
- **FieldMesh is a data-logging platform, not a fixed sensor package.**
- **Low-cost sensors still need high standards.**
- **Low-cost monitoring is only useful when its uncertainty is visible.**
- **Deployment should not require an engineer.**
- **The complexity should sit behind the interface — not in front of the user.**
- **The aim is not one fixed study design — it is a platform that makes chosen designs easier to reproduce.**
- **One monitoring framework. Two complementary uses.**
- **The architecture is working. The next step is establishing what its data can reliably support.**
- **If solar farms are mosaics, our monitoring systems need to see the mosaic.**

---

# 24. Claims and wording to avoid

Do not say:

- “FieldMesh solves microclimate monitoring.”
- “FieldMesh is fully validated.”
- “FieldMesh is production ready.”
- “FieldMesh is the cheapest system.”
- “FieldMesh is the best system.”
- “FieldMesh guarantees standardisation.”
- “Consumer-grade sensors are equivalent to research-grade sensors.”
- “Nodes enter deep sleep.”
- “Nodes sleep between readings.”

Prefer:

- “FieldMesh is being developed to reduce practical monitoring barriers.”
- “The prototype architecture is working.”
- “Scientific validation is ongoing.”
- “The aim is to make dense monitoring more realistic.”
- “The platform can support more consistent monitoring designs.”
- “The current prototype uses consumer-grade sensors.”
- “The node platform is modular.”
- “Nodes power off completely between measurements.”

---

# 25. Presenter positioning

Presenter: **Tom Armstrong**

Desired positioning:

An ecologist / conservation technologist working across:

- field ecology
- environmental monitoring
- electronics
- embedded systems
- software
- data

Avoid framing the work as:

> “I invented the solution.”

Preferred framing:

> **This is one attempt to make the kind of monitoring we are asking for in the literature more feasible in the field.**

The poster should present FieldMesh as a response to an identified methodological and practical problem.

---

# 26. Original keynote development path

Before the presentation format changed to a poster, the following 13-slide structure was developed.

It is useful as background material and may help when discussing the poster in person.

1. **Monitoring the mosaic** — research-led opening
2. **The monitoring gap** — inconsistent studies
3. **What would better monitoring require?** — distributed, standardised, reliable, scalable
4. **FieldMesh** — many simple nodes, one coordinating hub
5. **Bad signal should not mean lost data** — local-first storage
6. **A missing reading should not be a mystery** — device health
7. **Powered off between measurements** — full hardware power-off
8. **Low-cost sensors still need high standards** — validation and modularity
9. **Deployment should not require an engineer** — local Wi-Fi setup
10. **No specialist software required** — browser dashboard
11. **Making monitoring easier to repeat** — within and across sites
12. **From working prototype to validated field system** — development roadmap
13. **What could this look like in practice?** — hybrid research and management deployment

The poster is a condensed version of this same narrative rather than a completely new concept.

---

# 27. Development roadmap wording

## Phase 1 — Core system

**Complete / working**

- node power cycle
- node-to-hub communication
- node discovery/configuration
- local storage
- local Wi-Fi setup interface
- cloud synchronisation
- browser dashboard

## Phase 2 — Scientific validation

**In progress**

- sensor calibration
- reference-instrument comparison
- drift testing
- uncertainty testing
- longer deployments

## Phase 3 — Field-ready refinement

**Next**

- wind direction
- sensor housing/enclosure refinement
- final/next production hardware
- repeatability testing
- broader deployments

---

# 28. Useful discussion points for the poster session

Because the presenter will stand beside the poster, detailed explanations can be kept verbal rather than printed.

Likely discussion areas:

- Why not just use one research-grade weather station?
- Why consumer-grade sensors?
- How will sensors be calibrated?
- How will sensor drift be handled?
- Can users fit other sensors?
- How long can nodes operate between servicing?
- How does the node fully power itself off and back on?
- How does the hub retain data when cellular coverage fails?
- How many nodes can one hub support?
- What wireless protocol is used?
- How are nodes paired and named?
- Can recording intervals differ?
- How will FieldMesh compare with commercial monitoring systems?
- Is the hardware/software open source?
- What field validation has already been completed?
- How could data be standardised across sites?
- Can the system support operational monitoring as well as research?
- Could it be used outside solar farms?

Not all of these questions currently have complete validated answers. Unknowns should remain unknown until evidence is available.

---

# 29. Assets likely needed for the final poster

## Essential

- high-resolution FieldMesh logo, if finalised
- photographs of current sensor-node hardware
- photograph of current field hub
- clean system architecture diagram
- current dashboard screenshot
- local setup-interface screenshot
- hypothetical solar-farm deployment illustration
- QR code destination
- presenter contact details
- presenter affiliation(s)
- final abstract / poster ID if needed for metadata

## Useful

- photograph of a solar farm with visible vegetation/microhabitat variation
- photo of sensors undergoing calibration/reference comparison
- hardware enclosure photo
- visual of the OFF → ON → OFF cycle
- one simple monitoring-gap illustration
- one simple validation pathway graphic

---

# 30. Open decisions / items still to finalise

- Exact final subtitle
- Presenter affiliation line
- Whether to display the paper citation prominently or in a small references area
- Final QR-code destination
- Exact dashboard screenshot
- Which real FieldMesh hardware images to use
- Whether the final poster should lean even more strongly toward the system than the monitoring gap
- Which management treatments to show in the hypothetical installation
- Whether “grazing, mowing, restoration, vegetation treatment” are the best examples for the CSB audience
- Final sensor-validation language once calibration work progresses
- Exact status of wind direction by poster submission
- Exact wording of production-hardware status by submission
- Whether to include one or two references beyond the review paper
- Confirmation of the actual poster submission deadline due to conflicting October 14 / August 20 wording in the conference brief

---

# 31. Source-of-truth technical corrections

These points override older wording in the original project source where there is a conflict.

1. **Nodes do not sleep. They fully power off between measurements.**
2. **The current sensors are consumer-grade.**
3. **Consumer-grade sensing must be paired with transparent calibration, validation, drift and uncertainty assessment.**
4. **FieldMesh nodes are not limited to the current sensor package.**
5. **FieldMesh should be described as a modular data-logging / monitoring platform.**
6. **Affordability is important because dense monitoring requires many points; it is not the sole purpose of the system.**
7. **Consistency should be framed at both scales: within sites and across sites.**
8. **The final application example should combine both research and management monitoring.**
9. **The system is a working prototype moving toward validated field use, not a production-ready commercial product.**

---

# 32. One-paragraph project summary

FieldMesh is a modular environmental monitoring platform being developed in response to a methodological gap identified in solar-farm microclimate research. Distributed sensor nodes collect environmental measurements at multiple locations and completely power off between scheduled readings, while a central field hub coordinates the network, stores a permanent local copy of the data and synchronises it when connectivity becomes available. The current prototype uses consumer-grade air, light, soil and wind sensors to make dense monitoring more feasible, but scientific validation, calibration, drift testing and transparent uncertainty reporting are central to the development pathway. The platform is not limited to the current sensor package and is intended to make chosen monitoring designs easier to repeat within and across solar farms. A future deployment could support both replicated ecological research and ongoing management monitoring within the same coordinated network.

---

# 33. One-sentence project description

> **FieldMesh brings distributed environmental data back from hard-to-reach field sites, stores it locally when connectivity fails, and makes it accessible through a simple browser-based system without requiring every monitoring point to carry the full complexity of a standalone station.**

---

# 34. Final poster takeaway

# If solar farms are mosaics, our monitoring systems need to see the mosaic.


