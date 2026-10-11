# Node Firmware — Next-Version Notes

**Started:** 2026-10-10

A running list of observations to fold into the next node firmware version. Entries
are observations and open questions, not a plan. Confirmed facts, unverified
inferences and proposals are labelled separately in each entry.

## 1. A powered-off unpaired node stays in the hub's pair/deploy list

**Observed (Tom, 2026-10-10).** New hub and two freshly erased nodes, all on build
`9c96213` (nodes: `esp32wroom` / `node-v3`). Both nodes appear in the hub app's list of
nodes available to pair or deploy. Turning one node off does not remove it; it stays
listed.

**Confirmed from hub source** (read, not reproduced on hardware):

- A node heard for the first time is added to the hub registry as `UNPAIRED`
  (`mothership/firmware/v2/src/config/node_registry.cpp`, log line `[REG] New node`).
- `getUnpairedNodes()` returns every `UNPAIRED` entry with no last-seen filter
  (`node_registry.cpp:408`).
- The only place a node is erased from the registry is after a confirmed cloud-driven
  unpair (`CONFIG_ACK ... removing node`, `mothership/firmware/v2/src/main.cpp`, around
  line 1811). Nothing ages out an unpaired node that has gone silent.
- The hub already records last-contact time per node (`lastSeen`, `lastSeenUnix`, and
  `lastSeenSec` in the node JSON), so the data to detect a silent node exists.

**Inferred, not tested:** only `PAIRED`/`DEPLOYED` nodes are written to NVS, so stale
`UNPAIRED` entries are probably RAM-only and clear when the hub restarts.

**Node side.** A node that is switched off cannot announce that it is leaving. While
unpaired and powered it broadcasts `NODE_STATUS (unpaired-recovery)`, so the absence of
those broadcasts is the only signal the hub gets.

**Options (proposed, nothing decided):**

1. Hub: age out or grey out `UNPAIRED` entries not heard within N seconds. This covers
   both a planned shutdown and a sudden power loss.
2. Node: send a "leaving" message on a clean shutdown. This only helps planned
   shutdowns, so on its own it does not fix the observed case.
3. Hub UI: show last-seen on unpaired cards and disable Pair/Deploy for stale ones.

**Open questions:**

- What N is right, relative to the node's unpaired broadcast interval (not measured here)?
- Should a stale entry be hidden, or kept and marked "not seen"?

**Ownership.** The change is mostly in the hub firmware (`mothership/`). It is recorded
here because it surfaced during node testing and option 2 would need a node-side
message.
