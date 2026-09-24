# acer-ec — Acer EC communication framework

[!CAUTION]
> **This is experimental software.** Writing incorrect values can overheat your
> laptop. Fan 1 (CPU) is EC-protected and manual duty control has no effect.
> Use at your own risk.

## Supported hardware

- **Laptop** — Acer Aspire A715-79G (i7-13620H, RTX 3050 6GB)
- **EC** — ITE 8222 (ACPI EC at `0x62/0x66`)
- **ACPI WMI** — 3 GUIDs (ABBC0F6B, ABBC0F6C, ABBC0F6D)

Supported and tested only on the machine above — the kernel module refuses
to load elsewhere (DMI allowlist). Porting to another Acer on the same EC
firmware platform means changing the DMI table in `src/acer_ec_core.c` and
re-verifying the `0xFE0B0100` SystemMemory address against that machine's
DSDT. Tested on **Fedora 44** with kernel **7.2.5**.

## What it does

| Sysfs file | Access | Description |
|---|---|---|
| `profile` | RW (1-4) | Fan profile: 1=quiet, 2=balanced, 3=performance, 4=gaming. Reading it back returns the last profile requested via SCMD (0 = unknown) — driver-tracked, not read from the EC; use it to confirm the write path, not EC state |
| `fan1_duty` | RO | Fan 1 EC internal state (0-255), NOT direct fan duty |
| `fan2_duty` | RO | Fan 2 EC internal state (0-255) |
| `fan1_rpm` | RO | Fan 1 speed (real RPM) |
| `fan2_rpm` | RO | Fan 2 speed (real RPM) |
| `fan3_rpm` | RO | Always 0 (unused slot) |
| `fan4_rpm` | RO | Always 0 (unused slot) |
| `fan1_duty_set` | WO | Write Fan 1 duty (experimental — likely no effect) |
| `fan2_duty_set` | WO | Write Fan 2 duty (0-255, e.g. `echo 0 > fan2_duty_set`) |
| `tmp_temp` | RO | ACPI temperature (deg C) |
| `dthl_val` | RO | EC throttle-level value (bitmask, semantics unconfirmed) |
| `dtbp_val` | RO | EC boost-power value (semantics unconfirmed) |
| `airp_val` | RO | EC-internal value (semantics unconfirmed) |
| `winf_val` | RO | EC Windows-interface flags (do not write) |
| `rinf_val` | RO | EC-internal value (varies between sessions) |
| `all` | RO | One-shot dump of all values |

### Examples

```bash
# Set balanced profile
echo 2 > /sys/kernel/acer_fanctl/profile

# Set GPU fan to 100% (max)
echo 255 > /sys/kernel/acer_fanctl/fan2_duty_set

# Stop GPU fan (0% duty, fan coasts)
echo 0 > /sys/kernel/acer_fanctl/fan2_duty_set

# Read everything
cat /sys/kernel/acer_fanctl/all
```

## Architecture

Four kernel modules, loaded in order:

```
acer_ec_core.ko      # infrastructure — ioremap + ACPI handle
    ↓ depends
acer_fanctl.ko       # consumer — sysfs fan/temp interface
acer_ec_debug.ko     # optional — debugfs hex dumps
acer_wmi_extras.ko   # optional — unclaimed Acer WMI GUIDs (camera key, event log)
```

- `acer_ec_core` exports `ec_core_read8`, `ec_core_read16`, `ec_core_scmd`
- `acer_fanctl` uses the core API, never touches hardware directly
- `acer_ec_debug` provides debugfs hex dumps of EC register ranges
- `acer_wmi_extras` binds the otherwise-unclaimed Acer WMI GUIDs
  (`ABBC0F6C` webcam events → KEY_CAMERA input; `ABBC0F6D` event channel
  logged only) so they don't sit driverless

## Install

```bash
git clone https://github.com/ancxanas/acer-ec.git
cd acer-ec
chmod +x install.sh
sudo ./install.sh
```

### Requirements

- `pahole` (from the `dwarves` package) — needed for BTF module support.
  Without it, modules will fail to load with "Exec format error".
- Kernel headers: `kernel-devel-$(uname -r)`

```bash
sudo dnf install dwarves kernel-devel-$(uname -r)
```

### Manual rebuild after kernel update

```bash
# Sync kernel-devel config with running kernel (avoids struct module mismatch)
cp /boot/config-$(uname -r) /usr/src/kernels/$(uname -r)/.config
make -C /usr/src/kernels/$(uname -r) olddefconfig
make -C /usr/src/kernels/$(uname -r) modules_prepare

# Build and install
make -C /lib/modules/$(uname -r)/build M=$(pwd) clean modules
sudo make -C /lib/modules/$(uname -r)/build M=$(pwd) modules_install
sudo depmod -a
sudo modprobe -r acer_wmi_extras acer_fanctl acer_ec_core
sudo modprobe acer_fanctl
```

## Uninstall

```bash
sudo ./uninstall.sh
```

## Files

```
acer-ec/
├── src/
│   ├── acer_ec_core.c          # core — ioremap + ACPI SCMD
│   ├── acer_ec_core.h          # core API header
│   ├── acer_fanctl.c           # fan profile + sysfs interface
│   ├── acer_ec_debug.c         # debugfs hex dumps
│   ├── acer_wmi_extras.c       # WMI binder (ABBC0F6C webcam key, ABBC0F6D event log)
│   ├── acer-ec.sh              # EC CLI (install.sh copies it to /usr/local/bin/acer-ec)
├── Makefile
├── install.sh
├── uninstall.sh
├── LICENSE              (GPL-2.0)
├── README.md
└── docs/
    └── reverse-engineering.md
```

## Limitations

- **Fan 1 (CPU) is EC-protected.** Manual duty writes via `fan1_duty_set` are
  accepted but the EC firmware overwrites DUT1 within ~500ms. Use `profile`
  switching for indirect CPU fan control.
- **GPU power limiting is not supported on this laptop.** NVIDIA driver 530+
  removed `nvidia-smi -pl` for laptop GPUs, and the Acer BIOS disables
  Dynamic Boost DC controller. The GPU power limit is firmware-managed.
  CPU power is managed separately (e.g. power-profiles-daemon); `acer-ec`
  sets EC fans only.
- **SCMD 0x68 programs both fan channels in one call.** The driver
  always does read-modify-write (verified live Sep 2026: a byte0=0 write
  latched the CPU channel to 0 and stopped the CPU fan until CoolerBoost
  was cycled — profile switching does NOT clear manual duty state).
  Never assume a duty write preserves the other channel. Recovery from
  a stuck channel: Fn+1 CoolerBoost on/off.
- **`dut1` is read-only in practice.** The register at 0xCE reflects EC fan
  state, but writing it does not change fan RPM — the EC firmware
  overwrites it within ~500ms. Use `profile` switching for indirect
  CPU fan control.
- **No `dracut --force` needed.** Modules are loaded after rootfs via
  `modules-load.d`. Initramfs inclusion is unnecessary.
- **RPM values** are converted from raw tachometer periods. Formula:
  `RPM = 120,000,000 / raw` — the EC counts both tach edges
  (dual-transition). Calibrated against Acer's Windows utility (~5000
  RPM at max): CoolerBoost and all-core ramp both converge to ~4800–4900.
  Raw values ~44000 -> ~2700 RPM idle. Values below raw 500 are treated
  as fan stopped (returns 0 RPM to prevent overflow). Transition
  readings during fast RPM slews are unreliable (torn 16-bit EC updates)
  — trust settled-state values only.

## CLI

`install.sh` copies `src/acer-ec.sh` to `/usr/local/bin/acer-ec`:

```bash
acer-ec                    # show status (default)
acer-ec status             # pretty-print fan/temp values
acer-ec profile            # show current profile + help
acer-ec profile gaming     # set profile by name
acer-ec profile 4          # set profile by number
```

EC profile names: `quiet` (1), `balanced` (2), `performance` (3), `gaming` (4).
Setting a profile verifies the write by reading it back and errors out if
the EC reports a different value.

## Research findings

### EC profile switching (re-verified Sep 2026)

Controlled A/B across profiles 2/3/4 — same machine, same loads:

| Condition | dut1 | Fan1 RPM | EC TMP | Package |
|---|---|---|---|---|
| Idle, any profile | 71 | ~1370 | 70–73°C | 69–73°C |
| 2-core load 20 s, profile 3 | 71 (frozen) | ~1370 | 96°C | 94–96°C |
| 4-core load 24 s, profile 4 | 71 (frozen) | ~1370 | 96°C | 94–96°C |
| ~10-core sustained load, profile 2 | ramped | 2467 (single observation during a package update, not a controlled run) | — | 96°C |

Findings:

- The EC **does not ramp the CPU fan for partial-core loads on any
  profile** — duty stays at 71 even with its own sensor at 96°C.
- It **does ramp under heavy sustained all-core load** (2467 RPM observed).
- Profiles are hints to the EC's internal curve, not commands; in the
  partial-load regime all three tested profiles behave identically.

> Supersedes earlier single-sample readings (12,330 / 15,600 RPM): those
> exceed plausible blower speeds and were likely tach-transition glitches,
> not steady-state values.
>
> Open: the GPU channel tach encoding. CPU is verified at 120M (matches
> Acer's Windows utility); GPU manual-mode readings (~14k at 120M) are
> implausible, suggesting a different per-channel encoding. Needs a
> settled GPU duty sweep, not another constant flip.

### EC register 0xD7-0xDB (Dynamic Boost interface)

| Register | Value | Meaning |
|---|---|---|
| `dthl` (0xD7) | 15 | Throttle level (bitmask, per DSDT analysis) |
| `dtbp` (0xD8) | 99 | Turbo boost power band (unconfirmed) |
| `airp` (0xD9) | 144 | Unknown EC-internal value |
| `winf` (0xDA) | 5 | Windows-interface flags; bit 0 suspected "OS takeover" (**UNTESTED — do not flip**) |
| `rinf` (0xDB) | 145 | Unknown (observed 145 and 209 across sessions — varies, not static) |

Meanings above are best-effort from DSDT analysis
(see `docs/reverse-engineering.md`), not confirmed semantics. Earlier notes
claiming these were "static BIOS configuration" were wrong — `rinf` varies
between sessions.

### Intel HWP Dynamic Boost

CPU-side dynamic boost can be enabled:
```bash
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost
```
This allows the CPU to boost higher when thermal headroom exists. Reversible
by writing 0. Only affects CPU, not GPU.

### EFI variables (research avenue)

The laptop exposes two interesting EFI variables via efivarfs:
- `Setup-ec87d643` (8368 bytes) — ALL Insyde H2O BIOS settings
- `OemNvPcfDcPwrLimitSetup` (1864 bytes) — NVIDIA DC power limit config

These could potentially be decoded to find BIOS-level Dynamic Boost toggle,
but this is a complex reverse-engineering task.

## License

GNU General Public License v2.0 — see [LICENSE](LICENSE).
