

### System Overview

The system is a small, self-contained **wireless sensor network** for environmental monitoring.

- A central **“mothership”** unit sits on site.
  - It creates its own Wi-Fi network so you can connect with a phone or laptop.
  - It keeps accurate time via a real-time clock (DS3231).
  - It stores all incoming data onto an SD card as a CSV file.
- Multiple **sensor nodes** are deployed around the site.
  - They measure environmental variables like air temperature.
  - They use a low-power wireless protocol (ESP-NOW) to send data back to the mothership.
  - They can sleep between measurements and wake up on a configurable interval.

Everything is designed to be:

- **Low-cost**  
- **Field-friendly** (minimal UI; simple web dashboard)  
- **Robust to power loss** (state is stored in non-volatile memory)

---

### Node Lifecycle: from “seen” to “deployed”

1. **Discovery**

   When a sensor node is powered up for the first time, it doesn’t know which mothership it belongs to.  
   It simply announces: “I’m here, my ID is `TEMP_001`, I’m a temperature node”.

   The mothership hears those announcements and shows the node in the web dashboard under **Unpaired Nodes**.

2. **Pairing**

   From the dashboard, you tick the box next to one or more unpaired nodes and click **Pair**.

   This does two things:

   - The mothership records that the node now belongs to this mothership.
   - A pairing command is sent back to the node so it also remembers which mothership to talk to.

   At this point the node is **bound**, but not yet deployed (it won’t send measurements).

3. **Deployment**

   Once you’re happy with which nodes to use, you select them in the **Paired Nodes** list and click **Deploy**.

   When you deploy:

   - The mothership sends the current time to each node (from its DS3231 clock).
   - Each node stores that time and starts its measurement/sending cycle.

   The deployed nodes now appear in the **Active Deployed Nodes** section and you will see live data being logged.

4. **Reverting / Unpairing**

   - **Revert to Paired**  
     If you want a node to stop sending data but keep its association with the mothership, you click **Revert to Paired**.  
     The node will stop transmitting and go back to “waiting for deployment”.

   - **Unpair**  
     If you want a node to completely forget the mothership and behave as “new” again, you use the **Unpair** section.  
     The node will forget its mothership, and the next time it appears it will be listed as Unpaired.

---

### Web Dashboard

The mothership exposes a simple web interface:

- Connect to the Wi-Fi network **`Logger001`**.
- Open a browser and go to **`http://192.168.4.1/`**.

From there you can:

- See the **current time** on the mothership.
- Set the **sampling / wake interval** for all nodes (e.g. every 5 minutes).
- **Discover** new nodes and pair them.
- **Deploy** and **revert** nodes.
- **Unpair** nodes entirely.
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
  - MAC address of the node
  - Sensor type (e.g. `temperature`)
  - Measured value

The mothership also logs a simple “I’m alive” status row every minute, which makes it easy to see if the system has been running continuously.

---

### What’s Working Now

- Nodes can be discovered, paired, deployed, reverted and unpaired from the web UI.
- The system behaves correctly across reboots and power cycles:
  - Nodes remember which mothership they belong to.
  - The mothership remembers which nodes are paired or deployed.
- Data is reliably logged to the SD card in a simple format that can be opened with Excel, R, Python, etc.

---
