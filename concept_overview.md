### System Overview

The system is a small, self-contained **wireless sensor network** for environmental monitoring.

- A central **“mothership”** unit sits on site.
  - It creates its own Wi-Fi network so you can connect with a phone or laptop.
  - It keeps accurate time via a real-time clock (DS3231).
  - It stores all incoming data onto an SD card as a CSV file.
  - It remembers paired / deployed nodes and their wake interval in non-volatile storage (NVS).

- Multiple **sensor nodes** are deployed around the site.
  - They measure environmental variables like air temperature.
  - They use a low-power wireless protocol (ESP-NOW) to send data back to the mothership.
  - They can sleep between measurements and wake up on a configurable interval driven by a DS3231 alarm.
  - They also store their own state (which mothership they belong to, whether they are deployed, wake interval, and RTC sync status) in NVS so they survive hard power cuts.

Everything is designed to be:

- **Low-cost**  
- **Field-friendly** (minimal UI; simple web dashboard)  
- **Robust to power loss**  
  - Mothership and nodes keep their pairing / deployment state in NVS.  
  - The DS3231 on each node is backed by a coin cell, so time survives main power loss.

---

### Node Lifecycle: from “seen” to “deployed”

1. **Discovery**

   When a sensor node is powered up for the first time, it doesn’t know which mothership it belongs to.  
   It simply announces: “I’m here, my ID is `TEMP_001`, I’m a temperature node.”

   The mothership hears those announcements and shows the node in the web dashboard under **Unpaired Nodes**.

   > On subsequent boots, if the node has already been paired, it loads the stored mothership MAC from NVS and reconnects automatically.

2. **Pairing**

   From the dashboard, you tick the box next to one or more unpaired nodes and click **Pair**.

   This does two things:

   - The mothership records that the node now belongs to this mothership (in its own NVS and in RAM).
   - A pairing command is sent back to the node so it also remembers which mothership to talk to (stored in NVS on the node).

   At this point the node is **bound**, but not yet deployed (it won’t send measurements).

3. **Deployment**

   Once you’re happy with which nodes to use, you select them in the **Paired Nodes** list and click **Deploy**.

   When you deploy:

   - The mothership sends the current time to each node (from its DS3231 clock).
   - The node sets its own DS3231 to that time.
   - Each node stores:
     - Mothership MAC
     - Deployed state
     - Wake interval
     - “RTC is synced” flag

   The deployed nodes now appear in the **Active Deployed Nodes** section and you will see live data being logged.

   > After a power cut, a deployed node reloads this state from NVS, sees that it is deployed and RTC is synced, and simply resumes sending data without needing to be redeployed.

4. **Reverting / Unpairing**

   - **Revert to Paired**  
     If you want a node to stop sending data but keep its association with the mothership, you click **Revert to Paired**.  
     The node will stop transmitting and go back to “waiting for deployment”, but it still remembers which mothership it belongs to.

   - **Unpair**  
     If you want a node to completely forget the mothership and behave as “new” again, you use the **Unpair** section.  
     The node will forget its mothership MAC, clear its deployed/synced flags, and the next time it appears it will be listed as Unpaired.

---

### Web Dashboard

The mothership exposes a simple web interface:

- Connect to the Wi-Fi network **`Logger001`**.
- Open a browser and go to **`http://192.168.4.1/`**.

From there you can:

- See the **current time** on the mothership.
- Set the **sampling / wake interval** for all nodes (e.g. every 5 minutes).
- **Discover** new nodes and pair them.
- **Deploy**, **revert**, and **unpair** nodes.
- See which nodes are currently deployed and actively sending data.
- Download the full **CSV log** of all data recorded to date.

Screenshots can be added to illustrate:

- The main dashboard view
- The lists of unpaired / paired / deployed nodes
- The RTC settings panel
- The CSV download button

---

### Data Format

All data is stored in a single CSV file on the SD card:

- Each row includes:
  - Timestamp (from the mothership RTC)
  - Node ID (e.g. `TEMP_001`)
  - Human-readable node name (e.g. `Hello-v2`, if configured)
  - MAC address of the node
  - Sensor type (e.g. `temperature`)
  - Measured value

The mothership also logs a simple “I’m alive” status row every minute, which makes it easy to see if the system has been running continuously.

---

### What’s Working Now

- Nodes can be discovered, paired, deployed, reverted and unpaired from the web UI.
- The system behaves correctly across reboots and power cycles:
  - The mothership remembers which nodes are paired or deployed via NVS.
  - Nodes remember which mothership they belong to, their wake interval and whether they were deployed.
  - If the RTC still has valid time (coin cell is present), deployed nodes resume sending data immediately after power is restored.
- Data is reliably logged to the SD card in a simple format that can be opened with Excel, R, Python, etc.
