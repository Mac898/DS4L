#!/usr/bin/env bash
# Restore the stock in-kernel hid_playstation driver.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
rmmod hid_playstation 2>/dev/null || true
sleep 0.2
modprobe hid_playstation
echo "restored stock hid_playstation (re-pair the controller if it didn't rebind)"
