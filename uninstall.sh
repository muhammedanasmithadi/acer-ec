#!/bin/bash
set -euo pipefail

KVERSION=$(uname -r)
MODDIR="/lib/modules/$KVERSION/extra"

echo "=== acer-ec uninstaller ==="

if lsmod | grep -q "^acer_wmi_extras"; then
    echo "Removing acer_wmi_extras..."
    rmmod acer_wmi_extras
fi
if lsmod | grep -q "^acer_fanctl"; then
    echo "Removing acer_fanctl..."
    rmmod acer_fanctl
fi
if lsmod | grep -q "^acer_ec_debug"; then
    echo "Removing acer_ec_debug..."
    rmmod acer_ec_debug
fi
if lsmod | grep -q "^acer_ec_core"; then
    echo "Removing acer_ec_core..."
    rmmod acer_ec_core
fi

echo "Removing module files..."
rm -f "$MODDIR/acer_ec_core.ko" "$MODDIR/acer_fanctl.ko" "$MODDIR/acer_ec_debug.ko" "$MODDIR/acer_wmi_extras.ko"
depmod -a

echo "Removing config files..."
rm -f /etc/modprobe.d/acer-ec.conf
rm -f /etc/modules-load.d/acer-ec.conf
rm -f /etc/sensors.d/acer-ec.conf
rm -f /usr/local/bin/acer-ec

echo "=== Done ==="
