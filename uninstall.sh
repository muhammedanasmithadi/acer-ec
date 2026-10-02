#!/bin/bash
# acer-ec uninstaller — removes modules, DKMS registration, configs, CLI.
# Usage: sudo ./uninstall.sh
set -euo pipefail

if [ $# -gt 0 ]; then
    echo "Usage: sudo ./uninstall.sh (no arguments)" >&2
    exit 2
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root (sudo ./uninstall.sh)" >&2
    exit 1
fi

KVERSION=$(uname -r)
MODDIR="/lib/modules/$KVERSION/extra"

echo "=== acer-ec uninstaller ==="

# Deregister from DKMS first, otherwise AUTOINSTALL rebuilds the modules
# on the next kernel update and they come back after "uninstall".
if [ -f dkms.conf ]; then
    DKMS_PKG=$(sed -n 's/^PACKAGE_NAME="\(.*\)"/\1/p' dkms.conf)
    if command -v dkms &>/dev/null; then
        echo "Removing DKMS registration..."
        dkms status 2>/dev/null \
            | awk -F'[:,/ ]+' -v pkg="$DKMS_PKG" '$1 == pkg && $2 != "" {print $2}' \
            | sort -u \
            | while read -r ver; do
                dkms remove -m "$DKMS_PKG" -v "$ver" --all 2>/dev/null || true
            done
    fi
fi

# NOTE: rmmod, not modprobe -r — modprobe also auto-removes the named
# module's dependencies and dies with "in use" while dependents load.
# Guarded per module so one busy module never aborts the uninstall.
for m in acer_wmi_extras acer_fanctl acer_ec_debug acer_ec_core; do
    if [ -d "/sys/module/$m" ]; then
        echo "Removing $m..."
        rmmod "$m" || echo "WARNING: could not unload $m, continuing anyway"
    fi
done

echo "Removing module files..."
# NOTE: Fedora/DKMS installs compressed (.ko.xz) modules — remove both
# suffixes or the modules survive the uninstall and load on next boot.
rm -f "$MODDIR"/acer_ec_core.ko* "$MODDIR"/acer_fanctl.ko* \
      "$MODDIR"/acer_ec_debug.ko* "$MODDIR"/acer_wmi_extras.ko*
depmod -a

echo "Removing config files..."
rm -f /etc/modprobe.d/acer-ec.conf
rm -f /etc/modprobe.d/acer_fanctl.conf
rm -f /etc/modules-load.d/acer-ec.conf
rm -f /etc/modules-load.d/acer_fanctl.conf
rm -f /etc/sensors.d/acer-ec.conf
rm -f /usr/local/bin/acer-ec
rm -f /usr/local/bin/profile

echo "=== Done ==="
