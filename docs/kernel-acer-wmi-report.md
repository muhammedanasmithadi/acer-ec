# Upstream request — mainline acer-wmi support for Acer Aspire A715-79G

This file holds two ready-to-submit reports for bringing the Acer Aspire
A715-79G under mainline kernel support. The problem is real and reproduced.
The goal is that a stock Fedora/upstream kernel activates the EC automatic
fan control without custom modules.

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
probes. Verified live on Fedora 44 (7.1.13 and 7.2.5): with no out-of-tree
modules loaded, `/sys/bus/wmi/devices/<ABBC0F6B/6C/6D>/driver` is empty.
(A custom out-of-tree module currently binds them as a workaround; the
request below is to make stock kernels do the job.)

## Impact

The EC firmware keeps its default (quiet-like) thermal profile on boot.
Until the OS selects a profile, the fans stay OFF at idle even at 60-75°C.
Reproduction is deterministic on every cold boot.

Only two things start the fans:

1. Pressing Fn+1 (Cooler Boost) manually each session.
2. Issuing the ACPI method `SCMD(0x69, BIT(profile-1))` — verified working,
   keeps fans ~1300 RPM at idle and ramps under heavy sustained all-core
   load. (Partial-core loads do not move the fan on any profile — measured;
   the EC only ramps for heavy sustained load.)

## Reverse-engineered EC profile control

From DSDT + register testing (see `docs/reverse-engineering.md`):

- ACPI method `SCMD` under `\_SB.WMI`:
  - `SCMD(0x68, buf[4])` — write fan duty (fan 2 / GPU only)
  - `SCMD(0x69, BIT(profile-1))` — select EC fan profile
- Profiles are selected by number 1–4, mapped to SCMD bit values
  1/2/4/8 (`BIT(profile-1)`): 1=quiet, 2=balanced, 3=performance, 4=gaming
- Default state (no SCMD) behaves like quiet: fan off at idle.

## Requested upstream change

1. Add the three GUIDs to `acer_wmi_id_table[]` in
   `drivers/platform/x86/acer-wmi.c` so the driver binds on this platform.
2. In `acer-wmi` (or via `platform_profile`), select the EC fan profile at
   boot (`SCMD(0x69, BIT(profile-1))`), defaulting to balanced, so stock
   kernels activate automatic fan control.

---

## Draft A — bugzilla.kernel.org

Fields to select:

- Product: Drivers
- Component: Platform
- Hardware: x86_64
- Severity: normal
- Version: 7.2.5-200.fc44 (also fails on 7.1.13-200.fc44, both Fedora 44)

Title:
> acer-wmi: no device matching for Acer Aspire A715-79G — EC automatic fan
> control inactive on boot

Body:

```
Summary:
The Acer Aspire A715-79G (BIOS 1.07.01TACI, ITE 8222 EC) exposes WMI GUIDs
ABBC0F6B / ABBC0F6C / ABBC0F6D, but acer-wmi carries no matching modalias,
so it never binds on stock kernels. With no in-tree driver claiming the
GUIDs, the EC keeps its default quiet thermal profile and fans stay OFF
at idle (60-75 C). The fan starts only after pressing Fn+1 (Cooler Boost)
or writing to the EC profile via the ACPI SCMD method.

Reproduction:
1. Cold boot kernel 7.1.13 or 7.2.5 (Fedora 44).
2. Observe: fans off, CPU ~60-75 C, no fan in sensors, no acer-wmi loaded.

Evidence:
- modinfo acer_wmi aliases:
  676AA15E-6A47-4D9F-A2CC-1E6D18D14026
  6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3
  67C3371D-95A3-4C37-BB61-DD47B491DAAB
- Platform GUIDs (present, unbound):
  ABBC0F6B-8EA1-11D1-00A0-C90629100000
  ABBC0F6C-8EA1-11D1-00A0-C90629100000
  ABBC0F6D-8EA1-11D1-00A0-C90629100000

Workaround (verified working):
Loader applies SCMD(0x69, BIT(2-1)) = balanced profile at boot. Fans then
run ~2700 RPM idle and ramp under heavy sustained all-core load
(partial-core loads do not move the fan — measured); automatic on every
boot.

Requested:
- Add ABBC0F6B / ABBC0F6C / ABBC0F6D to acer-wmi's wmi_device_id_table.
- Issue the EC fan-profile SCMD at boot or expose platform_profile so the
  fan curve is activated on stock kernels.
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
so the driver does not bind. Consequence on Linux: the EC stays in its
default (quiet) thermal profile and the fans never spin automatically at
idle (60-75 C); on Windows they are presumably activated by the NitroSense
driver via the EC profile command (unverified).

Reverse-engineering of the platform (DSDT + register experiments) shows the
EC selects its fan profile through the \_SB.WMI SCMD ACPI method:
  SCMD(0x69, BIT(profile-1)) with profile numbers 1-4 mapping to bit
  values 1/2/4/8: 1=quiet 2=balanced 3=performance 4=gaming
Setting a profile at boot restores automatic fan control (verified
on 7.1.13 and 7.2.5: ~2700 RPM idle, ramps under heavy sustained all-core
load; partial-core loads do not move the fan on any profile).

Proposal:
1. Add the three GUIDs to acer_wmi_id_table[] in drivers/platform/x86/acer-wmi.c.
2. On probe (or via platform_profile), select the EC profile so stock kernels
   control the fans automatically.

Full evidence and a runnable reference module live here:
  https://github.com/ancxanas/acer-ec (docs/reverse-engineering.md)

Happy to test patches.
```