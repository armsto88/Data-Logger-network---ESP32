# Monitoring the Mosaic

## FieldMesh for Fine-Scale Environmental Monitoring in Solar Farms

**Presenter:** Tom Armstrong  
**Event:** Conference on Solar Energy and Biodiversity 2026  
**Format:** 15-minute keynote  
**Status:** Working presentation copy

---

## Presentation framing

This keynote follows a research-led narrative:

**Review → monitoring gap → design requirements → FieldMesh → practical deployment**

FieldMesh should be presented as a working prototype developed in response to a methodological gap, not as a finished commercial product. The talk should remain practical, scientifically transparent and accessible to a mixed audience of researchers, ecologists, consultants, land managers and solar-sector professionals.

### Core message

> If solar farms are mosaics, our monitoring systems need to see the mosaic.

### Research basis

The talk is grounded in the review paper:

**A mosaic of microclimates: biodiversity outcomes and wildlife habitat potential in large-scale solar facilities**  
*Biological Reviews*  
https://onlinelibrary.wiley.com/doi/10.1002/brv.70171

---

# Slide 1 — Opening title

## On-slide copy

# Monitoring the mosaic

## From a research gap to FieldMesh

**Fine-scale environmental monitoring for solar biodiversity landscapes**

Tom Armstrong  
Conference on Solar Energy and Biodiversity 2026

## Visual direction

Use a wide solar-farm image with visible contrasts between panel shade, open row spaces, vegetation and surrounding habitat. Add a subtle mosaic or environmental-gradient overlay.

A small progression near the bottom can establish the structure of the talk:

**Review → Monitoring gap → Field system**

Avoid showing detailed hardware on the opening slide. The first image should establish the landscape and ecological question.

## Speaker notes

> This project began with a review of microclimates in large-scale solar facilities.
>
> The review showed that solar farms can create complex patterns of temperature, moisture, light and airflow. But it also exposed a major limitation: studies often measure these conditions in different ways, making comparison and generalisation difficult.
>
> FieldMesh grew out of a simple question: how can we make the kind of dense, consistent monitoring called for in the literature more realistic in the field?

---

# Slide 2 — The monitoring gap

## On-slide copy

# We found a monitoring gap

Solar-farm microclimate studies are difficult to compare.

**Different sensors**  
**Different placements**  
**Different sampling intervals**  
**Different reporting methods**

## We need monitoring that is more consistent, distributed and repeatable.

## Visual direction

Show four mismatched monitoring examples, such as:

- one conventional weather station;
- one sensor beneath a panel;
- one sensor mounted at a different height;
- several differently spaced monitoring points.

These can feed into a fragmented or incomplete dataset graphic.

An alternative layout could contrast:

**What we need**  
Dense · Consistent · Repeatable

**What often happens**  
Sparse · Variable · Difficult to compare

## Speaker notes

> The review showed that microclimate conditions within solar facilities can vary considerably, but the most important starting point for this talk was a methodological gap.
>
> Studies used different sensors, different placements, different measurement intervals and different reporting approaches.
>
> This makes it difficult to compare findings across sites or build a reliable evidence base.
>
> The question was therefore not simply what should we measure, but how can we make consistent, distributed monitoring practical enough to repeat?

## Transition

> That monitoring gap became a practical design brief.

---

# Slide 3 — The design requirements

## On-slide copy

# What would better monitoring require?

## Many measurement points — deployed consistently

**Distributed**  
Capture variation across the site

**Standardised**  
Use consistent sensors, placements and intervals

**Reliable**  
Keep data safe through power and connectivity failures

**Scalable**  
Remain affordable and manageable as the network grows

## Visual direction

Place one isolated weather station on the left and a distributed monitoring network on the right.

Under the network, show the four requirements:

**Distributed · Standardised · Reliable · Scalable**

The visual should communicate:

> More spatial detail should not require proportionally more complexity.

## Speaker notes

> The monitoring gap became a design brief.
>
> Fine-scale environmental variation cannot be captured from one location. We need multiple measurement points distributed across the site.
>
> But once we increase the number of points, cost and complexity increase with them.
>
> A useful system therefore needs to be consistent, reliable and simple to scale—not just technically capable of recording environmental data.
>
> These became the core requirements behind FieldMesh.

## Transition

> FieldMesh was developed around the idea of distributing the measurements while centralising the complexity.

---

# Slide 4 — The FieldMesh concept

## On-slide copy

# FieldMesh

## Many simple sensor nodes. One coordinating field hub.

The nodes measure conditions across the site.

The hub collects, stores and synchronises the data.

**Dense monitoring without duplicating the complexity at every location.**

## Visual direction

Use a simple system diagram:

**Sensor node · Sensor node · Sensor node · Sensor node**  
↓  
**Field hub**  
↓  
**Cloud and dashboard**

Place the nodes at recognisable solar-farm positions:

- beneath a panel;
- between rows;
- at the site edge;
- in a reference area.

The concept should be clear before any detailed technical explanation is introduced.

## Speaker notes

> FieldMesh is built around a simple idea: distribute the measurements, but centralise the complexity.
>
> Small sensor nodes are placed throughout the site. Each node measures its local environmental conditions and sends those readings to a central field hub.
>
> The hub coordinates the network, stores a permanent local copy of the data and synchronises it when connectivity is available.
>
> This allows the network to grow without requiring every measurement point to have its own internet connection, interface or complete data-management system.

## Transition

> One of the most important design choices was separating data collection from internet availability.

---

# Slide 5 — Data survive poor connectivity

## On-slide copy

# Bad signal should not mean lost data

**Measure locally**

**Send to the field hub**

**Store a permanent local copy**

**Synchronise when connectivity returns**

## Connectivity affects when data arrive — not whether they survive.

## Visual direction

Use a left-to-right flow:

**Sensor nodes → Field hub → Local storage → Cloud → Dashboard**

Place a broken-signal icon between the hub and cloud. Keep the measurements visibly stored at the hub while the cloud connection is unavailable, then show the data continuing once the connection returns.

The hub and local-storage stage should be the visual centre of the slide.

## Speaker notes

> Remote field sites often have intermittent or unreliable connectivity.
>
> FieldMesh therefore stores readings locally at the hub before attempting to synchronise them with the cloud.
>
> When the mobile connection is unavailable, the measurements remain on site. Once connectivity returns, the accumulated data can be uploaded.
>
> This separates data collection from internet availability.
>
> A poor signal may delay access to the data, but it should not cause the measurements themselves to disappear.

## Key line

> Connectivity affects when data arrive—not whether they survive.

## Transition

> Protecting the readings is only one part of trustworthy monitoring. We also need to know whether the devices themselves are working properly.

---

# Slide 6 — Device health and data trust

## On-slide copy

# A missing reading should not be a mystery

FieldMesh records more than environmental conditions.

It also tracks:

**Battery state**  
**Last report time**  
**Connection status**  
**Sensor health**

## Device health is part of the ecological data workflow.

## Visual direction

Show a simplified dashboard card containing:

- an environmental reading;
- battery voltage or state;
- last report time;
- connection status;
- a sensor warning or health indicator.

Alongside it, show a gap in a time series with the question:

**Was this an ecological pattern—or did the sensor fail?**

## Speaker notes

> A dataset is only useful if we can trust how it was produced.
>
> When a reading disappears, we need to know whether the environment changed, the battery ran down, the connection failed or a sensor stopped responding.
>
> FieldMesh therefore records device-health information alongside environmental measurements.
>
> Battery state, connection status, reporting history and sensor health help distinguish real ecological patterns from technical failure.
>
> This is particularly important in distributed networks, where a problem at one node may otherwise remain unnoticed for weeks.

## Key line

> A missing reading should not be a mystery.

## Transition

> Long-term reliability also depends on how the nodes use power between measurements.

---

# Slide 7 — Powered off between measurements

## On-slide copy

# Powered off between measurements

Each node follows a controlled cycle:

**Power on**  
**Measure**  
**Transmit**  
**Confirm delivery**  
**Power off completely**

## Between measurements, the node consumes no operating power.

## Visual direction

Use a horizontal timeline:

**OFF** ━━━━━ **ON: measure + transmit** ━ **OFF** ━━━━━ **ON** ━ **OFF**

The long powered-off periods should dominate the visual.

Do not use sleep or moon icons. The nodes do not use processor deep sleep; they switch off completely.

A smaller annotation can read:

**The recording schedule controls when power is restored.**

## Speaker notes

> Long-term distributed monitoring depends heavily on power consumption.
>
> FieldMesh nodes do not remain powered and enter a software sleep mode. Once a measurement has been recorded and successfully delivered, the node powers off completely.
>
> An independent timing system restores power at the next scheduled recording interval. The node then starts, reads its sensors, transmits the data, confirms delivery and shuts down again.
>
> This hardware-controlled power cycle removes the background consumption associated with leaving the processor and supporting electronics active between readings.
>
> It also creates a predictable operating cycle that can be repeated consistently across every node in the network.

## Key line

> The lowest-power operating state is not sleep—it is off.

## Transition

> The current prototype combines this power architecture with an accessible, modular sensor package.

---

# Slide 8 — Accessible sensors need transparent validation

## On-slide copy

# Low-cost sensors still need high standards

The current prototype uses consumer-grade sensors.

They make dense deployment more realistic—but they must be:

**Calibrated**  
**Validated**  
**Checked for drift**  
**Reported with clear limitations**

## Affordability should not come at the cost of transparency.

### Small supporting note

**The node platform is modular and is not limited to the current sensor package.**

## Current prototype measurements

- air temperature and relative humidity;
- visible-spectrum light conditions;
- soil moisture and soil temperature at two depths;
- wind speed;
- wind direction in development.

## Visual direction

Use a balance graphic:

**More monitoring points**  
↔  
**Known accuracy and limitations**

Below it, show:

**Consumer-grade sensor → calibration → field validation → usable data**

The modularity note should remain visible but secondary to the validation message.

## Speaker notes

> The current FieldMesh prototype uses consumer-grade sensors because dense monitoring quickly becomes unaffordable if every location requires research-grade instrumentation.
>
> But low cost does not remove the need for scientific rigour.
>
> These sensors need to be calibrated, validated against reference instruments, checked for drift and reported with clear uncertainty and limitations.
>
> That work is still ongoing and is an important next stage of development.
>
> The aim is not to claim that inexpensive sensors are equivalent to reference-grade instruments. The aim is to understand where they are sufficiently accurate, where correction is needed and what questions they can reliably support.
>
> The platform itself is modular, so higher-grade or project-specific sensors can also be integrated when the research question requires them.

## Key lines

> Low-cost monitoring is only useful when its uncertainty is visible.

> FieldMesh is a data-logging platform, not a fixed sensor package.

## Transition

> The same principle applies to field usability: a technically capable network is of little value if deploying it requires an engineer at every site.

---

# Slide 9 — Field setup and deployment

## On-slide copy

# Deployment should not require an engineer

The field hub creates its own local Wi-Fi network.

From a phone or laptop, users can:

**Discover nodes**  
**Set recording intervals**  
**Check sensor and connection status**  
**Deploy the network**

## No app, command line or internet connection is required on site.

## Visual direction

Show a simple three-step field workflow:

**1. Power on the hub**  
**2. Connect through a browser**  
**3. Configure and deploy nodes**

Use a real screenshot of the local setup interface where possible. Pair it with a photograph of a node being configured or installed beside a solar array.

Friendly field names can be demonstrated in the interface rather than added to the main slide copy, for example:

- Under-panel north;
- Row gap 3;
- Grassland reference.

## Speaker notes

> One of the practical barriers to distributed monitoring is setup complexity.
>
> FieldMesh is intended to be configured by ecologists and field teams rather than requiring firmware changes or specialist software.
>
> The field hub creates its own local Wi-Fi network and hosts the setup interface directly. A user connects through a normal web browser on a phone, tablet or laptop.
>
> Nodes can then be discovered, configured and checked before they are placed around the site.
>
> Because the interface is hosted locally, the network can be deployed without mobile coverage or an existing internet connection.
>
> The aim is to make a multi-node deployment feel like one coordinated system rather than a collection of separate devices.

## Key line

> The system should hide technical complexity without hiding the condition of the network.

## Transition

> Once the network is deployed and synchronised, the same emphasis on accessibility continues in the dashboard.

---

# Slide 10 — Browser-based dashboard

## On-slide copy

# No specialist software required

Once data are synchronised, the dashboard can be opened from:

**Phone**  
**Tablet**  
**Laptop**

Users can view:

**Environmental trends**  
**Differences between nodes**  
**Battery and reporting status**

## The same interface supports field checks and longer-term analysis.

## Visual direction

Use one dashboard screenshot shown across three device frames:

- phone;
- tablet;
- laptop.

Keep the laptop view largest. A small browser bar or browser icon can reinforce that the dashboard is accessed through the web rather than through an installed application.

## Speaker notes

> Once the hub synchronises the data, users can access the dashboard through a standard web browser.
>
> There is no dedicated application to install and no specialist analysis software is required to check the network.
>
> A field worker can open it on a phone, while a researcher can use the same system on a laptop to compare nodes and examine longer-term patterns.
>
> The dashboard brings together environmental measurements, battery state and reporting status, so users can quickly see both what the site is doing and whether the network is operating normally.
>
> More detailed analysis can still be carried out through exported data, but routine access should remain simple.

## Key line

> The complexity should sit behind the interface—not in front of the user.

## Transition

> The broader value of a shared platform is that it can make chosen monitoring designs easier to repeat.

---

# Slide 11 — Comparable within and across sites

## On-slide copy

# Making monitoring easier to repeat

FieldMesh is being developed to support consistent:

**Sensor configurations**  
**Recording intervals**  
**Deployment procedures**  
**Health metadata**  
**Data formats**

## Within a site

Compare panels, row gaps, edges, references and management treatments.

## Across sites

Repeat the same monitoring framework across different solar farms.

**The aim is not one fixed study design—it is a platform that makes chosen designs easier to reproduce.**

## Visual direction

Split the slide into two connected scales.

### Left: one solar farm

Show repeated nodes positioned:

- beneath panels;
- between rows;
- at the edge;
- in a reference area;
- within management treatments.

### Right: multiple solar farms

Show several simplified sites using the same node symbols and comparable monitoring positions.

Connect both scales to one shared data structure or standardised dataset graphic.

## Speaker notes

> Consistency matters at two scales.
>
> Within one solar farm, the same monitoring approach can be repeated across panel positions, vegetation treatments, edges and reference areas.
>
> Across multiple solar farms, the same sensor configuration, recording interval, deployment procedure and data format can be reused.
>
> FieldMesh does not prescribe where every sensor must be placed or which questions researchers must ask.
>
> Its role is to reduce avoidable differences in how a chosen monitoring design is implemented.
>
> That can improve comparisons between treatments within a site, while also making evidence from different sites easier to combine and interpret.

## Key line

> Repeatability is valuable within a study—and comparability becomes valuable across studies.

## Transition

> This is the potential of the platform, but it is important to be transparent about where the development currently stands.

---

# Slide 12 — Development roadmap

## On-slide copy

# From working prototype to validated field system

## Phase 1 — Core system

**Complete**

- Node power-on, measure, transmit and power-off cycle
- Node discovery and configuration
- Hub-based local storage
- Cloud synchronisation
- Browser dashboard

## Phase 2 — Scientific validation

**In progress**

- Sensor calibration
- Comparison with reference instruments
- Drift and uncertainty testing
- Longer field deployments

## Phase 3 — Field-ready refinement

**Next**

- Complete wind direction
- Refine sensor housings
- Finalise production hardware
- Test repeatability across sites

## The architecture is working. The next stages focus on evidence, reliability and deployment.

## Visual direction

Use a three-stage horizontal path:

**Working prototype → Scientific validation → Field-ready system**

Include one real image for each stage where possible:

- current node or hub hardware;
- sensors beside reference instruments;
- a proposed field deployment.

Avoid presenting the final phase as a commercial launch. The goal is validated, repeatable field use.

## Speaker notes

> Development is progressing through three broad phases.
>
> The first phase was building the complete data pathway—from powering a node and recording a measurement through to local storage, cloud synchronisation and browser access. That core architecture is now working.
>
> The second phase is scientific validation. The consumer-grade sensors need to be compared with reference instruments, calibrated where necessary and tested for drift, uncertainty and long-term field behaviour.
>
> The third phase is field-ready refinement: completing wind direction, improving housings, finalising the next hardware version and testing whether the same deployment process can be repeated across sites.
>
> The important distinction is that the system can already collect and move data. The next challenge is establishing the quality, reliability and appropriate uses of those data.

## Key line

> Building the system was the first step. Establishing what its data can reliably support is the next.

## Transition

> To finish, imagine how the same platform could support both research and management within one solar farm.

---

# Slide 13 — Hypothetical research and management deployment

## On-slide copy

# What could this look like in practice?

## One coordinated network across the solar farm

### Research comparisons

**Under panels · Between rows · Edges · Reference areas**

Replicated nodes capture spatial variation using the same sensors and recording intervals.

### Management monitoring

**Grazing · Mowing · Restoration · Vegetation treatments**

Nodes track how environmental conditions respond to interventions over time.

## One monitoring framework. Two complementary uses.

**Research can explain the patterns. Management can respond to them.**

## Visual direction

Use an aerial or simplified plan of a solar farm divided into clear monitoring zones:

- repeated under-panel and between-row locations;
- external or site-edge reference points;
- a grazed area;
- a mown area;
- a restoration or wildflower treatment;
- one central field hub.

Use matching node symbols for replicated research locations and a second subtle marker style for management-monitoring points. Both should feed into the same hub and dashboard.

A small key can distinguish:

**Replicated comparison points**  
**Management monitoring points**

The listed management treatments are illustrative and can be adapted to the site or audience.

## Speaker notes

> A practical installation could combine two different monitoring purposes.
>
> The first is research. Repeated nodes beneath panels, between rows, along edges and in reference areas could help quantify spatial variation using consistent sensors and recording intervals.
>
> The second is management. Additional nodes could be placed within grazing, mowing, restoration or vegetation treatments to track how local conditions change after an intervention.
>
> These do not need to be separate systems. The same network, data structure and device-health workflow could support both.
>
> Research can help explain why patterns occur, while ongoing monitoring can help managers see whether interventions are producing the intended environmental conditions.
>
> FieldMesh is still progressing through validation and field refinement, but this is the type of coordinated installation it is being developed to support.
>
> If solar farms are mosaics, our monitoring systems need to see the mosaic.

## Final closing line

# If solar farms are mosaics,
# our monitoring systems need to see the mosaic.

## Final screen treatment

Keep the hypothetical deployment illustration visible while revealing the closing line.

A small footer can include:

- Tom Armstrong;
- contact details;
- project website or repository;
- QR code.

---

# Suggested timing

| Section | Slides | Approximate time |
|---|---:|---:|
| Research gap and requirements | 1–3 | 3 minutes |
| FieldMesh concept and architecture | 4–7 | 5 minutes |
| Sensors, usability and dashboard | 8–10 | 3.5 minutes |
| Repeatability and development status | 11–12 | 2.5 minutes |
| Hypothetical installation and close | 13 | 1 minute |

**Total:** approximately 15 minutes

---

# Presentation language to preserve

- “The monitoring gap became a design brief.”
- “Distribute the measurements, but centralise the complexity.”
- “Connectivity affects when data arrive—not whether they survive.”
- “A missing reading should not be a mystery.”
- “The lowest-power operating state is not sleep—it is off.”
- “Low-cost monitoring is only useful when its uncertainty is visible.”
- “FieldMesh is a data-logging platform, not a fixed sensor package.”
- “The complexity should sit behind the interface—not in front of the user.”
- “The aim is not one fixed study design—it is a platform that makes chosen designs easier to reproduce.”
- “If solar farms are mosaics, our monitoring systems need to see the mosaic.”

---

# Claims and terminology to avoid

Do not describe the nodes as entering deep sleep. They power off completely between measurements.

Avoid claiming that:

- FieldMesh is production ready;
- the sensor package is fully validated;
- consumer-grade sensors are equivalent to reference-grade instruments;
- FieldMesh guarantees standardisation;
- the platform is limited to the current sensor set;
- the system solves microclimate monitoring.

Prefer language such as:

- working prototype;
- active scientific validation;
- modular sensor platform;
- supports more consistent monitoring;
- makes dense deployment more realistic;
- developed to reduce practical barriers.
