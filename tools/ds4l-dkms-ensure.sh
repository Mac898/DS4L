#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Macauley Lim
#
# ds4l-dkms-ensure.sh — root helper: make sure the DS4L hid-playstation
# fork is built, DKMS-installed and loaded.
#
# Usage: ds4l-dkms-ensure.sh [driver-dir]   (default: ../driver)
#
# If the module is already loaded it does nothing. Otherwise it runs
# install-dkms.sh and reloads hid_playstation. If a controller is bound the
# unload fails and the fork takes effect on the next reconnect.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root (pkexec/sudo)" >&2
    exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
DRIVER="${1:-$HERE/../driver}"

param=/sys/module/hid_playstation/parameters/ds5_drop_all_input
if [ -e "$param" ]; then
    echo "DS4L: module already loaded."
    exit 0
fi

echo "DS4L: installing the DKMS module..."
[ -f "$DRIVER/install-dkms.sh" ] || { echo "driver dir not found: $DRIVER" >&2; exit 1; }
bash "$DRIVER/install-dkms.sh"

echo "DS4L: reloading hid-playstation..."
if ! modprobe -r hid_playstation 2>/dev/null; then
    echo "DS4L: could not unload hid_playstation (a controller is probably"
    echo "DS4L: bound). Reconnect the controllers to pick up the new module."
fi
modprobe hid_playstation || true

if [ -e "$param" ]; then
    echo "DS4L: module active."
    exit 0
fi

echo "DS4L: module still not loaded; see the build log above." >&2
exit 1
