# FieldMesh — System Overview

> **Status:** FieldMesh is in active development. Core firmware, on-site setup, cloud sync, and the web dashboard are built and working on prototype hardware; a few sensor channels (wind direction) and the final production hardware run are still being finished. Everything below describes what the system is built to deliver.

**FieldMesh** is an environmental monitoring network that brings detailed, trustworthy data back from places standard weather stations can't reach — at a fraction of the cost of proprietary equipment. It's built for people who need to understand conditions on the ground at a remote site, without mains power, without reliable internet, and without needing a technician to keep it running.

## How it works, in plain terms

FieldMesh has two parts working together:

1. **Sensor nodes** — small, battery-powered units scattered around a site. Each one wakes briefly on its own schedule, takes a reading, and goes straight back to sleep. A node spends almost its entire life powered fully off, which is what stretches battery life in the field.

2. **A field hub** — one on-site device that gathers readings from every node over a short-range wireless link, keeps a permanent local copy, and forwards the accumulated data to the cloud whenever it has a signal. It's also the one place you interact with in the field: turn it on, connect to its Wi-Fi, and a setup page opens automatically for adding nodes, naming them, and choosing how often they should record.

Once data reaches the cloud, it shows up on a web dashboard — from a phone, tablet, or laptop, anywhere with a browser. No app to install, no proprietary software.

## What it measures

Each node builds a rounded picture of the conditions right where it's placed:

- **Air** — temperature and humidity just above the ground or canopy
- **Light** — the full colour spectrum of sunlight reaching the site, from violet through to deep red, not just brightness
- **Soil** — moisture and temperature at two separate probe depths
- **Wind** — speed, with direction in development
- **Device health** — battery level and connection status, so you know a node is trustworthy before you trust its data

Because every node reports its own battery and sensor health alongside its readings, a gap in the data is never a silent mystery — the dashboard tells you which node needs attention and why.

## The experience it's designed to deliver

**Set it up in minutes, not with a manual.** Pairing and deploying a node happens from a simple on-screen setup page hosted by the hub itself — no specialist configuration tools, no firmware flashing in the field. The screen updates live as you work, so you see a node join the moment it's found instead of wondering if it worked.

**Walk away, not check in constantly.** Because nodes are powered fully off between readings rather than idling, a single battery charge goes much further — the system is built for "install and forget," not frequent battery swaps.

**Never lose a reading.** Every measurement is written to permanent local storage the moment it's taken and confirmed received by the hub before it's ever discarded from the node. If the internet connection drops or the cellular signal is poor, nothing is lost — data queues locally and uploads automatically once a connection is available.

**Know what's happening without visiting the site.** The dashboard shows live conditions, historical trends, battery levels, and the health of every node in one place. New readings appear automatically — no manual refreshing.

**Reconfigure remotely.** Need to change how often a node records, or which sensors it should expect? Those changes can be pushed out and picked up automatically the next time a node checks in — no need to physically retrieve and reprogram it.

**Recover gracefully.** If a node ever loses contact with its hub, it can be reset back into pairing mode in the field and rejoined in seconds, rather than needing to be replaced or sent back for reprogramming.

**Built to scale.** One hub can coordinate many nodes across a site, with each node's transmission scheduled rather than competing for airtime — so the network stays reliable as more sensors are added, not more fragile.

**Affordable enough to actually deploy.** Because it's built on low-cost, open hardware, FieldMesh makes fine-grained, site-specific monitoring accessible to projects that could never justify the cost of commercial weather stations — letting users capture the local variation (shade, airflow, soil, ground cover) that a single regional station always misses.

## In one sentence

FieldMesh brings reliable, detailed environmental data back from places that are hard to reach, keeps it safe even when the internet doesn't, and puts it in front of you on a screen — without asking you to be a technician to get there.
