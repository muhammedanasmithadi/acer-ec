# Charging stall — diagnosed: pack protection latch (ACEREC-29)

## Fault (confirmed 2026-10-02)

The pack drained to empty while tethered (14 % → 0 % over hours, AC
connected, `current_now` 0 mA the whole time), latched its own
protection, and refused charge afterwards: orange LED (EC requesting
charge), `status=Charging`, but 0 mA and a zeroed gauge (`present_mah=0`,
`soc_pct=0` in the EC mirror, `capacity=0`/`charge_now=0` in sysfs). The
machine then shut itself off and could not boot at 0 % without AC.

## Recovery (verified)

Shut down → unplug AC → wait 20–30 min → re-attach AC → boot. Charging
resumed immediately at 2.925 A / ~45 W. The unplugged off-window is the
active ingredient: hours tethered never recovered it. At 0 % the pack
cannot boot the machine at all — AC must be attached before power-on.

## What was ruled out

- **Kernel misreport.** EC `0x2A`/`0x2E`/`0x16`/`0x1A` match sysfs to the
  exact digit in both states. Both paths faithfully reported zero.
- **Trigger-by-reading.** sysfs, upower, and debugfs reads across 60 s left
  current pinned at 2.925 A. No read class touches charger state, so the
  EC-command hypothesis is retired and rung 2d (`profile` write) stays
  permanently off the table — there is no hypothesis left that it tests.
- **Adapter/cable.** 150 W brick (20 V × 7.5 A) powers the system, LED goes
  orange, replug watch showed 20/20 zero reads with no flicker.

## What is still unexplained

Why the pack drained *while tethered*. If AC was attached the whole time,
the EC never opened the charge path at 14 % and the system silently ran on
battery to death — that would be an EC power-path fault and the actual
thing worth escalating. If the machine was unplugged at any point in that
window, there may be no fault beyond the latch itself.

## Standing rule

Treat `status=Charging` with `current_now=0` for more than a minute or two
as an emergency, not an observation: shut down, unplug, soak 20–30 min.
A second latch *without* a deep drain means the pack is failing, not
unlucky — escalate then (see below), do not just soak again.

## Escalation path (real solution, in order)

1. Check Acer's support site for a BIOS newer than 1.07.01TACI (fwupd/LVFS
   absence proves nothing — Acer rarely publishes there).
2. Check warranty status (serial number on the bottom cover). If covered
   and it recurs, file a battery/charging service request.
3. Recurrence with proof of tethered drain (logs showing AC online
   continuously while capacity falls) is the evidence Acer needs — it
   indicates an EC power-path fault, which is a board/firmware matter no
   EC poking from Linux can fix.
4. No charger *write* path will be added to `acer-ec`: the pack's own
   controller owns that decision, and blind writes risk the latch class
   of fault this document describes.

## Instrument

`acer_ec_debug` exposes the block read-only: `reg16_16` / `reg16_1A` /
`reg16_22` (design-voltage constant, not telemetry) / `reg16_2A` /
`reg16_2E`, `reg8_38`, `reg16_66` (unidentified counter, not a cutoff),
plus the decoded `batt` node. Probe ladder: `sudo ./scripts/charging-probe.sh`
(read-only by default; `--allow-write` retained only as a gate, not a
recommendation).
