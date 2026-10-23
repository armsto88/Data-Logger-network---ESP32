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
   the nodes were previously deployed to another hub, erase or re-pair them
   first, or the roster stays empty.

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
production hub before they go anywhere. The bench NVS keys (`bench`) are
harmless and are ignored by production.
