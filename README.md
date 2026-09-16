# acer-ec — Acer EC communication framework

[!CAUTION]
> **This is experimental software.** Writing incorrect values can overheat your
> laptop. Fan 1 (CPU) is EC-protected and manual duty control has no effect.
> Use at your own risk.

## Supported hardware

- **Laptop** — Acer Aspire A715-79G (i7-13620H, RTX 3050 6GB)
- **EC** — ITE 8222 (ACPI EC at `0x62/0x66`)
- **ACPI WMI** — 3 GUIDs (ABBC0F6B, ABBC0F6C, ABBC0F6D)

Likely compatible with any Acer/Nitro/Predator laptop built on the same EC
firmware platform. Tested on **Fedora 44** with kernel **7.1.x**.

## What it does

| Sysfs file | Access | Description |
|---|---|---|
| `profile` | RW (1-4) | Fan profile: 1=quiet, 2=balanced, 3=performance, 4=gaming |
| `fan1_duty` | RO | Fan 1 EC internal state (0-255), NOT direct fan duty |
| `fan2_duty` | RO | Fan 2 EC internal state (0-255) |
| `fan1_rpm` | RO | Fan 1 speed (real RPM) |
| `fan2_rpm` | RO | Fan 2 speed (real RPM) |
| `fan3_rpm` | RO | Always 0 (unused slot) |
| `fan4_rpm` | RO | Always 0 (unused slot) |
| `fan1_duty_set` | WO | Write Fan 1 duty (experimental — likely no effect) |
| `fan2_duty_set` | WO | Write Fan 2 duty (0-255, e.g. `echo 0 > fan2_duty_set`) |
| `tmp_temp` | RO | ACPI temperature (deg C) |
| `dthl_val` | RO | Dynamic thermal headroom limit (15 = 15W boost budget) |
| `dtbp_val` | RO | Dynamic boost power ceiling (99 = max budget) |
| `airp_val` | RO | Airflow pressure sensor |
| `winf_val` | RO | Wind flow / intake info |
| `rinf_val` | RO | Rear intake info |
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
acer_wmi_extras.ko   # optional — WMI GUID 0xF6CB5C3C driver
```

- `acer_ec_core` exports `ec_core_read8`, `ec_core_read16`, `ec_core_scmd`
- `acer_fanctl` uses the core API, never touches hardware directly
- `acer_ec_debug` provides debugfs hex dumps of EC register ranges
- `acer_wmi_extras` binds to Acer-specific WMI GUID `F6CB5C3C` and exposes
  webcam toggle, TDP read/write, and panel overdrive via sysfs

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

### Kernel updates (DKMS)

`install.sh` uses DKMS when available. `dkms.conf` sets
`AUTOINSTALL="yes"`, so the modules rebuild automatically for each new
kernel that installs. A new kernel only needs its `kernel-devel` package:

```bash
sudo dnf install kernel-devel-$(uname -r)
```

DKMS keeps the modules consistent across kernels. To refresh the modules
for the running kernel:

```bash
sudo dkms uninstall -m acer-ec -v 0.1 --all
sudo dkms build -m acer-ec -v 0.1 -k $(uname -r)
sudo dkms install -m acer-ec -v 0.1 -k $(uname -r)
```

To force reinstall via `install.sh` (re-runs the DKMS add/build/install
flow and rewrites the modprobe/modules-load/sensors configs):

```bash
sudo ./install.sh
```

### Manual rebuild after kernel update (no DKMS)

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
│   ├── acer_wmi_extras.c       # WMI GUID 0xF6CB5C3C driver
│   ├── acer-ec.sh              # legacy EC-only CLI
│   └── profile                 # unified CLI (EC + CPU)
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
  Dynamic Boost DC controller. The GPU power limit is firmware-managed. The
  `profile` script sets EC fans and CPU power profile only.
- **`dut1` is not fan duty.** The register at 0xCE does not directly control
  fan RPM. The EC's internal thermal algorithm manages fan speed independently.
  `dut1` represents the EC's internal thermal budget allocation, which varies
  by profile. Compare RPM values across profiles to measure fan impact.
- **No `dracut --force` needed.** Modules are loaded after rootfs via
  `modules-load.d`. Initramfs inclusion is unnecessary.
- **RPM values** are converted from raw tachometer periods. Formula:
  `RPM = 60,000,000 / raw`. Raw values ~40000 -> ~1500 RPM. Values below
  raw 500 are treated as fan stopped (returns 0 RPM to prevent overflow).

## CLI

The `install.sh` script installs two commands to `/usr/local/bin`:

### `profile` — unified power control

Controls EC fans and CPU power profile (power-profiles-daemon) in one command.
GPU power is firmware-controlled and cannot be changed via software.

```bash
profile                     # show help + current state
profile status              # show EC fans/temps + CPU + GPU status
profile quiet               # EC=1: Fans off at idle, aggressive under load
profile balanced            # EC=2: Default moderate fan curve (default)
profile performance         # EC=3: Higher RPM threshold before throttling
profile gaming              # EC=4: Fans pre-cool at idle, more boost under load
```

Each profile sets EC fans and CPU power simultaneously. Missing subsystems
(power-profiles-daemon) are silently skipped. GPU power limit is
firmware-controlled on this laptop and cannot be changed via software.

### `acer-ec` — EC-only control

```bash
acer-ec                    # show status (default)
acer-ec status             # pretty-print fan/temp values
acer-ec profile            # show profile help
acer-ec profile gaming     # set profile by name
acer-ec profile 4          # set profile by number
```

EC profile names: `quiet` (1), `balanced` (2), `performance` (3), `gaming` (4).

## Research findings

### EC profile switching (confirmed working)

Profiles control the EC's thermal budget allocation strategy, not just fan
speed. Verified via direct register testing and stress tests:

| Profile | EC=1 (quiet) | EC=4 (gaming) |
|---|---|---|
| **Idle (79°C)** | Fan OFF (0 RPM) | Fan fast (12,330 RPM) |
| **Load (95°C)** | Fan MAX (15,600 RPM) | Fan slow (1,300 RPM) |

- **Quiet mode**: Fans off at idle for silence. Under load, ramps fans
  aggressively to prevent throttling while keeping power limited.
- **Gaming mode**: Fans pre-cool at idle for thermal headroom. Under load,
  allows higher CPU/GPU temps and runs fans slower, giving more thermal
  budget to compute boost.

The EC trades fan noise for compute performance. Gaming mode allocates more
thermal budget to CPU/GPU boost and less to fans. The labels are correct —
"quiet" means silent at idle, "gaming" means maximum performance under load.

### EC register 0xD7-0xDB (Dynamic Boost interface)

| Register | Value | Meaning |
|---|---|---|
| `dthl` (0xD7) | 15 | Dynamic thermal headroom limit = 15W |
| `dtbp` (0xD8) | 99 | Dynamic boost power ceiling = 99% |
| `airp` (0xD9) | 144 | Airflow pressure sensor |
| `winf` (0xDA) | 5 | Wind flow / intake info |
| `rinf` (0xDB) | 209 | Rear intake info |

These registers are static across profiles and temperatures — likely
read-only configuration from BIOS.

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
