#!/usr/bin/env bash
# Install the DS4L hid-playstation fork via DKMS, into updates/dkms. That
# outranks the stock module, so it loads on boot and is rebuilt on kernel
# updates. Run with sudo.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo: sudo $0"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
NAME="hid-playstation-ds5"
VER="0.1"
SRC="/usr/src/${NAME}-${VER}"

# Regenerate the vendored sources if they're missing.
if [ ! -f "$HERE/hid-playstation.c" ] || [ ! -f "$HERE/hid-ids.h" ]; then
    echo "generated source missing; running prepare.sh..."
    ( cd "$HERE" && ./prepare.sh )
fi

# Remove any prior registration so this is idempotent.
if dkms status "${NAME}/${VER}" 2>/dev/null | grep -q "${NAME}"; then
    echo "removing existing dkms registration ${NAME}/${VER}..."
    dkms remove "${NAME}/${VER}" --all || true
fi

echo "staging clean source into ${SRC}..."
rm -rf "$SRC"
mkdir -p "$SRC"
for f in dkms.conf Makefile ds4l.patch prepare.sh load.sh load-debug.sh unload.sh \
         README.md .gitignore hid-playstation.c hid-ids.h; do
    [ -f "$HERE/$f" ] && cp "$HERE/$f" "$SRC/"
done

echo "=== dkms add ===";     dkms add     -m "$NAME" -v "$VER"
echo "=== dkms build ===";   dkms build   -m "$NAME" -v "$VER"
echo "=== dkms install ==="; dkms install -m "$NAME" -v "$VER" --force

echo "=== depmod ===";       depmod -a

echo
echo "=== dkms status ==="
dkms status "${NAME}/${VER}"
echo
echo "=== modprobe resolution (should point at updates/dkms, not kernel/) ==="
modinfo -n hid_playstation || true
echo
echo "Installed. To load it without rebooting (unplug the controller first if"
echo "removal is blocked):"
echo "  sudo modprobe -r hid_playstation && sudo modprobe hid_playstation"
echo "Check:  cat /sys/module/hid_playstation/parameters/ds5_drop_all_input"
