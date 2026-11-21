#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Load the DS4L module with ds5_drop_all_input=1: every Bluetooth input
# report is dropped, so no gamepad events, but hidraw still works. For inspecting raw
# reports without Steam Input reacting. Run with sudo.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
KO="$HERE/hid-playstation.ko"
[ -f "$KO" ] || { echo "build first: make"; exit 1; }

modprobe -r hid_playstation 2>/dev/null || rmmod hid_playstation 2>/dev/null || true
sleep 0.3
# insmod doesn't resolve dependencies.
modprobe ff-memless 2>/dev/null || true
modprobe led-class-multicolor 2>/dev/null || true
insmod "$KO" ds5_drop_all_input=1
sleep 0.6

for d in /sys/bus/hid/devices/*054C*; do
    [ -e "$d" ] && echo "$(basename "$d") -> $(basename "$(readlink -f "$d/driver" 2>/dev/null || echo NONE)")"
done
ls -l /dev/hidraw* 2>/dev/null || true
echo "Re-pair the controller if it didn't bind. Restore stock: sudo $HERE/unload.sh"
