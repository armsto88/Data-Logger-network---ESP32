# FieldMesh — Weekly Update (week of 30 June – 7 July 2026)

Hi all,

Here's a quick round-up of what we got done this week. The short version: FieldMesh is now a lot better at telling you *why* something isn't working, and we've laid the groundwork for sharing sites with collaborators.

---

## What we achieved this week

**Connectivity and system health**

- Every time the mothership uploads, it now logs the cellular signal strength, which SIM/carrier it's on, and whether it's on LTE or 2G.
- It also logs system stuff like why it last rebooted and how much memory is free — so instead of "the data just stopped," you can see whether it was a modem issue or a sensor issue.

**Node pause + battery health**

- You can now mark a node as **paused** — handy when you've pulled a sensor out for maintenance but don't want it to look like it's died.
- Battery voltage is now reported **under load** (while the modem is transmitting), which gives a much more honest picture of battery health than a resting reading.

**Sensor fault detection**

- The system now tracks three things per node: what you *configured* it to measure, what it *actually* reported, and what's been *missing for two uploads in a row*.
- So instead of a mysterious gap in your temperature data, the dashboard can tell you "air temperature is configured but not reporting" — much easier to troubleshoot in the field.

**Project sharing**

- You can now give colleagues or clients **read-only** access to a project — they can see all the nodes, readings, and status for a site, but can't change anything or see device credentials.
- This is locked down at the database level, so there's no risk of a shared viewer seeing someone else's data.

**Spectral sensor (AS7341) improvements**

- The database now stores five extra channels from the light sensor: broadband clear, near-infrared, gain, integration time, and a saturation flag.
- In practice this means spectral readings can be normalised across different settings, and we can flag and throw out saturated readings — important groundwork for any vegetation-index or canopy work down the track.

**Better sync + node control**

- Reworked the radio sync so the mothership gives each node a turn to send data rather than everyone talking at once — fewer collisions, fairer uploads, especially with bigger fleets.
- You can now change a node's recording interval, pause it, or unpair it from the dashboard, and the node confirms back that it got the message.

**Moving to Supabase**

- We've started shifting the cloud backend from Google Sheets to Supabase (a proper database with user accounts, security, and real-time updates). Both backends work for now, so we can migrate across safely without losing any data.

---

## What this means in the field

You can now see **where** each node is, **how long** it's been out there, **whether it's paused or actually faulty**, **how the battery's really holding up**, and **how good the cell signal is** — all updated each time the mothership checks in. Spectral data is now captured with enough metadata to be useful for vegetation work. And you can share a site with a colleague or client without handing over the keys.

Cheers,
Thomas