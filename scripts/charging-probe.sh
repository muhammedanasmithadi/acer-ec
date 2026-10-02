#!/bin/bash
# charging-probe — controlled trigger ladder for the ACEREC-29 charging stall.
#
# Usage:
#   sudo ./scripts/charging-probe.sh [--allow-write] [--out DIR]
#
# Default is fully read-only: sysfs/upower/debugfs reads plus sleeps.
# --allow-write enables rung 2d only (one SCMD 0x69 mask-4 write, the
# least-intrusive channel: register 0x04, unused fan slot). Without the
# flag, rung 2d is skipped and the rest still runs.
#
# Run once while STALLED (Charging, 0 mA), save the log, then run again
# while CHARGING and diff the two full-window dumps (rung 3 files).
set -euo pipefail

ALLOW_WRITE=0
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --allow-write) ALLOW_WRITE=1; shift ;;
        --out) OUT="${2:?--out needs a directory}"; shift 2 ;;
        -h|--help)
            echo "Usage: sudo $0 [--allow-write] [--out DIR]"
            exit 0
            ;;
        *) echo "charging-probe: unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "charging-probe: run as root" >&2; exit 1; }

SYS=/sys/class/power_supply/BAT0
AC_SYS=/sys/class/power_supply/AC
EC=/sys/kernel/debug/acer_ec
FANCTL=/sys/kernel/acer_fanctl

[ -d "$SYS" ] || { echo "charging-probe: $SYS missing" >&2; exit 1; }
if [ ! -d "$EC" ]; then
    echo "charging-probe: $EC missing — run: modprobe acer_ec_debug" >&2
    exit 1
fi

if [ -z "$OUT" ]; then
    OUT="/tmp/opencode/charging-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUT"
LOG="$OUT/probe.log"
exec > >(tee "$LOG") 2>&1

echo "=== charging-probe $(date -Is) ==="
echo "allow_write=$ALLOW_WRITE out=$OUT"
echo "uptime: $(cut -d' ' -f1 /proc/uptime)s  load: $(cat /proc/loadavg)"
echo "AC online: $(cat "$AC_SYS/online" 2>/dev/null || echo ?)"

snap() {
    # $1 = label. One timestamped snapshot: sysfs + EC in the same instant.
    local label="$1"
    local now status cap cur charge volt ec_batt
    now="$(date +%T)"
    status="$(cat "$SYS/status")"
    cap="$(cat "$SYS/capacity")"
    cur="$(cat "$SYS/current_now")"
    charge="$(cat "$SYS/charge_now")"
    volt="$(cat "$SYS/voltage_now")"
    ec_batt="$(tr '\n' ' ' < "$EC/batt")"
    printf '%s [%-12s] sysfs status=%s cap=%s current=%s charge=%s volt=%s | ec %s\n' \
        "$now" "$label" "$status" "$cap" "$cur" "$charge" "$volt" "$ec_batt"
}

echo "--- rung 1: stuck-state baseline (3 samples, 5 s apart) ---"
snap "rung1-a"
sleep 5
snap "rung1-b"
sleep 5
snap "rung1-c"

echo "--- rung 2a: sysfs read class (cached, no EC I/O expected) ---"
snap "pre-2a"
cat "$SYS/charge_now" > /dev/null
cat "$SYS/status" > /dev/null
sleep 10
snap "post-2a"

if command -v upower > /dev/null 2>&1; then
    echo "--- rung 2b: upower read class ---"
    snap "pre-2b"
    BAT_PATH="$(upower -e 2>/dev/null | grep -E 'battery|BAT' | head -1 || true)"
    if [ -n "$BAT_PATH" ]; then
        upower -i "$BAT_PATH" > "$OUT/upower.txt"
    fi
    sleep 10
    snap "post-2b"
else
    echo "--- rung 2b: skipped (no upower) ---"
fi

echo "--- rung 2c: debugfs read class (ioread only, no EC command) ---"
snap "pre-2c"
cat "$EC/dump" > "$OUT/dump.txt"
cat "$EC/reg16_2A" > /dev/null
cat "$EC/batt" > /dev/null
sleep 10
snap "post-2c"

if [ "$ALLOW_WRITE" -eq 1 ]; then
    echo "--- rung 2d: SCMD mask-4 write (EXPLICIT, user confirmed) ---"
    snap "pre-2d"
    echo 4 > "$FANCTL/profile"
    echo "profile now: $(cat "$FANCTL/profile")"
    sleep 10
    snap "post-2d"
    echo "fan check: $(tr '\n' ' ' < "$FANCTL/all" | cut -c1-160)"
    echo "If a fan went quiet, cycle CoolerBoost (Fn+1) to restore it."
else
    echo "--- rung 2d: SKIPPED (re-run with --allow-write to test the SCMD trigger) ---"
fi

echo "--- rung 3: full-window dump for stalled-vs-charging diff ---"
cp "$OUT/dump.txt" "$OUT/dump-$(date +%H%M%S).txt"
echo "dump saved. While CHARGING, re-run this script and diff the two dump files;"
echo "bytes that move outside the known battery block are charger-state candidates."

echo "=== done. Log: $LOG ==="
