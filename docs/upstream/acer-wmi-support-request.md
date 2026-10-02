# Upstream request — mainline acer-wmi support for Acer Aspire A715-79G

This file holds two ready-to-submit reports for bringing the Acer Aspire
A715-79G under mainline kernel support. The problem is real and reproduced.
The goal is that a stock Fedora/upstream kernel activates the EC automatic
fan control without custom modules.

> **Status: proposal 2 retracted (2026-10-02).** Firmware tables
> (`acpidump` → `iasl -d`, `dsdt.dsl`) show `SCMD(0x69)` is a 4-bit channel
> mask that writes `0xFF` to one EC register per set bit, not a profile
> selector. Proposal 1 stands. See "Correction" below.

## Problem summary

- DMI: Acer Aspire A715-79G, BIOS 1.07.01TACI 03/30/2024
- EC: ITE 8222 (ACPI EC at 0x62/0x66)
- WMI GUIDs exposed by the platform:
  - ABBC0F6B-8EA1-11D1-00A0-C90629100000
  - ABBC0F6C-8EA1-11D1-00A0-C90629100000
  - ABBC0F6D-8EA1-11D1-00A0-C90629100000
  - F6CB5C3C-9CAE-4EBD-B577-931EA32A2CC0 (MXM / NVIDIA mux, not fan-related)
- `modinfo acer_wmi` on 7.1.13 / 7.2.5 exposes these aliases only:
  - `wmi:676AA15E-6A47-4D9F-A2CC-1E6D18D14026`
  - `wmi:6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3`
  - `wmi:67C3371D-95A3-4C37-BB61-DD47B491DAAB`

None of the aliases match the platform GUIDs, so stock `acer-wmi` never
probes. Verified two ways: live on Fedora 44 (7.1.13 and 7.2.5) with no
out-of-tree modules loaded, `/sys/bus/wmi/devices/<ABBC0F6B/6C/6D>/driver`
is empty; and in the firmware tables, `\_SB.WMI._WDG` (`dsdt.dsl:95736`)
lists exactly those three GUIDs, none of which appear in the in-tree
device table. (A custom out-of-tree module currently binds them as a
workaround; the request below is to make stock kernels do the job.)

## Impact

The EC firmware's fan channels stay at their boot state on Linux. Observed
on this machine: fan 1 sits at ~2700 RPM at idle with the CPU package at
69-73 °C, and does **not** ramp for any load on any channel setting within
30 s — 2-core, 4-core and 8-core sustained loads all left it at ~2700 RPM
with the package at 94-96 °C. Only minutes-long full-package load was ever
seen to move it (~4900 RPM, one uncontrolled observation).

Two things start the fans beyond that:

1. Pressing Fn+1 (Cooler Boost) manually each session.
2. A duty write through `SCMD(0x68)`, which can pin a channel to a fixed
   value — including latching the CPU fan off at byte0=0 until CoolerBoost
   is cycled.

## What the firmware actually implements

From the ACPI tables captured 2026-10-02 and disassembled with `iasl -d`
(see `docs/reverse-engineering.md` for the full walkthrough):

- `\_SB.WMI.SCMD` dispatches on `Arg1`:
  - `SCMD(0x68, ARGS)` (`dsdt.dsl:97162`) writes four duty bytes from
    `ARGS` to EC registers 0x01-0x04.
  - `SCMD(0x69, ARGS)` (`dsdt.dsl:97197`) treats `ARGS` as a 4-bit mask
    and writes the literal `0xFF` to EC register 0x01/0x02/0x03/0x04 for
    each set bit — a per-channel run-at-full command.
- The same two bodies are duplicated in `\_SB.DCHU.SCMD`
  (`dsdt.dsl:106617`, `dsdt.dsl:106652`).
- Sweeping every literal `ECMD` argument buffer in the DSDT, the only EC
  register writes the firmware makes are duty `0x00`/`0xFF` on indices
  0x01-0x04, plus `0xC4` (event enable), `0xC2` and `0xD8`. There is no
  profile register, and no thermal-policy register among them.

**No `platform_profile` mapping can be built on these commands.** The
quiet/balanced/performance/gaming encoding this report previously claimed
does not exist in the firmware.

## Requested upstream change

1. Add the three GUIDs to `acer_wmi_id_table[]` in
   `drivers/platform/x86/acer-wmi.c` so the driver binds on this platform.
   This is the only part of the original request that survives verification.

Not requested, and no longer claimed: selecting a fan profile at boot.

## Correction (2026-10-02)

The original version of this document asserted that
`SCMD(0x69, BIT(profile-1))` selected a fan profile, with 1/2/4/8 mapping to
quiet/balanced/performance/gaming, and that writing a profile at boot
restored automatic fan control. Both claims were wrong:

- The disassembly shows `0x69` writes `0xFF` to a channel register, so
  `BIT(profile-1)` = 1 pins fan 1 to maximum rather than quieting it.
- Controlled A/B runs across the four channel settings and 2/4/8-core
  loads showed no separation in RPM or duty, which is what a
  wrong-register write looks like, not four overlapping fan curves.

Sending the `acpidump` to the maintainer is what surfaced this. Any
proposal built on the profile interpretation should be disregarded.

---

## Draft A — bugzilla.kernel.org

Fields to select:

- Product: Drivers
- Component: Platform
- Hardware: x86_64
- Severity: normal
- Version: 7.2.5-200.fc44 (also fails on 7.1.13-200.fc44, both Fedora 44)

Title:
> acer-wmi: no device matching for Acer Aspire A715-79G — three WMI GUIDs
> exposed but unclaimed

Body:

```
Summary:
The Acer Aspire A715-79G (BIOS 1.07.01TACI, ITE 8222 EC) exposes WMI GUIDs
ABBC0F6B / ABBC0F6C / ABBC0F6D, but acer-wmi carries no matching modalias,
so it never binds on stock kernels. No in-tree driver claims those GUIDs.

Reproduction:
1. Cold boot kernel 7.1.13 or 7.2.5 (Fedora 44).
2. Observe: /sys/bus/wmi/devices/<ABBC0F6B/6C/6D>/driver is empty and no
   acer-wmi module loads.

Evidence:
- modinfo acer_wmi aliases:
  676AA15E-6A47-4D9F-A2CC-1E6D18D14026
  6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3
  67C3371D-95A3-4C37-BB61-DD47B491DAAB
- Platform GUIDs (present, unbound), from \_SB.WMI._WDG in the DSDT:
  ABBC0F6B-8EA1-11D1-00A0-C90629100000
  ABBC0F6C-8EA1-11D1-00A0-C90629100000
  ABBC0F6D-8EA1-11D1-00A0-C90629100000

Requested:
- Add ABBC0F6B / ABBC0F6C / ABBC0F6D to acer-wmi's wmi_device_id_table.

Not requested: fan profile control. The ACPI tables show SCMD 0x69 is a
4-bit channel mask that writes 0xFF to a channel register, with no
profile or thermal-policy register among the firmware's EC writes, so
there is nothing for acer-wmi to map onto platform_profile. An earlier
version of this report claimed otherwise; the dump corrected it.
```

---

## Draft B — mailing list (platform-driver-x86@vger.kernel.org)

Subject:
> [RFC] acer-wmi: add Aspire A715-79G WMI GUIDs and EC fan profile support

Body:

```
Hi all,

The Acer Aspire A715-79G (ITE 8222 EC) exposes Acer WMI GUIDs
ABBC0F6B/ABBC0F6C/ABBC0F6D which are absent from acer-wmi's device table,
so the driver does not bind and no in-tree driver claims them.

Proposal:
1. Add the three GUIDs to acer_wmi_id_table[] in drivers/platform/x86/acer-wmi.c.

Attached is an acpidump of this machine, so the WMI objects and EC command
paths can be read directly rather than taken on trust.

Correction to my first version of this patch. I claimed the EC selects a
fan profile through \_SB.WMI SCMD 0x69, with BIT(profile-1) mapping
1/2/4/8 to quiet/balanced/performance/gaming, and that writing a profile
at boot restored automatic fan control. That is wrong, and the disassembly
says so. SCMD 0x69 (dsdt.dsl:97197) treats Arg1 as a 4-bit mask and writes
the literal 0xFF to EC register 0x01/0x02/0x03/0x04, one write per set
bit, so BIT(profile-1)=1 pins fan 1 to maximum rather than quieting it.
The same 0x68 and 0x69 bodies are duplicated verbatim in \_SB.DCHU.SCMD
(dsdt.dsl:106617 and :106652).

Sweeping every literal ECMD argument buffer in the DSDT, the firmware's
only EC register writes are duty 0x00 and 0xFF on indices 0x01-0x04, plus
0xC4 (event enable), 0xC2 and 0xD8. There is no profile register and no
thermal-policy register to map onto platform_profile.

My measurements contradict the original claim the same way. Across the four
channel settings and 2-core, 4-core and 8-core sustained loads, fan 1 held
~2700 RPM with the package at 94-96 C and no separation between settings.
So I withdraw the profile and platform_profile parts of the request, and
the ~2700 RPM "ramp" figure from the first version.

What the dump does confirm is the GUID half. \_SB.WMI._WDG
(dsdt.dsl:95736) lists exactly ABBC0F6B/6C/6D-8EA1-11D1-00A0-C90629100000,
and none appear in acer_wmi_id_table[], which is why the driver never
binds.

Full disassembly notes and a runnable reference module live here:
  https://github.com/ancxanas/acer-ec (docs/reverse-engineering.md)

Happy to test patches.
```