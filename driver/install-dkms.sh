#!/usr/bin/env bash
# Install the DS4L hid-playstation fork via DKMS, into updates/dkms. That
# outranks the stock module, so it loads on boot and is rebuilt on kernel
# updates. Run with sudo.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo: sudo $0"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
# Name and version come from dkms.conf only.
NAME="$(sed -n 's/^PACKAGE_NAME="\(.*\)"/\1/p' "$HERE/dkms.conf")"
VER="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$HERE/dkms.conf")"
[ -n "$NAME" ] && [ -n "$VER" ] || { echo "cannot read $HERE/dkms.conf" >&2; exit 1; }
SRC="/usr/src/${NAME}-${VER}"

if [ ! -f "$HERE/hid-playstation.c" ] || [ ! -f "$HERE/hid-ids.h" ]; then
    echo "hid-playstation.c / hid-ids.h missing; run prepare.sh first" >&2
    exit 1
fi

# Remove any prior registration so this is idempotent.
if dkms status "${NAME}/${VER}" 2>/dev/null | grep -q "${NAME}"; then
    echo "removing existing dkms registration ${NAME}/${VER}..."
    dkms remove "${NAME}/${VER}" --all || true
fi

echo "staging source into ${SRC}..."
rm -rf "$SRC"
mkdir -p "$SRC"
for f in dkms.conf Makefile hid-playstation.c hid-ids.h COPYING; do
    cp "$HERE/$f" "$SRC/"
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

# With Secure Boot on, an unsigned module is refused at load time.
if command -v mokutil >/dev/null 2>&1 &&
   mokutil --sb-state 2>/dev/null | grep -q 'SecureBoot enabled'; then
    if [ -z "$(modinfo -F signer hid_playstation 2>/dev/null)" ]; then
        echo
        echo "WARNING: Secure Boot is enabled and the module is not signed, so"
        echo "it will not load. Enroll a DKMS signing key (MOK) and reinstall;"
        echo "see your distro's DKMS / Secure Boot docs."
    fi
fi

echo
echo "Installed. To load it without rebooting (unplug the controller first if"
echo "removal is blocked):"
echo "  sudo modprobe -r hid_playstation && sudo modprobe hid_playstation"
echo "Check:  cat /sys/module/hid_playstation/parameters/ds5_drop_all_input"
