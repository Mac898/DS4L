#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Load the DS4L module in place of the stock one, without installing it.
# Run with sudo.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
KO="$HERE/hid-playstation.ko"
[ -f "$KO" ] || { echo "build first: make"; exit 1; }

modprobe -r hid_playstation 2>/dev/null || rmmod hid_playstation 2>/dev/null || true
sleep 0.3
modprobe ff-memless 2>/dev/null || true
modprobe led-class-multicolor 2>/dev/null || true
insmod "$KO"
sleep 0.6

for d in /sys/bus/hid/devices/*054C*; do
    [ -e "$d" ] && echo "$(basename "$d") -> $(basename "$(readlink -f "$d/driver" 2>/dev/null || echo NONE)")"
done
ls -l /dev/hidraw* 2>/dev/null || true
echo "Restore stock: sudo $HERE/unload.sh"
