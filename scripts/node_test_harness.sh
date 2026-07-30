#!/usr/bin/env bash
#
# node_test_harness.sh — flash the node hardware self-test, gate on its
# verdict, then flash production firmware only if every required check passed.
#
#   1. build + flash  env:esp32wroom-selftest
#   2. capture the serial log until the self-test prints its SUMMARY line
#   3. parse the RESULT| lines into a table
#   4. any FAIL (or a missing SUMMARY) -> abort, production firmware NOT flashed
#   5. all clear -> build + flash env:esp32wroom (production)
#   6. read back the production boot banner as proof the real image is running
#
# Usage:
#   scripts/node_test_harness.sh [--port COMx] [--skip-wind] [--no-flash-prod]
#
#   --port COMx        serial port of the board under test (default: COM5,
#                      matching env:esp32wroom). Bench ports move around, so
#                      pass this explicitly and make sure it is not a deployed
#                      node or the mothership.
#   --skip-wind        skip the operator-prompted anemometer spin test. The
#                      wind path is then reported WARN and NOT verified.
#   --no-flash-prod    run the self-test and report, but stop before flashing
#                      production firmware.
#
# Logs land in node/firmware/tests/results/.

set -uo pipefail

# ───────────────────────────────────────────────────────────────────── settings

PORT="COM5"
SKIP_WIND=0
FLASH_PROD=1

# Self-test finishes on its own; this is only a safety net for a board that
# crashes or hangs. The wind window alone is 10 s.
MONITOR_TIMEOUT_S=90

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FW_DIR="$REPO_ROOT/node/firmware"
RESULTS_DIR="$FW_DIR/tests/results"

SELFTEST_ENV="esp32wroom-selftest"
PROD_ENV="esp32wroom"

# ──────────────────────────────────────────────────────────────────── arg parse

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      [[ $# -ge 2 ]] || { echo "error: --port needs a value" >&2; exit 2; }
      PORT="$2"; shift 2 ;;
    --port=*)        PORT="${1#*=}"; shift ;;
    --skip-wind)     SKIP_WIND=1; shift ;;
    --no-flash-prod) FLASH_PROD=0; shift ;;
    -h|--help)
      sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
      exit 0 ;;
    *)
      echo "error: unknown argument '$1' (try --help)" >&2
      exit 2 ;;
  esac
done

# ───────────────────────────────────────────────────────────────────── colours

if [[ -t 1 ]]; then
  C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
  C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YELLOW=$'\033[33m'; C_CYAN=$'\033[36m'
else
  C_RESET=""; C_BOLD=""; C_DIM=""
  C_GREEN=""; C_RED=""; C_YELLOW=""; C_CYAN=""
fi

hr()   { printf '%s\n' "------------------------------------------------------------------------"; }
info() { printf '%s==>%s %s\n' "$C_CYAN$C_BOLD" "$C_RESET" "$*"; }
die()  { printf '%sERROR:%s %s\n' "$C_RED$C_BOLD" "$C_RESET" "$*" >&2; exit 1; }

# ─────────────────────────────────────────────────────────────── preflight

# PlatformIO's launcher is often not on PATH under Git Bash even when Core is
# installed, so fall back to the standard penv location before giving up.
if command -v pio >/dev/null 2>&1; then
  PIO="pio"
elif [[ -x "$HOME/.platformio/penv/Scripts/pio.exe" ]]; then
  PIO="$HOME/.platformio/penv/Scripts/pio.exe"
elif [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
  PIO="$HOME/.platformio/penv/bin/pio"
else
  die "'pio' not found on PATH or in ~/.platformio/penv. Install PlatformIO Core or activate its venv."
fi

[[ -d "$FW_DIR" ]] || die "node firmware dir not found: $FW_DIR"

mkdir -p "$RESULTS_DIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
PORT_TAG="$(printf '%s' "$PORT" | tr -c '[:alnum:]' '_')"
LOG="$RESULTS_DIR/selftest_${PORT_TAG}_${STAMP}.log"

cd "$FW_DIR" || die "cannot cd to $FW_DIR"

hr
printf '%sNODE TEST HARNESS%s\n' "$C_BOLD" "$C_RESET"
printf '  port          : %s\n' "$PORT"
printf '  selftest env  : %s\n' "$SELFTEST_ENV"
printf '  production env: %s\n' "$PROD_ENV"
printf '  wind test     : %s\n' "$([[ $SKIP_WIND -eq 1 ]] && echo 'SKIPPED (--skip-wind)' || echo 'enabled (operator must spin the cup)')"
printf '  log           : %s\n' "$LOG"
hr
echo
printf '%sCheck that %s is the board under test — not a deployed node or the mothership.%s\n' \
  "$C_YELLOW" "$PORT" "$C_RESET"
echo

# ───────────────────────────────────────────── step 1: build + flash self-test

info "Step 1/6  Building and flashing self-test firmware to $PORT"

# --skip-wind is a compile-time flag, so it has to be injected at build time.
if [[ $SKIP_WIND -eq 1 ]]; then
  export PLATFORMIO_BUILD_FLAGS="-D SELFTEST_SKIP_WIND=1"
fi

if ! "$PIO" run -e "$SELFTEST_ENV" -t upload --upload-port "$PORT"; then
  die "self-test build/upload failed — board never flashed, nothing to parse.
       Check the port, cable, and that the board is in bootloader mode."
fi

unset PLATFORMIO_BUILD_FLAGS

echo
info "Step 2/6  Capturing self-test output (timeout ${MONITOR_TIMEOUT_S}s)"
if [[ $SKIP_WIND -eq 0 ]]; then
  printf '%sThe test will pause and ask you to SPIN THE ANEMOMETER CUP — watch for it.%s\n' \
    "$C_YELLOW$C_BOLD" "$C_RESET"
fi
echo

# ────────────────────────────────────────────── step 2: capture serial output

# The monitor runs in the background writing straight to the log, and a
# separate `tail -f` mirrors it to the terminal. Killing an explicit PID is far
# more reliable on Windows than relying on SIGPIPE to tear down a pipeline —
# a monitor left alive would still hold the COM port and break the production
# upload two steps later.
#
# --dtr 0 --rts 0 matches the env settings so attaching does not reset the board.
: > "$LOG"

"$PIO" device monitor -p "$PORT" -b 115200 \
  --quiet --no-reconnect --dtr 0 --rts 0 > "$LOG" 2>&1 &
MON_PID=$!

# Live mirror so the operator actually sees the "spin the cup" prompt.
stdbuf -oL tail -n +1 -f "$LOG" 2>/dev/null &
TAIL_PID=$!

cleanup_monitor() {
  kill "$TAIL_PID" 2>/dev/null || true
  kill "$MON_PID"  2>/dev/null || true
  wait "$TAIL_PID" 2>/dev/null || true
  wait "$MON_PID"  2>/dev/null || true
}
trap 'cleanup_monitor; exit 130' INT TERM

CAPTURED=0
DEADLINE=$((SECONDS + MONITOR_TIMEOUT_S))
while [[ $SECONDS -lt $DEADLINE ]]; do
  if grep -qaF 'RESULT|SUMMARY|' "$LOG" 2>/dev/null; then
    CAPTURED=1
    sleep 1   # let the trailing lines land before we tear the monitor down
    break
  fi
  # Monitor exited on its own (port vanished, board unplugged, ...).
  kill -0 "$MON_PID" 2>/dev/null || break
  sleep 1
done

cleanup_monitor
trap - INT TERM

# Give Windows a moment to actually release the COM port handle.
sleep 2

if [[ $CAPTURED -eq 0 ]]; then
  printf '%sTimed out after %ss without a SUMMARY line.%s\n' \
    "$C_YELLOW" "$MONITOR_TIMEOUT_S" "$C_RESET"
fi

echo
hr

# ────────────────────────────────────────────────── step 3: parse + report

info "Step 3/6  Self-test results"
echo

if [[ ! -s "$LOG" ]]; then
  die "no serial output captured from $PORT.
       The board may not have booted, or another program is holding the port."
fi

RESULT_LINES="$(grep -a '^RESULT|' "$LOG" || true)"

if [[ -z "$RESULT_LINES" ]]; then
  printf '%sNo RESULT lines found in the serial log.%s\n' "$C_RED$C_BOLD" "$C_RESET"
  printf 'Last 20 lines of %s:\n' "$LOG"
  tail -n 20 "$LOG"
  die "self-test produced no parseable output — production firmware NOT flashed."
fi

printf '%s  %-22s %-6s %s%s\n' "$C_BOLD" "CHECK" "STATUS" "DETAIL" "$C_RESET"
hr

FAIL_COUNT=0
WARN_COUNT=0
PASS_COUNT=0
SUMMARY_LINE=""

while IFS= read -r line; do
  # RESULT|<NAME>|<VERDICT>|<DETAIL>
  name="$(printf '%s' "$line" | cut -d'|' -f2)"
  verdict="$(printf '%s' "$line" | cut -d'|' -f3)"
  detail="$(printf '%s' "$line" | cut -d'|' -f4-)"

  if [[ "$name" == "SUMMARY" ]]; then
    SUMMARY_LINE="$line"
    continue
  fi

  case "$verdict" in
    PASS) colour="$C_GREEN"; PASS_COUNT=$((PASS_COUNT + 1)) ;;
    FAIL) colour="$C_RED";   FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
    WARN) colour="$C_YELLOW"; WARN_COUNT=$((WARN_COUNT + 1)) ;;
    *)    colour="$C_DIM" ;;
  esac

  printf '  %-22s %s%-6s%s %s\n' "$name" "$colour" "$verdict" "$C_RESET" "$detail"
done <<< "$RESULT_LINES"

hr
printf '  %s%d passed%s, %s%d failed%s, %s%d warnings%s\n' \
  "$C_GREEN" "$PASS_COUNT" "$C_RESET" \
  "$C_RED" "$FAIL_COUNT" "$C_RESET" \
  "$C_YELLOW" "$WARN_COUNT" "$C_RESET"
echo

# ───────────────────────────────────────────────────────────── step 4: gate

info "Step 4/6  Gate"

# A missing SUMMARY means the firmware died partway through — treat that as a
# failure even if every check printed so far said PASS.
if [[ -z "$SUMMARY_LINE" ]]; then
  printf '%sSelf-test never reached its summary — it hung or crashed mid-run.%s\n' \
    "$C_RED$C_BOLD" "$C_RESET"
  printf 'Full log: %s\n' "$LOG"
  die "incomplete self-test — production firmware NOT flashed."
fi

if [[ $FAIL_COUNT -gt 0 ]]; then
  printf '%sFAILED CHECKS:%s\n' "$C_RED$C_BOLD" "$C_RESET"
  printf '%s\n' "$RESULT_LINES" | awk -F'|' '$3 == "FAIL" { printf "  - %s: %s\n", $2, $4 }'
  echo
  printf 'Full log: %s\n' "$LOG"
  die "$FAIL_COUNT check(s) failed — production firmware NOT flashed."
fi

# The firmware counts its own checks, so if we saw fewer than it reported, the
# monitor attached late and we are looking at a partial log. Silently passing a
# board on a truncated capture is exactly the failure this harness exists to
# prevent.
SUMMARY_COUNTS="$(printf '%s' "$SUMMARY_LINE" | cut -d'|' -f3)"   # <pass>/<total>
SUMMARY_TOTAL="${SUMMARY_COUNTS#*/}"
SEEN_TOTAL=$((PASS_COUNT + FAIL_COUNT))

if [[ "$SUMMARY_TOTAL" =~ ^[0-9]+$ ]] && [[ "$SEEN_TOTAL" -ne "$SUMMARY_TOTAL" ]]; then
  printf '%sTruncated capture: firmware ran %s checks but only %s reached this log.%s\n' \
    "$C_RED$C_BOLD" "$SUMMARY_TOTAL" "$SEEN_TOTAL" "$C_RESET"
  printf 'Full log: %s\n' "$LOG"
  die "incomplete results — production firmware NOT flashed. Re-run the harness."
fi

# Belt and braces: honour the firmware's own overall verdict too.
if printf '%s' "$SUMMARY_LINE" | grep -q 'OVERALL:FAIL'; then
  printf 'Full log: %s\n' "$LOG"
  die "firmware reported OVERALL:FAIL — production firmware NOT flashed."
fi

printf '%sAll required checks passed.%s' "$C_GREEN$C_BOLD" "$C_RESET"
if [[ $WARN_COUNT -gt 0 ]]; then
  printf ' (%d warning(s) — review above.)' "$WARN_COUNT"
fi
echo
if [[ $SKIP_WIND -eq 1 ]]; then
  printf '%sNote: the anemometer was NOT verified (--skip-wind).%s\n' "$C_YELLOW" "$C_RESET"
fi
echo

# ──────────────────────────────────────────── step 5: flash production firmware

if [[ $FLASH_PROD -eq 0 ]]; then
  info "Step 5/6  Skipped (--no-flash-prod). Board still has SELF-TEST firmware."
  printf 'Flash production manually with:\n  cd node/firmware && pio run -e %s -t upload --upload-port %s\n' \
    "$PROD_ENV" "$PORT"
  printf 'Log: %s\n' "$LOG"
  exit 0
fi

info "Step 5/6  Flashing production firmware ($PROD_ENV) to $PORT"
echo

if ! "$PIO" run -e "$PROD_ENV" -t upload --upload-port "$PORT"; then
  printf 'Log: %s\n' "$LOG"
  die "production flash FAILED — the board still has self-test firmware on it.
       Retry, or flash manually: cd node/firmware && pio run -e $PROD_ENV -t upload --upload-port $PORT"
fi

# ─────────────────────────────────── step 6: read back the production banner

# esptool verifies the write, so this is not about whether the bytes landed —
# it is about giving the tester positive proof on screen that the board is now
# running the real node firmware and not the self-test image it was running
# sixty seconds ago. Greps for the banner in src/main.cpp setup().
echo
info "Step 6/6  Confirming production firmware is running"
echo

BOOTLOG="${LOG%.log}_prodboot.log"
: > "$BOOTLOG"

"$PIO" device monitor -p "$PORT" -b 115200 \
  --quiet --no-reconnect --dtr 0 --rts 0 > "$BOOTLOG" 2>&1 &
BOOT_MON_PID=$!

BOOT_DEADLINE=$((SECONDS + 25))
CONFIRMED=0
while [[ $SECONDS -lt $BOOT_DEADLINE ]]; do
  if grep -qaF 'FieldMesh Node:' "$BOOTLOG" 2>/dev/null; then
    CONFIRMED=1
    sleep 2   # let the version/MAC lines that follow the banner land
    break
  fi
  kill -0 "$BOOT_MON_PID" 2>/dev/null || break
  sleep 1
done

kill "$BOOT_MON_PID" 2>/dev/null || true
wait "$BOOT_MON_PID" 2>/dev/null || true
sleep 1

# A board still running the self-test would print its own banner instead.
if grep -qaF 'NODE FULL SELF-TEST' "$BOOTLOG" 2>/dev/null; then
  printf '%sBoard is still running SELF-TEST firmware, not production.%s\n' \
    "$C_RED$C_BOLD" "$C_RESET"
  printf 'Boot log: %s\n' "$BOOTLOG"
  die "production firmware did not take effect — reflash before deploying."
fi

if [[ $CONFIRMED -eq 1 ]]; then
  printf '%sConfirmed — board is running production firmware:%s\n' "$C_GREEN$C_BOLD" "$C_RESET"
  echo
  grep -aE 'FieldMesh Node:|^Firmware: v|^Boot #|^MAC: |^\[fwid\]' "$BOOTLOG" \
    | head -n 6 | sed 's/^/    /'
  echo
else
  printf '%sCould not capture the boot banner within 25s.%s\n' "$C_YELLOW$C_BOLD" "$C_RESET"
  printf 'The flash itself succeeded (esptool verifies the write), so this is most\n'
  printf 'likely a monitor timing issue. Confirm manually with:\n'
  printf '  cd node/firmware && pio device monitor -p %s -b 115200\n' "$PORT"
  printf 'and look for the "FieldMesh Node:" banner. Boot log: %s\n' "$BOOTLOG"
  echo
fi

hr
printf '%sNODE READY%s — self-test passed and production firmware (%s) is flashed.\n' \
  "$C_GREEN$C_BOLD" "$C_RESET" "$PROD_ENV"
if [[ $SKIP_WIND -eq 1 ]]; then
  printf '%sReminder: the anemometer was NOT verified (--skip-wind).%s\n' "$C_YELLOW" "$C_RESET"
fi
printf 'Self-test log: %s\n' "$LOG"
printf 'Boot log     : %s\n' "$BOOTLOG"
hr
exit 0
