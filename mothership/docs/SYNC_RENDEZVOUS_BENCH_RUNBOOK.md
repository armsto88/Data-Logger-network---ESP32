# Sync rendezvous bench: attendance against hub boot latency

This is the confirming experiment from section 10.4 of
`MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md`, and it runs before any Stage 0 or
Stage 3 work. It checks one claim: that the rendezvous aperture is
`25 - t_bootH` seconds, so nodes are lost because the hub is late, not because
the window is too short.

## What the bench build does

Env `mothership-v2-bench-sync-sweep` is the production firmware plus two
bench-only changes, both behind compile flags that production never sets:

- Before each rendezvous the hub delays by the next step of 0, 5, 10 ... 45 s
  and prints `[BENCH] window=N inject_ms=.. boot_ms=.. to_slot_s=..`. The step
  counter is in NVS and restarts at 0 on a new build.
- The sync period is 4 minutes per 1 minute of recording interval, instead of
  18, so one ten-window sweep takes about 40 minutes.

Everything else, including the join window, the grant loop and the constants the
note wants to change, is untouched. **Never flash this build for field use.**

## What it can and cannot show

- It shows whether attendance falls as the injected delay grows, and where. The
  note predicts full attendance below about 15 s of boot time and a collapse
  toward 0-1 attendees above about 25 s. Flat attendance across the sweep means
  the mechanism in section 3.1 is wrong, and Stage 3 must not be designed yet.
- With two nodes it cannot show drain-rate limits (Stage 2) or anything about
  fleet size. One hub and two nodes in one room also cannot reproduce RF
  contention between many nodes.
- A single window per step is noisy. Leave it running for two sweeps (20
  windows, about 80 minutes) so each delay is sampled twice.

## Setup

1. **Hardware.** HUB-R6-01 on battery with SW9 in RUN, USB to the laptop, the
   SD card fitted (so boot time is realistic), and both nodes powered a few
   metres away. Put **SW10 in the service position** for the flash.
2. **Flash and capture.**
   ```sh
   scripts/hub_bringup.py run HUB-R6-01 sync-sweep
   ```
   This builds and flashes the bench env, then logs every line with a timestamp
   and reconnects each time the hub cuts its own power. When the flash has
   finished, **slide SW10 back to RUN**, or the hub will never power itself off
   between windows.
3. **Commission** from the hub's own pages (press the config button, join the
   hub's Wi-Fi, open `192.168.4.1`):
   - set the hub clock;
   - pair both nodes and deploy them with a **recording interval of 1 minute**.
     The hub then shows a sync interval of **4 minutes**; if it shows 18, the
     bench build did not flash;
   - leave the data destination on **Local storage only**, so no modem time
     adds to the hub's awake period;
   - start the deployment, then shut the hub down from the page.
4. **Check the first window.** At the next sync slot the log should show
   `[BENCH] window=0 inject_ms=0 ...` followed by `[SYNC] roster +` lines. If
   the nodes were previously deployed to another hub, free them first (next
   section), or the roster stays empty.

## Freeing nodes that belong to another hub

A deployed node drops every deploy, time-sync and sync-window packet from a hub
other than the one it is bound to, and the test hub's pages only pair nodes that
report themselves unpaired. There is no on-device wipe. Unpairing from the owning
hub works, but it also clears the node on that hub (the deployment is archived and
its name, number and coordinates are cleared), so it is the wrong way to borrow a
node from a live deployment. Use spare nodes if you have them. Otherwise, with one
node on USB at a time (`esptool.py` from the PlatformIO `tool-esptoolpy` package):

1. **Back up its NVS:** `read_flash 0x9000 0x5000 <node>.bin --after no_reset`.
   Keep the file outside any directory that `git clean` touches.
2. **Erase it:** `erase_region 0x9000 0x5000`, then boot it once. It logs
   `no valid config record; using defaults`, comes up `UNPAIRED` and broadcasts
   `NODE_STATUS`, so the test hub lists it under New. Its sensor selection is
   cleared and is set again in the hub's wizard. An unpaired node powers itself
   off 15 minutes after boot, so power-cycle it just before pairing.
3. **To give it back:** `write_flash 0x9000 <node>.bin`, read the partition back
   and compare hashes, boot it once and check `[CFG]` shows `state=2 deployed=1`,
   the original sync interval and the original hub, and that its clock is right.
   The RTC is not in NVS and the test hub's deploy sets it.

Never reset a node three times within 20 s: that is the rescue gesture. It keeps
the config but leaves the node awake. Every esptool run that ends in a hard reset
counts as a boot.

## Reading the result

Stop the runner with Ctrl-C when 20 windows have run, then:

```sh
scripts/sync_sweep_report.py hardware/FieldHub_v2/revisions/2026-10-10-r6/bringup/HUB-R6-01/<stamp>_sync-sweep.log
```

It prints one row per window and a summary per injected delay, with the
predicted aperture beside the observed attendance. `t_boot_s` is the hub's boot
time as the equation defines it, including the injected delay. Windows marked
`SESSION-TIMEOUT`, `INCOMPLETE` or `NO-RENDEZVOUS` did not run normally and
should be read from the raw log before being counted.

## Afterwards

Reflash production firmware (`mothership-v1-main`). The nodes keep the 4-minute
sync schedule until a hub deploys them again, so redeploy them from the
production hub before they go anywhere, or restore a backed-up node as described
above. The bench NVS keys (`bench`) are harmless and are ignored by production.

## First run (2026-10-24)

HUB-R6-01 on commit `0f00c8c`, two nodes (ENV_6C0AA0 and ENV_D13F98, node
firmware build `9c96213`), 20 windows with `--lead 10`. The base hub boot was
1.3-1.6 s (no modem). Log: `bringup/HUB-R6-01/20261024-102749_sync-sweep.log`.

| Injected delay | Windows | Attended |
| --- | --- | --- |
| 0, 5, 10, 15, 20, 25, 30 s | 0-6 and 10-16 | 2/2 in all 14 |
| 35, 40, 45 s | 7-9 and 17-19 | 0/2 in all 6 |

Attendance is full while the predicted overlap is 3 s or more and zero once it
reaches 0, in both sweeps, at the edge the aperture equation gives (`t_bootH` of
35 s). The cliff is bracketed between `t` of about 32 s and 37 s, not pinned. The
window after the empty ones drained 16 queued readings with no loss, so a missed
window costs delay, not data.

What it does not show: anything about fleet size or drain rate (two nodes), the
production hub's real `t_bootH` (the bench boot has no modem, and the note's own
live fleet logs show a typical join window of about 22 s, so the losses would
come from occasional slow boots), or why a window failed. Each node reported a
queue of about 2 readings per 4 minutes at a 1-minute recording interval, where 4
would be expected. That was not investigated.

Because nodes stop listening at slot + 25 s (`SYNC_MARKER_GRACE_SEC`), a hub that
arrives later than that gets nobody however long it keeps accepting joiners. The
same sweep with only late-joiner acceptance would therefore be expected to look
like this one. The change that moves the cliff is the pre-wake lead (note
section 8, Stage 0a), which needs `kJoinCapMs` raised with it.
