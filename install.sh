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
    dkms remove -m "$DKMS_PKG" -v "$DKMS_VER" --all 2>/dev/null || true
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
cat > "$PROBE_D/acer-ec.conf" <<'CONF'
# acer-ec — module load order + default profile
softdep acer_fanctl pre: acer_ec_core
options acer_fanctl profile_param=2
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

# ---- 6. Load ----
echo "Loading modules..."
modprobe acer_ec_core
modprobe acer_fanctl

echo "=== Done ==="
echo "Status:"
cat /sys/kernel/acer_fanctl/all 2>/dev/null || echo "(no sysfs — check dmesg)"
