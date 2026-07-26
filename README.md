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
| `fan1_duty` | RO | Fan 1 duty cycle (0-255), EC-managed |
| `fan2_duty` | RO | Fan 2 duty cycle (0-255) |
| `fan1_rpm` | RO | Fan 1 speed (real RPM) |
| `fan2_rpm` | RO | Fan 2 speed (real RPM) |
| `fan3_rpm` | RO | Always 0 (unused slot) |
| `fan4_rpm` | RO | Always 0 (unused slot) |
| `fan1_duty_set` | WO | Write Fan 1 duty (experimental — likely no effect) |
| `fan2_duty_set` | WO | Write Fan 2 duty (0-255, e.g. `echo 0 > fan2_duty_set`) |
| `tmp_temp` | RO | ACPI temperature (deg C) |
| `dthl_val` | RO | Throttle level (bitmask) |
| `dtbp_val` | RO | Turbo boost power band |
| `airp_val` | RO | Airplane / temperature band |
| `winf_val` | RO | Windows interface flags |
| `rinf_val` | RO | Radio info flags |
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
│   ├── acer_wmi_extras.c       # WMI GUID 0xF6CB5C3C driver
│   ├── acer-ec.sh              # legacy EC-only CLI
│   └── profile                 # unified CLI (EC + CPU + GPU)
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
  Dynamic Boost. The GPU power limit is firmware-managed. The `profile` script
  sets EC fans and CPU power profile; GPU power is informational only.
- **No `dracut --force` needed.** Modules are loaded after rootfs via
  `modules-load.d`. Initramfs inclusion is unnecessary.
- **RPM values** are converted from raw tachometer periods. Formula:
  `RPM = 60,000,000 / raw`. Raw values ~40000 -> ~1500 RPM.

## CLI

The `install.sh` script installs two commands to `/usr/local/bin`:

### `profile` — unified power control

Controls EC fans, CPU power profile (power-profiles-daemon), and GPU power
limit (nvidia-smi) in one command:

```bash
profile                     # show help + current state
profile status              # show EC fans/temps + CPU + GPU status
profile quiet               # EC=1, CPU=power-saver, GPU=35W
profile balanced            # EC=2, CPU=balanced, GPU=45W  (default)
profile performance         # EC=3, CPU=performance, GPU=60W
profile gaming              # EC=4, CPU=performance, GPU=75W
```

Each profile sets all three subsystems simultaneously. Missing subsystems
(power-profiles-daemon or nvidia-smi) are silently skipped. GPU power limiting
is firmware-controlled on this laptop and cannot be changed via software.

### `acer-ec` — EC-only control

```bash
acer-ec                    # show status (default)
acer-ec status             # pretty-print fan/temp values
acer-ec profile            # show profile help
acer-ec profile gaming     # set profile by name
acer-ec profile 4          # set profile by number
```

EC profile names: `quiet` (1), `balanced` (2), `performance` (3), `gaming` (4).

## License

GNU General Public License v2.0 — see [LICENSE](LICENSE).
