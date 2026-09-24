#!/bin/bash
set -euo pipefail

KVERSION=$(uname -r)
MODDIR="/lib/modules/$KVERSION/extra"
PROBE_D="/etc/modprobe.d"
LOAD_D="/etc/modules-load.d"

echo "=== acer-ec installer ==="

# ---- 0. Preflight ----
command -v pahole &>/dev/null || { echo "ERROR: pahole not found. Install: sudo dnf install dwarves"; exit 1; }

# Sync kernel-devel .config with running kernel to avoid struct module size mismatch
KDEV_CFG="/usr/src/kernels/$KVERSION/.config"
BOOT_CFG="/boot/config-$KVERSION"
if [ -f "$BOOT_CFG" ] && [ -f "$KDEV_CFG" ]; then
    if ! grep -q "CONFIG_DEBUG_INFO_BTF_MODULES=y" "$KDEV_CFG" 2>/dev/null; then
        echo "Syncing kernel-devel config (missing BTF)..."
        cp "$BOOT_CFG" "$KDEV_CFG"
        make -C "/usr/src/kernels/$KVERSION" olddefconfig
        make -C "/usr/src/kernels/$KVERSION" modules_prepare
    fi
fi

# ---- 1. Remove old monolithic install if present ----
if lsmod | grep -q "^acer_fanctl"; then
    echo "Removing old acer_fanctl module..."
    rmmod acer_fanctl
fi
if [ -f "$MODDIR/acer_fanctl.ko" ] || [ -f "$MODDIR/acer_fanctl.ko.xz" ]; then
    echo "Cleaning old acer_fanctl.ko from $MODDIR"
    rm -f "$MODDIR/acer_fanctl.ko" "$MODDIR/acer_fanctl.ko.xz"
fi
if [ -f "$PROBE_D/acer_fanctl.conf" ]; then
    echo "Removing old modprobe config..."
    rm -f "$PROBE_D/acer_fanctl.conf"
fi
if [ -f "$LOAD_D/acer_fanctl.conf" ]; then
    echo "Removing old modules-load config..."
    rm -f "$LOAD_D/acer_fanctl.conf"
fi

# ---- 2. Build (DKMS with direct-make fallback) ----
DKMS_PKG=$(sed -n 's/^PACKAGE_NAME="\(.*\)"/\1/p' dkms.conf)
DKMS_VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)

if command -v dkms &>/dev/null; then
    echo "Building and installing via DKMS ($DKMS_PKG/$DKMS_VER)..."
    # Purge the DKMS tree for this module first. Without this, a stale
    # build cache can make `dkms build` relink old .o files (MODPOST-only,
    # ~1s, no CC lines) and silently reinstall the previous build.
    # A real build takes tens of seconds — watch for CC lines below.
    dkms remove -m "$DKMS_PKG" -v "$DKMS_VER" --all 2>/dev/null || true
    rm -rf "/var/lib/dkms/$DKMS_PKG"
    dkms add "$PWD"
    dkms build -m "$DKMS_PKG" -v "$DKMS_VER" -k "$KVERSION"
    dkms install -m "$DKMS_PKG" -v "$DKMS_VER" -k "$KVERSION"
else
    echo "dkms not found — using direct build"
    make -C /lib/modules/"$KVERSION"/build M="$PWD" modules
    echo "Installing to $MODDIR"
    mkdir -p "$MODDIR"
    cp -v acer_ec_core.ko acer_fanctl.ko "$MODDIR/"
    if [ -f acer_ec_debug.ko ]; then
        cp -v acer_ec_debug.ko "$MODDIR/"
    fi
    if [ -f acer_wmi_extras.ko ]; then
        cp -v acer_wmi_extras.ko "$MODDIR/"
    fi
    depmod -a
fi

# ---- 4. Modprobe config ----
# Default EC profile 4=gaming: identical to the other profiles at idle,
# maximum fan headroom under sustained load (validated on A715-79G).
cat > "$PROBE_D/acer-ec.conf" <<'CONF'
# acer-ec — module load order + default profile
softdep acer_fanctl pre: acer_ec_core
options acer_fanctl profile_param=4
CONF
echo "Wrote $PROBE_D/acer-ec.conf"

# ---- 5. Modules-load config ----
cat > "$LOAD_D/acer-ec.conf" <<'CONF'
# Load acer EC modules at boot
acer_ec_core
acer_fanctl
acer_wmi_extras
CONF
echo "Wrote $LOAD_D/acer-ec.conf"

# ---- 5c. lm_sensors config ----
mkdir -p /etc/sensors.d
cat > /etc/sensors.d/acer-ec.conf <<'CONF'
chip "acer_ec-*"
    label temp1 "EC Temp"
    label fan1 "CPU Fan"
    label fan2 "GPU Fan"
CONF
echo "Wrote /etc/sensors.d/acer-ec.conf"

# ---- 5d. CLI ----
# Keep /usr/local/bin in sync with the repo script on every install.
install -m 0755 src/acer-ec.sh /usr/local/bin/acer-ec
echo "Installed /usr/local/bin/acer-ec"

# ---- 6. Load ----
# Unload first so a reinstall actually picks up the new build
# (modprobe alone is a no-op when the old module is already loaded).
# Each rmmod is guarded: under set -e a single busy-module failure must
# not abort the installer and strand the machine without fan control —
# the modprobe lines below must always run.
for m in acer_wmi_extras acer_fanctl acer_ec_debug acer_ec_core; do
    if lsmod | grep -q "^$m"; then
        echo "Unloading old $m..."
        rmmod "$m" || echo "WARNING: could not unload $m, continuing anyway"
    fi
done
echo "Loading modules..."
modprobe acer_ec_core
modprobe acer_fanctl
# extras provides the camera key + WMI event log; reload it too, since the
# unload loop above removes it (modules-load.d only applies at boot).
modprobe acer_wmi_extras

echo "=== Done ==="
echo "Status:"
cat /sys/kernel/acer_fanctl/all 2>/dev/null || echo "(no sysfs — check dmesg)"
