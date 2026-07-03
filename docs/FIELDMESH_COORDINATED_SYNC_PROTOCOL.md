# FieldMesh Coordinated Sync Protocol

## Objective

Replace simultaneous node dumps with a bounded mothership-controlled session.
Only one deployed node receives permission to transmit snapshots at a time.
Records remain in the node queue until the mothership has persisted them and
returned a matching durable acknowledgement.

## Session sequence

1. The mothership opens a 12-second rendezvous and repeatedly broadcasts both
   legacy `SYNC_WINDOW_OPEN` and `SYNC_SESSION` beacons.
2. Each coordinated node derives a stable 0-1.8 second jitter from its node ID
   and session ID, then sends `NODE_HELLO` with its queue depth.
3. The mothership freezes the responder roster. Missing deployed nodes do not
   block nodes that responded.
4. The mothership issues one targeted `DUMP_GRANT` at a time. A grant permits a
   maximum of four records within nine seconds.
5. For every snapshot, the mothership writes the record to flash before sending
   `SNAPSHOT_ACK`. The node retries up to three times and pops the queue head
   only after a matching persisted acknowledgement.
6. The node sends idempotent `DUMP_DONE` with its remaining queue depth. Nodes
   with backlog return to the round-robin roster for another grant.
7. As soon as a node is empty, the mothership sends `SYNC_RELEASE` containing
   final mothership time and the active rendezvous schedule. The node persists
   those values, sends `RELEASE_ACK`, rearms its RTC, and powers down.
8. Nodes that fail or exceed the global deadline are released with their
   backlog intact. No failed node can hold the rest of the fleet open.

## Wire messages

| Direction | Message | Purpose |
|---|---|---|
| Mothership -> fleet | `SYNC_SESSION` | Session ID and bounded join/session windows |
| Node -> mothership | `NODE_HELLO` | Identity, config version, RTC time, and queue depth |
| Mothership -> node | `DUMP_GRANT` | Exclusive transmit permission, quota, and deadline |
| Node -> mothership | `NODE_SNAPSHOT2` | One persisted node record |
| Mothership -> node | `SNAPSHOT_ACK` | Confirms that exact sequence was durably stored |
| Node -> mothership | `DUMP_DONE` | Grant outcome and remaining queue depth |
| Mothership -> node | `SYNC_RELEASE` | Final clock and active rendezvous schedule |
| Node -> mothership | `RELEASE_ACK` | Confirms schedule persistence before shutdown |

Every grant, completion, and release carries a session ID. Grants additionally
carry a grant ID, preventing delayed packets from an earlier round or wake from
authorizing a transmission.

## Schedule migration safety

When the rendezvous interval changes, the mothership persists two schedules:

- the new active interval and phase;
- the previous interval and phase with three remaining grace cycles.

The RTC is armed for whichever schedule has the earliest next appointment.
Every old-schedule wake rebroadcasts the active schedule and decrements the
persisted grace counter. This survives mothership resets. After three serviced
old appointments, the old schedule is retired.

## Failure behaviour

- A missing `SNAPSHOT_ACK` causes retransmission of the same sequence; it never
  causes the node to delete the record.
- A missing `DUMP_DONE` expires only that grant. The scheduler moves on.
- A missing `SYNC_RELEASE` leaves the node on its already-persisted schedule;
  its local session timeout still rearms and powers down safely.
- A missing node is absent from the roster and cannot delay responders.
- The mothership snapshot queue is 32 records rather than eight. Any remaining
  queue drops are reported at the end of the coordinated window.

## Rolling deployment

The protocol retains the legacy marker path in both directions:

- New nodes connected to an older V2 mothership wait briefly for
  `SYNC_SESSION`, then use a jittered legacy dump.
- Older V2 nodes connected to the new mothership still see
  `SYNC_WINDOW_OPEN`; the mothership continues accepting and acknowledging
  those snapshots during the rendezvous.

Flash the mothership first when practical, then update nodes in small groups.
For field validation, start with two nodes containing several queued records and
confirm ordered `grant`, `done`, and `release ... confirmed=1` log lines before
updating the fleet.
